"""Disposable UI fixture; never installs runtime packages or starts Docker."""
from pathlib import Path
import sys
import tempfile
import threading
import time
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import installer


class Fixture(installer.Installer):
    def work(self, action, options):
        self.progress('Checking this computer', 'Checking the destination and required components.')
        time.sleep(.3)
        with self.lock:
            if options.get('directory', '').endswith('blocked'):
                self.state.update(busy=False, phase='Needs attention', error='System permission was cancelled. Choose Install server to try again.')
            elif action == 'check':
                self.state.update(busy=False, phase='System components will be prepared', detail='Choose Install server. Required components will be installed automatically.', result={'needsRuntime': True})
            else:
                self.state.update(busy=False, phase='Your server is ready', result={'url':'http://localhost:8080','directory':options['directory']})


server = installer.make_server(Fixture())
print(server.origin + '/#' + server.token, flush=True)
try:
    server.serve_forever()
finally:
    server.server_close()
