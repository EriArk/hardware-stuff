"""Companion setup and administrator sessions. No credentials are persisted."""
import base64
from collections import OrderedDict
from dataclasses import dataclass, field
from http.cookies import SimpleCookie
import os
import re
import secrets
import threading
import time

from .booklore_api import BookLoreAPI
from .companion_accounts import AccountControls
from .opds_facade import UpstreamFailure
from .web_errors import WebError

SESSION_SECONDS = 3600


def credentials(data, new=False):
    username, password = data.get('username'), data.get('password')
    if (not isinstance(username, str) or not isinstance(password, str)
            or not username or len(username) > 64 or ':' in username
            or any(ord(c) < 33 for c in username) or not password or len(password) > 512):
        raise WebError(400, 'Укажите логин без пробелов и пароль.')
    result = {'username': username, 'password': password}
    if new:
        # BCrypt's limit is bytes, not Python code points.
        if len(password) < 8 or len(password.encode('utf-8')) > 72:
            raise WebError(400, 'Пароль: от 8 символов до 72 байт UTF-8.')
        for key in ('name', 'email'):
            value = data.get(key)
            if not isinstance(value, str) or not value.strip() or len(value) > 254 or any(ord(c) < 32 for c in value):
                raise WebError(400, 'Укажите имя и email.')
            result[key] = value.strip()
        if not re.fullmatch(r'[^\s@]+@[^\s@]+\.[^\s@]+', result['email']):
            raise WebError(400, 'Проверьте адрес email.')
    return result


def public_user(user):
    if not isinstance(user, dict) or not isinstance(user.get('permissions'), dict):
        raise WebError(502, 'Некорректный ответ библиотеки.')
    return {key: user.get(key) for key in ('id', 'username', 'name', 'email')} | {
        'admin': user['permissions'].get('admin') is True,
    }


@dataclass
class AdminSession:
    access: str
    refresh: str
    user_id: int
    reader_token: str = ''
    csrf: str = field(default_factory=lambda: secrets.token_urlsafe(24))
    expires: float = field(default_factory=lambda: time.time() + SESSION_SECONDS)
    lock: threading.RLock = field(default_factory=threading.RLock)


