"""Real-device regression: Sleep switch preference survives a cold restart.

Run with the momentary Sleep button released. Restores momentary mode on exit.
"""
import argparse
import time

from provision_reader import open_connection, open_with_retry, transact, resume_sync


def command(connection, text, marker):
    return transact(connection, text, marker, text, seconds=25)


def set_mode(connection, latching):
    status = command(connection, 'POWER STATUS', 'POWER STATUS')[-1]
    if ('switch=latching' in status) == latching:
        return
    command(connection, 'SETTINGS OPEN', 'WIFI UI page=0')
    for _ in range(3):
        command(connection, 'INPUT DOWN SHORT', 'WIFI UI')
    command(connection, 'INPUT CENTER SHORT', 'WIFI UI page=8')
    for _ in range(2):
        command(connection, 'INPUT DOWN SHORT', 'WIFI UI')
    command(connection, 'INPUT CENTER SHORT', 'WIFI UI')
    command(connection, 'SETTINGS CLOSE', 'HOME OPEN COMPLETE')


def restart(connection, port):
    command(connection, 'SYSTEM RESTART CONFIRM', 'SYSTEM RESTART')
    connection.close()
    time.sleep(2)
    connection = open_with_retry(port, seconds=20)
    command(connection, 'PING', 'PONG')
    return connection


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--port', default='COM3')
    args = parser.parse_args()
    connection = open_connection(args.port)
    try:
        command(connection, 'HOME OPEN', 'HOME OPEN COMPLETE')
        status = command(connection, 'POWER STATUS', 'POWER STATUS')[-1]
        if 'level=0' in status:
            raise RuntimeError('Release the Sleep button before testing')
        set_mode(connection, True)
        connection = restart(connection, args.port)
        assert 'switch=latching' in command(connection, 'POWER STATUS', 'POWER STATUS')[-1]
    finally:
        if not connection.is_open:
            connection = open_with_retry(args.port, seconds=20)
        try:
            set_mode(connection, False)
            connection = restart(connection, args.port)
            assert 'switch=momentary' in command(connection, 'POWER STATUS', 'POWER STATUS')[-1]
            resume_sync(connection)
        finally:
            connection.close()
    print('SLEEP_SETTINGS_ACCEPTED latching_persisted=true momentary_persisted=true')


if __name__ == '__main__':
    main()
