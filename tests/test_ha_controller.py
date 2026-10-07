"""Exercise durable dispatch and replay without HA, network or physical alarms."""
import asyncio
import copy
import importlib
import json
from pathlib import Path
import struct
import sys
import types

for name in ('homeassistant', 'homeassistant.helpers', 'homeassistant.exceptions',
             'homeassistant.helpers.storage', 'homeassistant.helpers.dispatcher', 'aioesphomeapi'):
    sys.modules[name] = types.ModuleType(name)
class Error(Exception): pass
sys.modules['homeassistant.exceptions'].HomeAssistantError = Error
sys.modules['aioesphomeapi'].APIClient = object
class Store:
    def __init__(self, *args): self.writes = []
    async def async_load(self): return None
    async def async_save(self, data): self.writes.append(copy.deepcopy(data))
sys.modules['homeassistant.helpers.storage'].Store = Store
sys.modules['homeassistant.helpers.dispatcher'].async_dispatcher_send = lambda *args: None
root = Path(__file__).resolve().parents[1]
package = types.ModuleType('helio_smart_wake')
package.__path__ = [str(root/'homeassistant/custom_components/helio_smart_wake')]
sys.modules['helio_smart_wake'] = package
Controller = importlib.import_module('helio_smart_wake.controller').Controller
class Hass:
    config = types.SimpleNamespace(path=lambda *args: str(root/'.test-build'/Path(*args)))
class Entry: entry_id='test'; data={}

def controller():
    c=Controller(Hass(), Entry())
    c.saved={'active':True, 'manual':0}
    c.transport={'owner':1, 'id':0, 'status':0, 'manual':0, 'idle':1}
    c.state.update(pending=1, epoch=1800000600, previous=1800000900, cancel=0, follow=0)
    c.calls=[]
    c.archives=[]
    async def archive(name,data):c.archives.append((name,data))
    c.archive=archive
    async def engine(command):
        c.calls.append(('engine', command))
        if command.startswith(('ACK', 'MANUAL')): c.state['pending']=0
        return {'ok':True, 'checkpoint':'fixed-checkpoint'}
    async def action(name,data):
        # A durable command must already exist before a network write.
        assert c.store.writes[-1]['command']['command_id']==data['command_id']
        c.calls.append(('action', name, copy.deepcopy(data)))
    async def audit(*args,**kwargs): pass
    c.engine, c.action, c.audit=engine,action,audit
    return c

async def main():
    c=controller();c.saved['active']=False
    await c.reconcile(1800000000);assert not c.calls and not c.store.writes
    c.saved['active']=True
    await c.reconcile(1800000000)
    assert c.saved['command']['command_id']==1 and any(x[0]=='action' for x in c.calls)
    sent=c.calls[-1][2]
    await c.reconcile(1800000001)
    assert sent['epoch']==1800000600
    await c.reconcile(1800000011);assert c.calls[-1][2]['command_id']==sent['command_id']
    # A failed durable save blocks every network retry, including reconnects in the same process.
    disk=controller();original=disk.store.async_save
    async def fail(data): raise OSError('disk full')
    disk.store.async_save=fail
    for moment in (1800000000,1800000001):
        try:await disk.reconcile(moment)
        except OSError:pass
        else:raise AssertionError('Unsaved intent proceeded')
    assert not any(x[0]=='action' for x in disk.calls)
    disk.store.async_save=original;await disk.reconcile(1800000002)
    assert any(x[0]=='action' for x in disk.calls)
    # A reboot after a write adopts verified evidence; it does not send another alarm.
    reboot=controller();reboot.saved=copy.deepcopy(c.saved)
    reboot.transport.update(id=1,status=2,epoch=1800000600)
    await reboot.reconcile(1800000012)
    assert reboot.saved['command'] is None and ('engine','ACK 1800000012 1') in reboot.calls
    assert not any(x[0]=='action' for x in reboot.calls)
    # Wrong-epoch replies cannot confirm a command.
    c.transport.update(id=1,status=2,epoch=1800000660)
    try:await c.reconcile(1800000012)
    except Error:pass
    else:raise AssertionError('Wrong epoch accepted')
    c.transport.update(id=0,status=0)
    await c.reconcile(1800000101)
    assert ('engine','ACK 1800000101 0') in c.calls and c.saved['command'] is None
    # Manual controls pause the current night and cancel any controller retry intent.
    c=controller();await c.reconcile(1800000000);c.transport['manual']=1
    await c.reconcile(1800000001)
    assert c.saved['command'] is None and ('engine','MANUAL 1800000001') in c.calls
    # Polling unchanged state does not repeatedly write a large model checkpoint.
    before=len(c.store.writes);await c.persist();await c.persist()
    assert len(c.store.writes)==before
    # A changed candidate/validation stage retains coefficients and training context.
    before=len(c.archives);c.state['candidate']=7;await c.persist()
    assert len(c.archives)==before+1 and json.loads(c.archives[-1][1])['versions']['candidate']==7
    await c.persist();assert len(c.archives)==before+1
    # Duplicate pending/committed events are ingested once; future timestamps fail closed.
    c=controller();c.saved['imported_at']=1700000000
    raw=struct.pack('<BIIH',1,1700000100,1000,3)+b'abc'
    await c.ingest(raw);await c.ingest(raw)
    assert len([x for x in c.calls if x[1].startswith('SLEEP')])==1
    bad=struct.pack('<BIIH',1,4000000000,1000,3)+b'abc'
    try:await c.ingest(bad)
    except ValueError:pass
    else:raise AssertionError('Future record accepted')
    # A slow journal transfer does not block live control or its decision lock.
    c=controller();c.connected=True;c.saved['checkpoint']='fixed'
    started=asyncio.Event();finish=asyncio.Event();ticks=[]
    async def transfer(seq):
        started.set();await finish.wait()
    async def tick():ticks.append(True)
    c._transfer=transfer;c.tick=tick
    download=asyncio.create_task(c.transfer(5));await started.wait()
    await asyncio.wait_for(c.control_once(),0.1);assert ticks
    finish.set();await download
    print('HA controller: persist-before-send, idempotent retry, reboot/lost ACK, wrong reply, expiration, manual override, write coalescing and replay passed')

asyncio.run(main())
