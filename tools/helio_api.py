import argparse
import asyncio
from pathlib import Path
import yaml
from aioesphomeapi import APIClient

root = Path('.')

async def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('operation', choices=['inspect', 'test', 'sleep', 'set', 'cancel', 'smart-on', 'smart-off'])
    parser.add_argument('--hour', type=int)
    parser.add_argument('--minute', type=int)
    parser.add_argument('--repeat', type=int, default=0)
    parser.add_argument('--wait', type=int, default=10)
    parser.add_argument('--config', type=Path, default=Path('helio-bridge.yaml'))
    parser.add_argument('--host', default='helio-bridge.local')
    args = parser.parse_args()
    secrets = yaml.safe_load((args.config.parent / 'secrets.yaml').read_text())
    class Loader(yaml.SafeLoader): pass
    Loader.add_constructor('!secret', lambda loader, node: secrets[loader.construct_scalar(node)])
    Loader.add_constructor('!include', lambda loader, node: loader.construct_scalar(node))
    config = yaml.load(args.config.read_text(), Loader=Loader)
    if args.operation == 'set' and (args.hour is None or args.minute is None):
        parser.error('Set requires hour and minute.')
    client = APIClient(args.host, 6053, noise_psk=config['api']['encryption']['key'], client_info='Helio setup')
    await client.connect(login=True)
    try:
        entities, services = await client.list_entities_services()
        names = {entity.key: entity.name for entity in entities}
        for entity in entities:
            print('Entity:', entity.name, type(entity).__name__, flush=True)
        for service in services:
            print('Action:', service.name, [arg.name for arg in service.args], flush=True)
        def state(item):
            name = names.get(item.key, '')
            if name.startswith('Helio'):
                print('State:', name, getattr(item, 'state', item), flush=True)
        client.subscribe_states(state)
        await asyncio.sleep(1)
        if args.operation == 'test':
            button = next(entity for entity in entities if entity.name == 'Test Helio Connection')
            client.button_command(button.key)
        elif args.operation == 'sleep':
            button = next(entity for entity in entities if entity.name == 'Read Helio Sleep Data')
            client.button_command(button.key)
        elif args.operation in ('smart-on', 'smart-off'):
            switch = next(entity for entity in entities if entity.name == 'Helio Smart Wake')
            client.switch_command(switch.key, args.operation == 'smart-on')
        elif args.operation in ('set', 'cancel'):
            name = 'helio_set_alarm' if args.operation == 'set' else 'helio_cancel_alarm'
            service = next(service for service in services if service.name == name)
            data = {'hour': args.hour, 'minute': args.minute, 'repeat_mask': args.repeat} if args.operation == 'set' else {}
            if args.operation == 'set':
                time_entity = next(entity for entity in entities if entity.name == 'Helio Alarm Time')
                client.time_command(time_entity.key, args.hour, args.minute, 0)
                repeat = {0: 'Once', 127: 'Every day', 31: 'Weekdays', 96: 'Weekends'}.get(args.repeat)
                if repeat:
                    select = next(entity for entity in entities if entity.name == 'Helio Alarm Repeat')
                    client.select_command(select.key, repeat)
            await client.execute_service(service, data)
        await asyncio.sleep(args.wait)
    finally:
        await client.disconnect()

asyncio.run(main())
