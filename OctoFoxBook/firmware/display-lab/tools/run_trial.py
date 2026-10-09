"""Run one explicit, bounded display trial over native USB CDC (pyserial).

Never flashes firmware. Leaves the final target visible for physical inspection.
"""
import argparse
import json
from pathlib import Path
import statistics
import time

import serial


def transact(port, command, prefix):
    started = time.monotonic()
    port.write((command + '\n').encode('ascii'))
    port.flush()
    while time.monotonic() - started < 12:
        line = port.readline().decode('utf8', errors='replace').strip()
        if line.startswith('LAB ERROR'):
            raise RuntimeError(line)
        if line.startswith(prefix):
            fields = dict(part.split('=', 1) for part in line.split() if '=' in part)
            fields['host_roundtrip_ms'] = round((time.monotonic() - started) * 1000, 2)
            print(line, flush=True)
            return fields
    raise TimeoutError(f'No {prefix} response to {command}')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--port', default='COM3')
    parser.add_argument('--quality', choices=['HIGH', 'NORMAL', 'FAST'], default='HIGH')
    parser.add_argument('--cleanup', choices=['NONE', 'HARD', 'SOFT', 'LOCAL'], default='HARD')
    parser.add_argument('--every', type=int, default=6)
    parser.add_argument('--scene', choices=['TEXT', 'MENU', 'COVER', 'LIBRARY', 'MIXED'], default='TEXT')
    parser.add_argument('--steps', type=int, default=11)
    parser.add_argument('--delay', type=float, default=2.0)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    if not 1 <= args.steps <= 48 or not 0.5 <= args.delay <= 30:
        parser.error('steps must be 1..48 and delay 0.5..30 seconds')
    if not 0 <= args.every <= 24 or (args.cleanup == 'NONE') != (args.every == 0):
        parser.error('NONE requires every=0; other cleanup modes require 1..24')
    if args.output.exists():
        parser.error('output exists; choose another result filename')
    result = {'profile': {k: v for k, v in vars(args).items() if k != 'output'},
              'frames': [], 'physical_quality': 'not-assessed', 'complete': False}
    args.output.parent.mkdir(parents=True, exist_ok=True)
    port = serial.Serial(port=None, baudrate=115200, timeout=0.2, write_timeout=3)
    port.port = args.port
    port.dtr = True
    port.rts = False
    try:
        port.open()
        port.reset_input_buffer()
        status = transact(port, 'LAB STATUS', 'LAB STATUS ')
        result['device'] = status
        if status.get('ready') != '1' or status.get('fault') != '0':
            raise RuntimeError('Display lab is not ready')
        if args.scene in ('COVER', 'LIBRARY', 'MIXED') and int(status.get('fixtures', 1)) < 2:
            raise RuntimeError('Cover scenes require display lab fixture version 2')
        if status.get('backend') == 'lilygo' and args.cleanup != 'NONE':
            raise RuntimeError('LilyGo clears on every frame; use NONE --every 0')
        start = f'LAB START {args.quality} {args.cleanup} {args.every} {args.scene}'
        result['frames'].append(transact(port, start, 'LAB FRAME '))
        for step in range(1, args.steps + 1):
            time.sleep(args.delay)
            frame = transact(port, 'LAB STEP', 'LAB FRAME ')
            result['frames'].append(frame)
            if int(frame['step']) != step or frame.get('ok') != '1':
                raise RuntimeError('Unexpected frame sequence')
        transact(port, 'LAB STOP', 'LAB STOPPED')
        frames = result['frames'][1:]  # exclude reset-to-white + first target
        times = [int(f['total_us']) / 1000 for f in frames]
        plain = [int(f['total_us']) / 1000 for f in frames if f['cleanup'] == 'NONE']
        result['summary'] = {
            'transitions': len(frames), 'median_ms': statistics.median(times),
            'max_ms': max(times),
            'no_cleanup_median_ms': statistics.median(plain) if plain else None,
            'full_area_cleans': sum(f['cleanup'] not in ('NONE', 'LOCAL') for f in frames),
            'local_cleans': sum(f['cleanup'] == 'LOCAL' for f in frames),
        }
        result['complete'] = True
        print(json.dumps(result['summary']), flush=True)
    except Exception as error:
        result['error'] = str(error)
        raise
    finally:
        port.close()
        args.output.write_text(json.dumps(result, indent=2) + '\n', encoding='utf8')


if __name__ == '__main__':
    main()
