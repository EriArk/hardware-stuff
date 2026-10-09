"""Real-device deep-sleep/timer regression; GPIO wake still needs a physical press."""
import argparse
import json
import time
from provision_reader import open_connection, open_with_retry, transact, resume_sync


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--port', default='COM3')
    args = parser.parse_args()
    with open_connection(args.port) as connection:
        transact(connection, 'HOME OPEN', 'HOME OPEN COMPLETE', 'home', seconds=20)
        status = transact(connection, 'POWER STATUS', 'POWER STATUS', 'power')[-1]
        if 'switch=momentary' in status and 'level=0' in status:
            raise RuntimeError('Release the Sleep button before the timer test')
        transact(connection, 'POWER TEST SLEEP 3', 'POWER DEEP_SLEEP', 'sleep', seconds=20)
    time.sleep(3.5)
    with open_with_retry(args.port, seconds=15) as connection:
        try:
            transact(connection, 'PING', 'PONG', 'wake', seconds=20)
            lines = transact(connection, 'DIAG STATUS', 'DIAG ', 'diagnostics')
            diagnostic = json.loads(next(line[5:] for line in lines if line.startswith('DIAG ')))
            assert diagnostic['wake_cause'] == 4, diagnostic  # ESP_SLEEP_WAKEUP_TIMER
            assert diagnostic['sd_mounted'] and diagnostic['psram_test']
            power = transact(connection, 'POWER STATUS', 'POWER STATUS', 'context')[-1]
            assert 'context_valid=true' in power
            home = transact(connection, 'HOME STATUS', 'HOME STATUS', 'restored-home')[-1]
            assert 'active=true' in home
        finally:
            resume_sync(connection)
    print('POWER_SLEEP_ACCEPTED wake=timer context_valid=true home_restored=true')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
