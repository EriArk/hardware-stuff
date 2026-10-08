"""Small server-side adapter for the pinned BookLore 2.3.1 management API."""
import json
from urllib.error import HTTPError, URLError
from urllib.request import HTTPRedirectHandler, Request, build_opener

from .web_errors import WebError


class NoRedirect(HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        return None


class BookLoreAPI:
    def __init__(self, opds_url):
        self.base = opds_url.rstrip('/').removesuffix('/api/v1/opds')
        self.opener = build_opener(NoRedirect())

    def request(self, path, data=None, token=None, method=None):
        headers = {'Accept': 'application/json'}
        if token:
            headers['Authorization'] = 'Bearer ' + token
        payload = None
        if data is not None:
            payload = json.dumps(data).encode('utf-8')
            headers['Content-Type'] = 'application/json'
        try:
            with self.opener.open(Request(self.base + path, payload, headers, method=method), timeout=20) as response:
                body = response.read(2 * 1024 * 1024 + 1)
                if len(body) > 2 * 1024 * 1024:
                    raise ValueError('Response too large')
                return json.loads(body) if body else None
        except HTTPError as error:
            # Never relay upstream exception bodies: they can contain private data.
            status = error.code
            error.close()
            messages = {
                400: 'Проверьте поля формы: сервер отклонил запрос.',
                401: 'Неверный логин или пароль либо срок входа истёк.',
                403: 'Недостаточно прав для этого действия.',
                409: 'Такой аккаунт или доступ к библиотеке уже существует.',
                429: 'Слишком много попыток. Подождите и повторите вход.',
            }
            raise WebError(status if status in messages else 502,
                           messages.get(status, 'Сервер библиотеки временно недоступен.')) from None
        except (URLError, OSError, ValueError):
            raise WebError(502, 'Нет ответа от библиотеки. Проверьте состояние перед повтором действия.') from None

    def configured(self):
        result = self.request('/api/v1/setup/status')
        if not isinstance(result, dict) or type(result.get('data')) is not bool:
            raise WebError(502, 'Не удалось определить состояние библиотеки.')
        return result['data']

    def login(self, username, password):
        result = self.request('/api/v1/auth/login', {'username': username, 'password': password})
        if not isinstance(result, dict) or not all(isinstance(result.get(k), str) and result[k]
                                                 for k in ('accessToken', 'refreshToken')):
            raise WebError(502, 'Не удалось войти в библиотеку.')
        return result
