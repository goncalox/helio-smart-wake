"""Exercise explicit durable command transport without HA/network/physical alarms."""
import asyncio,copy,importlib,json,struct,sys,types
from pathlib import Path
from unittest.mock import patch
for name in ('homeassistant','homeassistant.helpers','homeassistant.exceptions','homeassistant.helpers.storage','homeassistant.helpers.dispatcher','aioesphomeapi'):
    sys.modules[name]=types.ModuleType(name)
class Error(Exception):pass
sys.modules['homeassistant.exceptions'].HomeAssistantError=Error
sys.modules['aioesphomeapi'].APIClient=object
class Store:
    def __init__(self,*args):self.writes=[]
    async def async_load(self):return None
    async def async_save(self,data):self.writes.append(copy.deepcopy(data))
sys.modules['homeassistant.helpers.storage'].Store=Store
sys.modules['homeassistant.helpers.dispatcher'].async_dispatcher_send=lambda *args:None
root=Path(__file__).resolve().parents[1]
package=types.ModuleType('helio_smart_wake');package.__path__=[str(root/'homeassistant/custom_components/helio_smart_wake')]
sys.modules['helio_smart_wake']=package
module=importlib.import_module('helio_smart_wake.controller');Controller=module.Controller
class Hass:
    config=types.SimpleNamespace(path=lambda *args:str(root/'.test-build'/Path(*args)))
    bus=types.SimpleNamespace(async_fire=lambda *args:None)
class Entry:entry_id='test';data={}
NOW=1800000000

def controller():
    c=Controller(Hass(),Entry());c.connected=True
    c.saved={'active':True,'manual':0,'automation_transport':True,'verified_epoch':0,'checkpoint':'fixed'}
    c.transport={'owner':1,'id':0,'status':0,'manual':0,'idle':1}
    c.state.update(pending=1,epoch=NOW+600,previous=NOW+900,cancel=0,follow=0)
    c.calls=[];c.archives=[]
    async def engine(command):
        c.calls.append(('engine',command))
        if command.startswith('OBSERVE'):c.state['pending']=0
        return {'ok':True,'checkpoint':'fixed'}
    async def action(name,data):
        assert c.store.writes[-1]['command']['command_id']==data['command_id']
        c.calls.append(('action',name,copy.deepcopy(data)))
    async def archive(name,data):c.archives.append((name,data))
    async def audit(*args,**kwargs):pass
    c.engine,c.action,c.archive,c.audit=engine,action,archive,audit
    return c

async def main():
    # Even wake-ready/model-created legacy state cannot dispatch without an instruction.
    c=controller();await c.tick();await c.reconcile(NOW)
    assert not any(x[0]=='action' for x in c.calls)
    assert any(x[1].startswith('OBSERVE') for x in c.calls)
    response=await c.request_alarm(NOW+600,'night-target',{'routine_state':{'n':NOW-1000}})
    sent=[x[2] for x in c.calls if x[0]=='action'][-1]
    assert response['command_id']==1 and sent['epoch']==NOW+600
    actions=len([x for x in c.calls if x[0]=='action'])
    await c.request_alarm(NOW+600,'night-target')
    assert len([x for x in c.calls if x[0]=='action'])==actions
    await c.reconcile(NOW+11);assert c.calls[-1][2]['command_id']==1
    try:await c.request_alarm(NOW+660,'night-target')
    except Error:pass
    else:raise AssertionError('Conflicting request ID accepted')
    # Failed saves block every send; an explicit repeated instruction recovers safely.
    disk=controller();original=disk.store.async_save
    async def fail(data):raise OSError('disk full')
    disk.store.async_save=fail
    for _ in range(2):
        try:await disk.request_alarm(NOW+600,'disk-test')
        except OSError:pass
        else:raise AssertionError('Unsaved instruction proceeded')
    assert not any(x[0]=='action' for x in disk.calls)
    disk.store.async_save=original;await disk.reconcile(NOW+2)
    assert any(x[0]=='action' for x in disk.calls)
    # Recovery adopts verified readback without another write.
    reboot=controller();reboot.saved=copy.deepcopy(c.saved)
    reboot.transport.update(id=1,status=2,epoch=NOW+600)
    await reboot.reconcile(NOW+12)
    assert reboot.saved['command'] is None and reboot.saved['verified_epoch']==NOW+600
    assert not any(x[0]=='action' for x in reboot.calls)
    await reboot.request_alarm(NOW+600,'night-target')
    assert not any(x[0]=='action' for x in reboot.calls)
    c.transport.update(id=1,status=2,epoch=NOW+660)
    try:await c.reconcile(NOW+12)
    except Error:pass
    else:raise AssertionError('Wrong verified epoch accepted')
    c.transport.update(id=0,status=0);await c.reconcile(NOW+101)
    assert c.saved['command'] is None and c.saved['last_instruction']['result']=='unverified'
    # Expired writes do not invent a replacement; only a fresh explicit call can renew.
    before=len(c.calls);await c.reconcile(NOW+102);assert len(c.calls)==before
    c=controller();await c.request_alarm(NOW+600,'manual-test');c.transport['manual']=1
    await c.reconcile(NOW+1)
    assert c.saved['command'] is None and c.saved['last_instruction']['result']=='manual'
    c=controller();c.saved['verified_epoch']=NOW+900
    await c.request_alarm(0,'cancel-test',cancel=True)
    assert c.saved['command']['cancel'] and c.saved['command']['previous']==NOW+900
    for bad in (NOW-60,NOW+20,NOW+61,NOW+86400):
        c=controller()
        try:await c.request_alarm(bad,'bad-time')
        except Error:pass
        else:raise AssertionError('Unsafe dated minute accepted')
    c=controller();await c.persist();before=len(c.store.writes);await c.persist();assert len(c.store.writes)==before
    before=len(c.archives);c.state['candidate']=7;await c.persist();assert len(c.archives)==before+1
    await c.persist();assert len(c.archives)==before+1
    c=controller();c.saved['imported_at']=1700000000
    raw=struct.pack('<BIIH',1,1700000100,1000,3)+b'abc'
    await c.ingest(raw);await c.ingest(raw);assert len([x for x in c.calls if x[1].startswith('SLEEP')])==1
    bad=struct.pack('<BIIH',1,4000000000,1000,3)+b'abc'
    try:await c.ingest(bad)
    except ValueError:pass
    else:raise AssertionError('Future record accepted')
    c=controller();started=asyncio.Event();finish=asyncio.Event();ticks=[]
    async def transfer(seq):started.set();await finish.wait()
    async def tick():ticks.append(True)
    c._transfer,c.tick=transfer,tick
    download=asyncio.create_task(c.transfer(5));await started.wait()
    await asyncio.wait_for(c.control_once(),0.1);assert ticks;finish.set();await download
    print('HA transport: explicit instructions only, durable/idempotent commands, recovery, expiry, manual edits, cancellation, replay and data/control independence passed')
with patch.object(module.time,'time',return_value=NOW):asyncio.run(main())
