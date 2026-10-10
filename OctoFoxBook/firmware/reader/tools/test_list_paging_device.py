"""Verify page-sized Library navigation on the connected reader, without writes."""
import argparse
import re

from provision_reader import open_connection, transact, resume_sync


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--port', default='COM3')
    args = parser.parse_args()
    with open_connection(args.port) as connection:
        def step(command):
            return transact(connection, command, 'LIBRARY OPEN COMPLETE', command, seconds=25)

        def coordinates(lines):
            line = next(line for line in lines if line.startswith('LIBRARY READY '))
            return {key: int(value) for key, value in re.findall(r'(rows|selected|first)=(\d+)', line)}

        try:
            lines = step('LIBRARY OPEN')
            state = coordinates(lines)
            # Library opens the section list; position on its first row.
            while state['selected'] > 1:
                state = coordinates(step('INPUT UP SHORT'))
            assert state['first'] == 0 and state['rows'] > 4, state
            transitions = []
            previous = 0
            for selected in range(2, state['rows'] + 1):
                lines = step('INPUT DOWN SHORT')
                state = coordinates(lines)
                expected = ((selected - 1) // 4) * 4
                assert state['selected'] == selected and state['first'] == expected, state
                if expected != previous:
                    assert any('CLEAN SCHEDULED reason=list-page' in line for line in lines)
                    transitions.append((previous, expected))
                previous = expected
            for selected in range(state['rows'] - 1, 0, -1):
                lines = step('INPUT UP SHORT')
                state = coordinates(lines)
                expected = ((selected - 1) // 4) * 4
                assert state['selected'] == selected and state['first'] == expected, state
                if expected != previous:
                    assert any('CLEAN SCHEDULED reason=list-page' in line for line in lines)
                    transitions.append((previous, expected))
                previous = expected
            assert (0, 4) in transitions and (4, 0) in transitions, transitions
            print('LIST_PAGING_ACCEPTED transitions=' + repr(transitions))
        finally:
            resume_sync(connection)


if __name__ == '__main__':
    main()
