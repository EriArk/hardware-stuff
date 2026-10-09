"""Account controls and recoverable password rotation for the pinned BookLore API."""
import base64

from .web_errors import WebError


def profile(data):
    import re
    result = {}
    for key in ('name', 'email'):
        value = data.get(key)
        if not isinstance(value, str) or not value.strip() or len(value) > 254 or any(ord(c) < 32 for c in value):
            raise WebError(400, 'Укажите имя и email.')
        result[key] = value.strip()
    if not re.fullmatch(r'[^\s@]+@[^\s@]+\.[^\s@]+', result['email']):
        raise WebError(400, 'Проверьте адрес email.')
    return result


def password(value):
    if not isinstance(value, str) or not 8 <= len(value) or len(value.encode('utf-8')) > 72:
        raise WebError(400, 'Пароль: от 8 символов до 72 байт UTF-8.')
    return value


class AccountControls:
    def __init__(self, companion):
        self.companion, self.app = companion, companion.app
        with self.app.db() as db:
            db.execute('''CREATE TABLE IF NOT EXISTS reader_access_state (
                owner_key TEXT PRIMARY KEY, user_id INTEGER NOT NULL,
                enabled INTEGER NOT NULL DEFAULT 1, password_pending INTEGER NOT NULL DEFAULT 0)''')

    def state(self, owner):
        with self.app.db() as db:
            row = db.execute('SELECT * FROM reader_access_state WHERE owner_key=?', (owner.casefold(),)).fetchone()
        return dict(row) if row else {'enabled': 1, 'password_pending': 0}

    def guard(self, owner, *, reading=True):
        state = self.state(owner)
        if not state['enabled']:
            raise WebError(403, 'Доступ к этой библиотеке отключён. Обратитесь к администратору.')
        if reading and state['password_pending']:
            raise WebError(403, 'Смена пароля не завершена. Администратор должен повторить её в Companion.')

    def decorate(self, user):
        state = self.state(user['username'])
        return user | {'accessEnabled': bool(state['enabled']), 'passwordPending': bool(state['password_pending'])}

    def save(self, user, **changes):
        with self.app.db() as db:
            db.execute('INSERT OR IGNORE INTO reader_access_state(owner_key,user_id) VALUES (?,?)',
                       (user['username'].casefold(), user['id']))
            row = db.execute('SELECT user_id FROM reader_access_state WHERE owner_key=?', (user['username'].casefold(),)).fetchone()
            if row[0] != user['id']:
                raise WebError(409, 'Владелец логина изменился. Автоматическое подключение отменено.')
            for key, value in changes.items():
                assert key in {'enabled', 'password_pending'}
                db.execute(f'UPDATE reader_access_state SET {key}=? WHERE owner_key=?',
                           (int(value), user['username'].casefold()))

    def revoke(self, user, *, admin=False):
        owner = user['username'].casefold()
        with self.app.lock:
            self.app.credential_epochs[owner] = self.app.credential_epochs.get(owner, 0) + 1
            for token, session in list(self.app.sessions.items()):
                if session.owner.casefold() == owner:
                    self.app.sessions.pop(token, None)
            for key, (name, _) in list(self.app.auth_cache.items()):
                if name.casefold() == owner:
                    self.app.auth_cache.pop(key, None)
        if admin:
            with self.companion.lock:
                for key, session in list(self.companion.sessions.items()):
                    if session.user_id == user['id']:
                        self.companion.sessions.pop(key, None)

    def rotate(self, user, value, session):
        value = password(value)
        self.save(user, password_pending=True)
        self.revoke(user)
        api = self.companion.api
        try:
            api.request('/api/v1/users/change-user-password', {'userId': user['id'], 'newPassword': value},
                        token=session.access, method='PUT')
            # The current admin token remains valid after a password change. Reuse it
            # rather than logging in twice within BookLore's one-second JWT window.
            token = session.access if user['id'] == session.user_id else api.login(user['username'], value)['accessToken']
            actual = api.request('/api/v1/users/me', token=token)
            if actual.get('id') != user['id'] or actual.get('username') != user['username']:
                raise WebError(409, 'Владелец логина изменился. Автоматическое подключение отменено.')
            accounts = api.request('/api/v2/opds-users', token=token)
            if not isinstance(accounts, list):
                raise WebError(502, 'Не удалось проверить доступ к библиотеке.')
            own = next((a for a in accounts if a.get('username') == user['username']), None)
            sort = own.get('sortOrder', 'RECENT') if own else 'RECENT'
            if own:
                # BookLore 2.3.1 cannot PATCH an OPDS password. Recreate only the
                # matching credential belonging to this user, preserving sort order.
                identity = own.get('id')
                if type(identity) is not int or identity <= 0:
                    raise WebError(502, 'Не удалось проверить доступ к библиотеке.')
                api.request(f'/api/v2/opds-users/{identity}', token=token, method='DELETE')
            self.companion.enable_reader_service(session.access)
            api.request('/api/v2/opds-users', {'username': user['username'], 'password': value, 'sortOrder': sort}, token=token)
            authorization = 'Basic ' + base64.b64encode(f"{user['username']}:{value}".encode()).decode()
            self.app.client.fetch_xml('', (), authorization)
        except Exception:
            # A timeout may follow a successful mutation. Keep the durable gate shut
            # and let the administrator explicitly retry; never claim rollback.
            return {'ok': False, 'passwordPending': True,
                    'message': 'Смена пароля не завершена. Доступ к чтению временно закрыт. Повторите смену пароля; книги сохранены.'}
        self.save(user, password_pending=False)
        self.revoke(user, admin=True)
        return {'ok': True, 'passwordPending': False, 'signedOut': user['id'] == session.user_id}

    def action(self, identity, action, data, session):
        from .companion import public_user
        api = self.companion.api
        with self.companion.mutations:
            users = api.request('/api/v1/users', token=session.access)
            target = next((u for u in users if u.get('id') == identity), None)
            if not target:
                raise WebError(404, 'Аккаунт не найден. Обновите список.')
            user = public_user(target)
            if action == 'profile':
                # Omit permissions/libraries entirely: profile edits cannot grant
                # roles or replace assigned catalogs supplied by another admin.
                result = api.request(f'/api/v1/users/{identity}', profile(data), token=session.access, method='PUT')
                return {'ok': True, 'user': self.decorate(public_user(result))}
            if action == 'password':
                return self.rotate(user, data.get('password'), session)
            if action == 'access':
                enabled = data.get('enabled')
                if type(enabled) is not bool:
                    raise WebError(400, 'Укажите состояние доступа.')
                if user['admin'] and not enabled:
                    raise WebError(400, 'Доступ администратора нельзя отключить.')
                if enabled and self.state(user['username'])['password_pending']:
                    raise WebError(409, 'Сначала завершите смену пароля.')
                self.save(user, enabled=enabled)
                self.revoke(user, admin=True)
                return {'ok': True, 'user': self.decorate(user)}
            raise WebError(404, 'Не найдено.')
