"""Maintain HA ESPHome using an existing SSH login and pinned server identity.

No browser session, TCP forwarding, public app port or Mac ESPHome runtime needed.
The companion client runs locally inside the existing Home Assistant SSH app.
"""
import argparse
import json
from pathlib import Path
import shlex
import sys
import paramiko


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('operation',choices=('status','compile','upload'))
    parser.add_argument('--ssh-config',required=True,type=Path)
    parser.add_argument('--known-hosts',required=True,type=Path)
    parser.add_argument('--port',required=True,type=int,help='ESPHome app ingress_port from HA app information')
    parser.add_argument('--configuration',default='helio-bridge.yaml')
    parser.add_argument('--device',help='ESP32 network address; required for explicit upload')
    parser.add_argument('--remote-client',default='/config/helio_maintenance/device_builder_local.py')
    args=parser.parse_args()
    if args.operation=='upload' and not args.device:parser.error('upload requires --device')
    config=json.loads(args.ssh_config.read_text());config['hostname']=config.pop('host')
    with paramiko.SSHClient() as client:
        client.load_host_keys(str(args.known_hosts))
        client.set_missing_host_key_policy(paramiko.RejectPolicy())
        client.connect(**config,timeout=15)
        client.get_transport().set_keepalive(30)
        command=['python3',args.remote_client,args.operation,'--port',str(args.port),'--configuration',args.configuration]
        if args.device:command+=['--device',args.device]
        _,out,err=client.exec_command(shlex.join(command),timeout=7500)
        for line in out:print(line,end='',flush=True)
        errors=err.read().decode()
        if errors:print(errors,end='',file=sys.stderr)
        code=out.channel.recv_exit_status()
        if code:print('Job not confirmed successful; inspect the app task/log before retrying',file=sys.stderr)
        return code


if __name__=='__main__':
    try:sys.exit(main())
    except (OSError,RuntimeError,paramiko.SSHException) as error:
        print(f'Maintenance failed: {error}',file=sys.stderr);sys.exit(1)
