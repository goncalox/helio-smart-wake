"""Read committed onboard logs via the encrypted ESPHome API; no BLE/alarm commands."""
import argparse
import asyncio
import base64
from datetime import datetime, timezone
import json
import os
from pathlib import Path
import re
import struct
import zlib
from sleep_decode import decode_record, ANSI
from activity_decode import decode_activity

MAX_RAW = 24576

def decode_blob(blob, sequence):
    magic, seq, length, crc = struct.unpack_from('<4sIII', blob)
    if magic != b'HLG2' or seq != sequence or length > MAX_RAW:
        raise ValueError('invalid batch header')
    raw = bytearray()
    i = 16
    while i < len(blob):
        b = blob[i]; i += 1
        if b: raw.append(b)
        else:
            if i == len(blob) or not blob[i]: raise ValueError('invalid zero run')
            raw.extend(b'\0' * blob[i]); i += 1
        if len(raw) > length: raise ValueError('oversized decompression')
    if len(raw) != length or zlib.crc32(raw) != crc: raise ValueError('batch checksum/length mismatch')
    events = []; i = 0
    while i < length:
        kind, timestamp, uptime, size = struct.unpack_from('<BIIH', raw, i); i += 11
        payload = raw[i:i+size]; i += size
        if len(payload) != size: raise ValueError('truncated event')
        item = dict(batch=seq, kind=kind, device_time=timestamp, uptime_ms=uptime)
        if timestamp: item['utc'] = datetime.fromtimestamp(timestamp, timezone.utc).isoformat()
        if kind == 1:
            if size % 594: raise ValueError('partial sleep record')
            item.update(kind='snapshot', raw_base64=base64.b64encode(payload).decode(),
                        records=[decode_record(payload[p:p+594]) for p in range(0, size, 594)])
        elif kind in (11,17):
            if (kind==11 and (size!=44 or payload[0]!=1)) or (kind==17 and (size!=48 or payload[0]!=2)): raise ValueError('unknown model observation layout')
            values = struct.unpack_from('<9f', payload, 8)
            item.update(kind='model_prediction', model_version='20261003-v1' if kind==11 else 'adaptive-'+str(struct.unpack_from('<I',payload,44)[0]), valid=bool(payload[1]),
                        stage={4:'Light',5:'Deep',8:'REM',7:'Awake'}.get(payload[2], 'Unknown'),
                        reason_code=payload[3], sample_time=struct.unpack_from('<I',payload,4)[0],
                        features=dict(zip(('heart_rate','hr_mean_5m','hr_sd_5m','intensity_mean_5m','steps_5m'),values[:5])),
                        scores=dict(zip(('Light','Deep','REM','Awake'),values[5:])))
        elif kind == 12:
            if size != 24 or payload[0] not in (1,2): raise ValueError('unknown early gate layout')
            fields=struct.unpack_from('<5I',payload,4); flags=payload[3]
            names=('band_through','model_sample_time','model_read_at','target','desired')
            item.update(kind='early_gate',strap_stage={4:'Light',5:'Deep',8:'REM',7:'Awake'}.get(payload[1],'Unknown'),
                        model_stage={4:'Light',5:'Deep',8:'REM',7:'Awake'}.get(payload[2],'Unknown'),
                        band_fresh=bool(flags&1),band_read_fresh=bool(flags&2),model_valid=bool(flags&4),
                        model_wake_ready=bool(flags&8),earlier_alarm_selected=bool(flags&16),
                        gate_policy='both-Light' if payload[0]==1 else 'both-Light-or-Awake',
                        **dict(zip(names,fields)))
            if payload[0]==1: item['model_light_ready']=bool(flags&8)
        elif kind == 19:
            if size not in (108,176) or (size == 108 and struct.unpack_from('<I',payload)[0] != 1) or (size == 176 and struct.unpack_from('<I',payload)[0] != 2):
                raise ValueError('unknown personal sleep score layout')
            names=('version','onset','end','observed','changed','first_at','signature','baseline_nights',
                   'target_minutes','available_components','helio_score','first_helio_score')
            score=dict(zip(names,struct.unpack_from('<12I',payload)))
            names=('score','first_score','component_coverage','activity_coverage','mean_hr','mean_movement',
                   'duration','continuity','timing','heart_rate','movement')
            score.update(zip(names,struct.unpack_from('<11f',payload,48)))
            score.update(zip(('asleep_minutes','awake_minutes','awakening_bouts','activity_minutes','hr_minutes','local_onset'),
                             struct.unpack_from('<6H',payload,92)))
            score['settled']=bool(struct.unpack_from('<I',payload,104)[0])
            for key in ('helio_score','first_helio_score'):
                if score[key]==255: score[key]=None
            if size == 176:
                score.update(zip(('previous_version','previous_first_at'),struct.unpack_from('<2I',payload,108)))
                score.update(zip(('previous_score','previous_first_score','previous_component_coverage','recent_shortfall',
                                  'hr_minute_change','hr_late_minus_early','hr_minute_sd','recent_shortfall_fraction'),
                                 struct.unpack_from('<8f',payload,116)))
                score.update(zip(('longest_awake_minutes','wake_cluster_30m','late_awake_minutes','restless_minutes',
                                  'movement_bursts','longest_movement_burst','hr_adjacent_pairs','recent_history_nights'),
                                 struct.unpack_from('<8H',payload,148)))
                score.update(zip(('pattern_flags','previous_helio','previous_first_helio'),struct.unpack_from('<3I',payload,164)))
                for key in ('previous_helio','previous_first_helio'):
                    if score[key]==255: score[key]=None
            item.update(kind='personal_sleep_score',score=score)
        elif kind == 18:
            if size!=548 or struct.unpack_from('<I',payload)[0]!=1: raise ValueError('unknown adaptive model audit layout')
            names=('format','champion_version','candidate_version','trained_through','checked_through','evaluated_nights','promotions','rejections','candidate_created')
            state=dict(zip(names,struct.unpack_from('<9I',payload)))
            weights=struct.unpack_from('<48d',payload,36);counts=struct.unpack_from('<32I',payload,420)
            state.update(champion_weights=[list(weights[i:i+6]) for i in range(0,24,6)],candidate_weights=[list(weights[i:i+6]) for i in range(24,48,6)],
                         champion_test=[list(counts[i:i+4]) for i in range(0,16,4)],candidate_test=[list(counts[i:i+4]) for i in range(16,32,4)])
            item.update(kind='adaptive_model_state',state=state)
        elif kind == 16:
            if size!=20 or payload[0]!=1: raise ValueError('unknown adaptive vote layout')
            stages={4:'Light',5:'Deep',8:'REM',7:'Awake'}
            fields=struct.unpack_from('<4I',payload,4)
            item.update(kind='adaptive_vote',champion_stage=stages.get(payload[1],'Unknown'),candidate_stage=stages.get(payload[2],'Unknown'),
                        input_valid=bool(payload[3]),sample_time=fields[0],champion_version=fields[1],candidate_version=fields[2],read_at=fields[3])
        elif kind == 13:
            if size != 12 or payload[0] != 1 or payload[1] > 2: raise ValueError('unknown wear observation layout')
            item.update(kind='wear_observation', state=('Unknown','Worn','Removed')[payload[1]],
                        activity_kind=payload[2], heart_rate=payload[3],
                        sample_time=struct.unpack_from('<I',payload,4)[0], read_at=struct.unpack_from('<I',payload,8)[0])
        elif kind == 14:
            if size != 24 or struct.unpack_from('<I',payload)[0] != 1: raise ValueError('unknown follow-up layout')
            names=('version','night_start','primary','confirmed','attempted')
            follow=dict(zip(names,struct.unpack_from('<5I',payload)))
            follow.update(stopped=bool(payload[20]),cancel_pending=bool(payload[21]),uncertain=bool(payload[22]))
            item.update(kind='follow_up',follow_up=follow)
        elif kind == 9:
            item.update(kind="activity", **decode_activity(payload))
        elif kind == 6:
            if size != 36: raise ValueError('unknown smart session layout')
            names = ('version','night_start','deadline','onset','confirmed','attempted')
            session = dict(zip(names, struct.unpack_from('<6I', payload)))
            duration, early, hour, minute = struct.unpack_from('<HHBB', payload, 24)
            session.update(duration_minutes=duration, early_minutes=early,
                finished=payload[30], early_selected=payload[31], manual_override=payload[32],
                cancel_pending=payload[33], awake_minutes=struct.unpack_from('<H',payload,34)[0])
            if session['version'] >= 3:
                session['session_end'] = session.pop('deadline')
                session.update(migration_pending=hour, reserved=minute, hard_deadline=False)
            else:
                session.update(deadline_hour=hour, deadline_minute=minute, hard_deadline=True)
            item.update(kind='smart_session', session=session)
        else:
            item.update(kind={2:'connection',3:'sleep_status',4:'alarm_status',5:'smart_status',7:'error',8:'system',10:'activity_status',15:'learning_status'}.get(kind, str(kind)),
                        message=payload.decode(errors='replace'))
        events.append(item)
    return events

