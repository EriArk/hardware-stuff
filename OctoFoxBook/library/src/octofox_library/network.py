"""Persistent origin allowlist and explicit, bounded connection diagnostics."""
import ipaddress
import json
import os
import re
import secrets
import time
from urllib.parse import urlsplit
from urllib.request import Request, build_opener

from .booklore_api import NoRedirect
from .qr import qr_data_url
from .web_errors import WebError


def origin(value):
    try:
        if not isinstance(value, str) or not value or len(value) > 300 or any(
                c.isspace() or c in "\\%#?*'\"" or ord(c) < 32 for c in value):
            raise ValueError()
        url = urlsplit(value)
        if (url.scheme not in {'http', 'https'} or not url.hostname or url.username is not None
                or url.password is not None or url.path not in {'', '/'} or url.query or url.fragment):
            raise ValueError()
        host = url.hostname.encode('idna').decode('ascii').lower()
        try:
            address = ipaddress.ip_address(host)
        except ValueError:
            if len(host) > 253 or not all(re.fullmatch(r'[a-z0-9](?:[a-z0-9-]{0,61}[a-z0-9])?', part)
                                          for part in host.split('.')):
                raise ValueError()
        else:
            if address.is_unspecified or address.is_multicast:
                raise ValueError()
            host = f'[{address.compressed}]' if address.version == 6 else str(address)
        port = url.port
        if port is not None and not 1 <= port <= 65535:
            raise ValueError()
        authority = host + (f':{port}' if port and port != {'http': 80, 'https': 443}[url.scheme] else '')
        return f'{url.scheme}://{authority}'
    except (ValueError, UnicodeError):
        raise WebError(400, 'Укажите адрес http:// или https:// без пути, логина и параметров.') from None