class Companion:
    def __init__(self, app, upstream):
        self.app = app
        self.api = BookLoreAPI(upstream)
        self.lock = threading.RLock()
        self.mutations = threading.Lock()
        self.sessions = OrderedDict()
        self.accounts = AccountControls(self)
        self.key_path = app.database.parent / 'companion-setup-key'
        try:
            fd = os.open(self.key_path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
        except FileExistsError:
            pass
        else:
            with os.fdopen(fd, 'w', encoding='ascii') as output:
                output.write(secrets.token_urlsafe(32) + '\n')

    def admin(self, session):
        with session.lock:
            try:
                user = self.api.request('/api/v1/users/me', token=session.access)
            except WebError as error:
                if error.status != 401:
                    raise
                tokens = self.api.request('/api/v1/auth/refresh', {'refreshToken': session.refresh})
                if not isinstance(tokens, dict) or not all(isinstance(tokens.get(k), str) and tokens[k]
                                                          for k in ('accessToken', 'refreshToken')):
                    raise WebError(401, 'Войдите в Companion снова.')
                session.access, session.refresh = tokens['accessToken'], tokens['refreshToken']
                user = self.api.request('/api/v1/users/me', token=session.access)
            public = public_user(user)
            self.accounts.guard(public['username'], reading=False)
            if public['id'] != session.user_id or not public['admin']:
                raise WebError(403, 'Companion доступен администратору библиотеки.')
            return public

    def login(self, data):
        values = credentials(data)
        self.accounts.guard(values['username'], reading=False)
        tokens = self.api.login(**values)
        user = public_user(self.api.request('/api/v1/users/me', token=tokens['accessToken']))
        if not user['admin']:
            raise WebError(403, 'Для чтения откройте библиотеку. Companion доступен администратору.')
        session = AdminSession(tokens['accessToken'], tokens['refreshToken'], user['id'])
        key = secrets.token_urlsafe(32)
        with self.lock:
            self.sessions[key] = session
            while len(self.sessions) > 64:
                self.sessions.popitem(last=False)
        return key, session, user

    def prepare_reader(self, values, token=None):
        """Finish provisioning as the target user, never under the admin's identity."""
        self.accounts.guard(values['username'])
        if token is None:
            token = self.api.login(values['username'], values['password'])['accessToken']
        user = public_user(self.api.request('/api/v1/users/me', token=token))
        if user['username'] != values['username']:
            raise WebError(409, 'Логин сервера отличается. Подключение отменено.')
        accounts = self.api.request('/api/v2/opds-users', token=token)
        if not isinstance(accounts, list):
            raise WebError(502, 'Не удалось проверить доступ к библиотеке.')
        if not any(a.get('username') == values['username'] for a in accounts):
            self.api.request('/api/v2/opds-users', {
                'username': values['username'], 'password': values['password'], 'sortOrder': 'RECENT',
            }, token=token)
        authorization = 'Basic ' + base64.b64encode(
            f"{values['username']}:{values['password']}".encode('utf-8')).decode('ascii')
        try:
            # Bypass the library's short authentication cache for provisioning proof.
            self.app.client.fetch_xml('', (), authorization)
        except UpstreamFailure as error:
            if error.status not in {401, 403}:
                raise WebError(502, 'Сервер чтения временно недоступен.') from None
            raise WebError(409, 'Аккаунт существует, но вход в библиотеку не подтверждён. '
                           'Возможно, у существующего доступа другой пароль; он не изменён.') from None
        return user

    def enable_reader_service(self, admin_token):
        settings = self.api.request('/api/v1/settings', token=admin_token)
        if not isinstance(settings, dict) or type(settings.get('opdsServerEnabled')) is not bool:
            raise WebError(502, 'Не удалось проверить настройки доступа к чтению.')
        if not settings['opdsServerEnabled']:
            self.api.request('/api/v1/settings', [{'name': 'OPDS_SERVER_ENABLED', 'value': True}],
                             token=admin_token, method='PUT')

    def reader_result(self, values, admin_token, token=None):
        try:
            self.enable_reader_service(admin_token)
            self.prepare_reader(values, token)
            return {'readerReady': True}
        except WebError as error:
            return {'readerReady': False, 'message': 'Аккаунт создан. ' + error.message +
                    ' Используйте «Завершить подключение» с теми же логином и паролем.'}

    def cookie(self, value, request_origin, age=SESSION_SECONDS):
        secure = '; Secure' if request_origin.startswith('https:') else ''
        return {'Set-Cookie': f'octofox_admin={value}; HttpOnly; SameSite=Strict; '
                f'Path=/companion-api; Max-Age={age}{secure}'}

    def route(self, handler, path):
        get = handler.command == 'GET'
        current_origin = handler.request_origin(require_origin=not get)
        if not get and path == '/companion-api/network/confirm':
            self.app.throttle('network-confirm:' + handler.client_address[0], limit=30)
            return handler.send(200, self.app.network.confirm(handler.body(), current_origin))
        if get and path == '/companion-api/status':
            return handler.send(200, self.app.discovery.metadata() | {'configured': self.api.configured(),
                'desktopSetup': bool(self.app.discovery.request_origin(handler.headers, handler.client_address[0]))})
        if not get and path in {'/companion-api/setup', '/companion-api/login'}:
            self.app.throttle('companion-login:' + handler.client_address[0])
            data = handler.body()
            if path.endswith('/setup'):
                supplied = handler.headers.get('X-Setup-Key', '')
                expected = self.key_path.read_text(encoding='ascii').strip()
                if not self.app.discovery.request_origin(handler.headers, handler.client_address[0]) and (
                        not supplied or not secrets.compare_digest(supplied.encode(), expected.encode())):
                    raise WebError(403, 'Неверный ключ первоначальной настройки.')
                values = credentials(data, new=True)
                with self.mutations:
                    if self.api.configured():
                        raise WebError(409, 'Первоначальная настройка уже завершена. Войдите в аккаунт.')
                    self.api.request('/api/v1/setup', values)
                    key, session, user = self.login(data)
                    result = self.reader_result(values, session.access, session.access)
            else:
                result = {}
                key, session, user = self.login(data)
            if self.app.discovery.request_origin(handler.headers, handler.client_address[0]):
                self.app.network.remember_local(current_origin)
            cookies = [self.cookie(key, current_origin)['Set-Cookie']]
            if result.get('readerReady') is not False:
                try:
                    session.reader_token, _ = self.app.login(data['username'], data['password'])
                    cookies.append(handler.reader_cookie(session.reader_token, current_origin))
                    result['readerReady'] = True
                except (WebError, UpstreamFailure):
                    result.update(readerReady=False, message='Вход в Companion выполнен. Доступ к чтению нужно завершить в разделе подключения аккаунта.')
            return handler.send(200, {'user': user, 'csrf': session.csrf, **result}, headers={'Set-Cookie': cookies})
        cookie = SimpleCookie()
        cookie.load(handler.headers.get('Cookie', ''))
        key = cookie.get('octofox_admin')
        key = key.value if key else ''
        with self.lock:
            session = self.sessions.get(key)
            if session and session.expires < time.time():
                del self.sessions[key]
                session = None
        if not session:
            raise WebError(401, 'Войдите в Companion.')
        if not get and not secrets.compare_digest(handler.headers.get('X-CSRF-Token', '').encode(), session.csrf.encode()):
            raise WebError(403, 'Обновите страницу и повторите действие.')
        if not get and path == '/companion-api/logout':
            with self.lock:
                self.sessions.pop(key, None)
            with self.app.lock:
                self.app.sessions.pop(session.reader_token, None)
            return handler.send(200, {'ok': True}, headers={'Set-Cookie': [
                self.cookie('', current_origin, 0)['Set-Cookie'], handler.reader_cookie('', current_origin, age=0)]})
        user = self.admin(session)  # Recheck upstream privileges, including after role changes.
        if path == '/companion-api/network':
            if get:
                return handler.send(200, self.app.network.info())
            return handler.send(200, self.app.network.save(handler.body(), current_origin))
        if not get and path.startswith('/companion-api/network/'):
            if path == '/companion-api/network/check-status':
                self.app.throttle('network-poll:' + str(user['id']), limit=60)
                return handler.send(200, self.app.network.check_status(handler.body(), user['id']))
            self.app.throttle('network:' + str(user['id']), limit=12)
            if path == '/companion-api/network/external-ip':
                return handler.send(200, self.app.network.discover_ip())
            if path == '/companion-api/network/check':
                return handler.send(200, self.app.network.begin_check(handler.body(), user['id']))
        if get and path == '/companion-api/me':
            return handler.send(200, {'user': user, 'csrf': session.csrf})
        if get and path == '/companion-api/users':
            users = self.api.request('/api/v1/users', token=session.access)
            return handler.send(200, {'users': [self.accounts.decorate(public_user(u)) for u in users]})
        action = re.fullmatch(r'/companion-api/users/([1-9][0-9]*)/(profile|password|access)', path)
        if not get and action:
            self.app.throttle('account-control:' + str(user['id']), limit=30)
            result = self.accounts.action(int(action[1]), action[2], handler.body(), session)
            headers = {'Set-Cookie': [self.cookie('', current_origin, 0)['Set-Cookie'],
                       handler.reader_cookie('', current_origin, age=0)]} if result.get('signedOut') else None
            return handler.send(200, result, headers=headers)
        if not get and path == '/companion-api/users':
            values = credentials(handler.body(), new=True)
            with self.mutations:
                users = self.api.request('/api/v1/users', token=session.access)
                if any(u['username'].casefold() == values['username'].casefold() for u in users):
                    raise WebError(409, 'Аккаунт уже существует. Для продолжения используйте «Завершить подключение».')
                # New accounts can read/download, never administer other users.
                self.api.request('/api/v1/auth/register', values | {
                    'permissionAccessOpds': True, 'permissionDownload': True,
                    'permissionUpload': True, 'selectedLibraries': [],
                }, token=session.access)
                result = self.reader_result(values, session.access)
            return handler.send(201, {'username': values['username'], **result})
        if not get and path == '/companion-api/reader-access':
            values = credentials(handler.body())
            with self.mutations:
                self.enable_reader_service(session.access)
                target = self.prepare_reader(values)
            return handler.send(200, {'username': target['username'], 'readerReady': True})
        raise WebError(404, 'Не найдено.')