async def download(args):
    import yaml
    from aioesphomeapi import APIClient
    secrets = yaml.safe_load((args.config.parent/'secrets.yaml').read_text())
    class Loader(yaml.SafeLoader): pass
    Loader.add_constructor('!secret', lambda loader,node: secrets[loader.construct_scalar(node)])
    Loader.add_constructor('!include', lambda loader,node: loader.construct_scalar(node))
    config=yaml.load(args.config.read_text(), Loader=Loader)
    out=args.output; out.mkdir(mode=0o700,parents=True,exist_ok=True)
    queue=asyncio.Queue()
    client=APIClient(args.host,6053,noise_psk=config['api']['encryption']['key'],client_info='Helio log download')
    await asyncio.wait_for(client.connect(login=True),30)
    missing=[]; batches=[]
    try:
        _,services=await client.list_entities_services()
        service=next(s for s in services if s.name=='helio_download_diagnostics')
        def log(m):
            s=ANSI.sub('',m.message.decode(errors='replace') if isinstance(m.message,bytes) else str(m.message))
            if 'HLG2 ' in s: queue.put_nowait(s.split('HLG2 ',1)[1])
        client.subscribe_logs(log,log_level=3,dump_config=False)
        await client.execute_service(service, {'sequence':-1})
        while True:
            message=await asyncio.wait_for(queue.get(),15)
            if message.startswith('INDEX'): break
            if message.startswith('ERROR'): raise RuntimeError(message)
        index={k:int(v) for k,v in re.findall(r'(\w+)=(\d+)',message)}
        (out/'index.json').write_text(json.dumps(dict(downloaded_at=datetime.now(timezone.utc).isoformat(),**index),indent=2))
        first=max(index['first'],args.since or index['first'])
        for seq in range(first,index['next']):
            path=out/f'{seq:08d}.bin'
            if path.exists():
                decode_blob(path.read_bytes(),seq); batches.append(seq); continue
            for attempt in range(3):
                data=bytearray(); expected=None
                while not queue.empty(): queue.get_nowait()
                await client.execute_service(service,{'sequence':seq})
                try:
                    while True:
                        msg=await asyncio.wait_for(queue.get(),10)
                        fields=dict(re.findall(r'(\w+)=([^ ]+)',msg))
                        if msg.startswith('ERROR'): raise ValueError(msg)
                        if int(fields.get('seq',-1)) != seq: continue
                        if msg.startswith('MISSING'): raise LookupError('batch overwritten or absent')
                        if msg.startswith('BEGIN'):
                            expected=int(fields['bytes']); data.clear()
                            if not 16 <= expected <= MAX_RAW*2+16: raise ValueError('invalid transfer length')
                        elif msg.startswith('DATA'):
                            if expected is None or int(fields['offset'])!=len(data): raise ValueError('missing chunk')
                            data.extend(bytes.fromhex(fields['hex']))
                            if len(data)>expected: raise ValueError('oversized transfer')
                        elif msg.startswith('END'):
                            if len(data)!=expected: raise ValueError('incomplete transfer')
                            decode_blob(data,seq)
                            temp=path.with_suffix('.tmp'); temp.write_bytes(data); temp.replace(path)
                            batches.append(seq); break
                    break
                except LookupError:
                    missing.append(seq); break
                except (ValueError,asyncio.TimeoutError,struct.error):
                    if attempt==2: missing.append(seq)
        events=[]
        for seq in batches: events.extend(decode_blob((out/f'{seq:08d}.bin').read_bytes(),seq))
        with (out/'events.jsonl').open('w') as f:
            for event in events: f.write(json.dumps(event)+'\n')
        with (out/'snapshots.jsonl').open('w') as f:
            for event in events:
                if event['kind']=='snapshot': f.write(json.dumps(event)+'\n')
        with (out/'activity.jsonl').open('w') as f:
            for event in events:
                if event['kind']=='activity': f.write(json.dumps(event)+'\n')
        summary=dict(activity_snapshots=sum(e['kind']=='activity' for e in events),batches=len(batches),snapshots=sum(e['kind']=='snapshot' for e in events),events=len(events),missing=missing,
                     dropped=index['dropped'],pending_bytes=index['pending'])
        (out/'summary.json').write_text(json.dumps(summary,indent=2))
        print(json.dumps(summary))
        if missing: raise RuntimeError('download incomplete; missing batches listed in summary')
    finally: await client.disconnect()

if __name__=='__main__':
    os.umask(0o077)
    parser=argparse.ArgumentParser()
    parser.add_argument('--config',type=Path,default=Path('/Users/goncalo/esphome/helio-bridge.yaml'))
    parser.add_argument('--host',default='192.168.1.154')
    parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--since',type=int)
    asyncio.run(download(parser.parse_args()))