class NetworkSettings:
    def __init__(self, app):
        self.app = app
        with app.db() as db:
            db.execute('CREATE TABLE IF NOT EXISTS network_settings (id INTEGER PRIMARY KEY CHECK(id=1), '
                       'revision INTEGER NOT NULL, origins TEXT NOT NULL)')
            db.execute("INSERT OR IGNORE INTO network_settings VALUES (1,0,'[]')")
        self.checks = {}
        self.public_ip = None

    def snapshot(self):
        with self.app.db() as db:
            row = db.execute('SELECT revision,origins FROM network_settings WHERE id=1').fetchone()
        primary = origin(self.app.origin)
        primary_url = urlsplit(primary)
        # An operator can change the primary origin in .env between launches.
        # Its new HTTPS policy must also take precedence over older saved HTTP.
        additional = [o for o in json.loads(row['origins']) if o != primary and not (
            urlsplit(o).hostname == primary_url.hostname and urlsplit(o).scheme != primary_url.scheme)]
        return {'primaryOrigin': primary, 'additionalOrigins': additional, 'revision': row['revision']}

    def allowed(self):
        state = self.snapshot()
        return [state['primaryOrigin'], *state['additionalOrigins']]

    def remember_local(self, current):
        """An authenticated owner opened the installation through the native app."""
        from .desktop_discovery import local_address
        parsed = urlsplit(current)
        if parsed.scheme != 'http' or not local_address(parsed.hostname or ''):
            return
        with self.app.db() as db:
            db.execute('BEGIN IMMEDIATE')
            row = db.execute('SELECT revision,origins FROM network_settings WHERE id=1').fetchone()
            values = json.loads(row['origins'])
            allowed = [origin(self.app.origin), *values]
            if current in allowed or len(values) >= 8 or any(
                    urlsplit(o).hostname == parsed.hostname and urlsplit(o).scheme != parsed.scheme for o in allowed):
                return
            db.execute('UPDATE network_settings SET revision=?,origins=? WHERE id=1',
                       (row['revision'] + 1, json.dumps([*values, current])))

    def request_origin(self, headers, require_origin=False, trusted_origin=None):
        host = headers.get('Host', '')
        supplied = headers.get('Origin')
        allowed = self.allowed()
        if trusted_origin:
            allowed.append(trusted_origin)
        try:
            if supplied:
                selected = origin(supplied)
                if selected in allowed and origin(urlsplit(selected).scheme + '://' + host) == selected:
                    return selected
            elif not require_origin:
                for selected in allowed:
                    if origin(urlsplit(selected).scheme + '://' + host) == selected:
                        return selected
        except WebError:
            pass
        raise WebError(403, 'Откройте библиотеку по одному из настроенных адресов.')

    def save(self, data, current):
        values = data.get('additionalOrigins')
        if not isinstance(values, list) or len(values) > 8 or type(data.get('revision')) is not int:
            raise WebError(400, 'Можно сохранить не более восьми дополнительных адресов.')
        primary = origin(self.app.origin)
        additional = list(dict.fromkeys(origin(v) for v in values))
        additional = [v for v in additional if v != primary]
        allowed = [primary, *additional]
        if current not in allowed:
            raise WebError(409, 'Сначала откройте основной адрес: текущий адрес нельзя удалить из-под активной страницы.')
        # Cookies are host-scoped, not scheme-scoped. Do not allow a second HTTP
        # route to downgrade the same HTTPS host's session.
        schemes = {}
        for value in allowed:
            parsed = urlsplit(value)
            if parsed.hostname in schemes and schemes[parsed.hostname] != parsed.scheme:
                raise WebError(400, 'Для одного имени сервера используйте одну схему: HTTP или HTTPS.')
            schemes[parsed.hostname] = parsed.scheme
        with self.app.db() as db:
            db.execute('BEGIN IMMEDIATE')
            revision = db.execute('SELECT revision FROM network_settings WHERE id=1').fetchone()[0]
            if data['revision'] != revision:
                raise WebError(409, 'Настройки уже изменены в другой вкладке. Обновите раздел и повторите изменение.')
            db.execute('UPDATE network_settings SET revision=?,origins=? WHERE id=1',
                       (revision + 1, json.dumps(additional)))
        with self.app.lock:
            self.checks = {k: v for k, v in self.checks.items() if v['origin'] in allowed}
        return self.info()

    def info(self):
        state = self.snapshot()
        bind = os.environ.get('OCTOFOX_PUBLISHED_BIND', '')
        port = os.environ.get('OCTOFOX_PUBLISHED_PORT', '')
        if not port.isdigit() or not 1 <= int(port) <= 65535:
            port = ''
        return state | {'publishedBind': bind, 'publishedPort': int(port) if port else None,
                        'externalReachability': 'unverified'}

    def discover_ip(self):
        # Only this fixed provider is contacted, only after the admin presses the
        # button. No arbitrary-URL server fetch and no cookies or credentials.
        with self.app.lock:
            if self.public_ip and time.time() - self.public_ip['checkedAt'] < 60:
                return self.public_ip
        try:
            request = Request('https://api.ipify.org?format=json', headers={'Accept': 'application/json'})
            with build_opener(NoRedirect()).open(request, timeout=6) as response:
                body = response.read(257)
                if len(body) > 256:
                    raise ValueError()
                address = ipaddress.ip_address(json.loads(body)['ip'])
                if not address.is_global:
                    raise ValueError()
        except (OSError, ValueError, KeyError, TypeError):
            raise WebError(502, 'Не удалось определить внешний IP. Настройки и доступ к книгам не изменены.') from None
        result = {'address': str(address), 'provider': 'ipify', 'checkedAt': int(time.time()),
                  'externalReachability': 'unverified'}
        with self.app.lock:
            self.public_ip = result
        return result

    def begin_check(self, data, user_id):
        target = origin(data.get('origin'))
        if target not in self.allowed():
            raise WebError(400, 'Сначала сохраните этот адрес в настройках.')
        token = secrets.token_urlsafe(32)
        now = time.time()
        with self.app.lock:
            self.checks = {k: v for k, v in self.checks.items() if v['expires'] > now}
            if len(self.checks) >= 32:
                raise WebError(429, 'Дождитесь окончания предыдущих проверок.')
            self.checks[token] = {'origin': target, 'user': user_id, 'expires': now + 600, 'confirmedAt': None}
        url = target + '/connection-check#' + token
        return {'token': token, 'url': url, 'qrDataUrl': qr_data_url(url), 'expiresIn': 600}

    def confirm(self, data, current):
        with self.app.lock:
            check = self.checks.get(str(data.get('token', '')))
            if not check or check['expires'] < time.time() or check['origin'] != current:
                raise WebError(404, 'Ссылка проверки истекла или открыта по другому адресу.')
            check['confirmedAt'] = int(time.time())
        return {'confirmed': True}

    def check_status(self, data, user_id):
        with self.app.lock:
            check = self.checks.get(str(data.get('token', '')))
            if not check or check['expires'] < time.time() or check['user'] != user_id:
                raise WebError(404, 'Проверка истекла. Создайте новую ссылку.')
            return {'origin': check['origin'], 'confirmedAt': check['confirmedAt'], 'scope': 'browser',
                    'externalReachability': 'unverified'}
