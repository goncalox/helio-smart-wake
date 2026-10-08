"""Run inside the existing HA SSH app; access ESPHome only through loopback."""
import argparse
from contextlib import closing
import http.client
import json
from pathlib import Path
import sys
import time

sys.path.insert(0, str(Path(__file__).parent / 'vendor'))
import websocket


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('operation', choices=('status', 'compile', 'upload'))
    parser.add_argument('--port', required=True, type=int)
    parser.add_argument('--configuration', default='helio-bridge.yaml')
    parser.add_argument('--device')
    parser.add_argument('--log', type=Path, default=Path(__file__).parent / 'device-builder-job.log')
    args = parser.parse_args()
    if not 1 <= args.port <= 65535:
        parser.error('invalid port')
    if Path(args.configuration).name != args.configuration or not args.configuration.endswith('.yaml'):
        parser.error('configuration must be an app YAML filename')
    if args.operation == 'upload' and not args.device:
        parser.error('upload requires --device')
    if args.operation == 'status':
        with closing(http.client.HTTPConnection('127.0.0.1', args.port, timeout=15)) as request:
            request.request('GET', '/ping')
            response=request.getresponse()
            if response.status != 200:
                raise RuntimeError(f'Device Builder returned HTTP {response.status}')
            print(json.dumps(json.loads(response.read())), flush=True)
        return 0
    with closing(websocket.create_connection(
        f'ws://127.0.0.1:{args.port}/{args.operation}', timeout=300, suppress_origin=True,
        http_no_proxy=['127.0.0.1'],
    )) as ws, args.log.open('w') as log:
        args.log.chmod(0o600)
        request={'type':'spawn', 'configuration':args.configuration}
        if args.operation=='upload':request['port']=args.device
        ws.send(json.dumps(request))
        started=time.monotonic();last_progress=started
        print(json.dumps({'operation':args.operation,'status':'started','log':str(args.log)}),flush=True)
        while time.monotonic()-started < 7200:
            raw=ws.recv()
            if not raw:
                raise RuntimeError('Stream closed without an exit result; inspect the app task before retrying')
            frame=json.loads(raw)
            if frame.get('event')=='line':
                line=frame.get('data','');log.write(line);log.flush()
                if time.monotonic()-last_progress >= 30:
                    print(json.dumps({'status':'running','seconds':int(time.monotonic()-started)}),flush=True)
                    last_progress=time.monotonic()
            elif frame.get('event')=='exit':
                code=frame.get('code')
                if not isinstance(code,int):raise RuntimeError('Invalid app exit result')
                print(json.dumps({'operation':args.operation,'exit_code':code,'log':str(args.log)}),flush=True)
                return code
        raise RuntimeError('Job exceeded two hours; inspect the app task before retrying')


if __name__=='__main__':
    try:sys.exit(main())
    except (OSError,RuntimeError,websocket.WebSocketException) as error:
        print(f'Maintenance failed: {error}',file=sys.stderr);sys.exit(1)
