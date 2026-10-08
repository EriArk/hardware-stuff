'use strict';
(() => {
  const $ = id => document.getElementById(id);
  let configured = false;
  let csrf = '';
  let creating = false;
  const network = window.createCompanionNetwork({api, report});

  function notice(message = '', error = false) {
    $('notice').textContent = message;
    $('notice').hidden = !message;
    $('notice').classList.toggle('error', error);
  }

  async function api(path, data, extraHeaders = {}) {
    const response = await fetch('/companion-api/' + path, {
      method: data === undefined ? 'GET' : 'POST', credentials: 'same-origin',
      headers: {'Content-Type': 'application/json', 'X-CSRF-Token': csrf, ...extraHeaders},
      ...(data === undefined ? {} : {body: JSON.stringify(data)}),
    });
    const result = await response.json();
    if (!response.ok) {
      const error = new Error(result.error || 'Не удалось выполнить действие.');
      error.status = response.status;
      throw error;
    }
    return result;
  }

  function welcome() {
    csrf = '';
    network.reset();
    $('companion-nav').hidden = true;
    $('dashboard').hidden = true;
    $('logout').hidden = true;
    $('welcome').hidden = false;
    $('setup-fields').hidden = configured;
    for (const input of $('setup-fields').querySelectorAll('input')) input.disabled = configured;
    $('form-kicker').textContent = configured ? 'С ВОЗВРАЩЕНИЕМ' : 'ПЕРВЫЙ ЗАПУСК';
    $('form-title').textContent = configured ? 'Войти в Companion' : 'Настроим библиотеку';
    $('form-description').textContent = configured
      ? 'Войдите как администратор, чтобы управлять аккаунтами читателей.'
      : 'Первый аккаунт станет администратором. Позже вы сможете пригласить других читателей.';
    $('identity-submit').textContent = configured ? 'Войти →' : 'Создать библиотеку →';
    $('password-hint').hidden = configured;
    const password = $('identity-form').elements.password;
    password.minLength = configured ? 1 : 8;
    password.autocomplete = configured ? 'current-password' : 'new-password';
  }

  async function users() {
    const data = await api('users');
    const fragment = document.createDocumentFragment();
    for (const user of data.users) {
      const li = document.createElement('li');
      const avatar = document.createElement('span');
      avatar.className = 'avatar'; avatar.setAttribute('aria-hidden', 'true');
      avatar.textContent = (user.name || user.username).slice(0, 1).toLocaleUpperCase();
      const info = document.createElement('div'); info.className = 'user-info';
      const name = document.createElement('strong'); name.textContent = user.name;
      const login = document.createElement('small'); login.textContent = '@' + user.username;
      info.append(name, login);
      const role = document.createElement('span'); role.className = 'role';
      role.textContent = user.admin ? 'Администратор' : 'Читатель';
      li.append(avatar, info, role); fragment.append(li);
    }
    $('users').replaceChildren(fragment);
    $('user-count').textContent = data.users.length;
  }

  async function dashboard(data) {
    configured = true; csrf = data.csrf;
    $('companion-nav').hidden = false;
    network.hide();
    $('welcome').hidden = true; $('dashboard').hidden = false; $('logout').hidden = false;
    $('signed-in').textContent = 'Вы вошли как ' + (data.user.name || data.user.username);
    await users();
  }

  function report(error) {
    notice(error.message || 'Нет соединения. Проверьте состояние перед повтором действия.', true);
    if (error.status === 401) { $('reader-dialog').close(); welcome(); }
  }

  $('network-tab').addEventListener('click', () => network.show());
  $('readers-tab').addEventListener('click', () => { network.hide(); $('dashboard').hidden = false; });

  $('identity-form').addEventListener('submit', async event => {
    event.preventDefault();
    const form = event.currentTarget, button = $('identity-submit');
    const data = Object.fromEntries(new FormData(form));
    const setupKey = data.setupKey; delete data.setupKey;
    button.disabled = true; notice();
    try {
      const result = await api(configured ? 'login' : 'setup', data, setupKey ? {'X-Setup-Key': setupKey} : {});
      form.reset();
      await dashboard(result);
      if (result.readerReady === false) notice(result.message, true);
      else if (result.readerReady) notice('Библиотека готова. Теперь можно добавить книги или пригласить читателя.');
    } catch (error) {
      // A lost response may follow successful setup. Never blindly resubmit it.
      try { configured = (await api('status')).configured; welcome(); } catch (_) { /* retain the original failure */ }
      report(error);
    } finally { button.disabled = false; }
  });

  $('add-reader').addEventListener('click', () => {
    $('create-error').hidden = true; $('reader-dialog').showModal();
  });
  $('close-dialog').addEventListener('click', () => { if (!creating) $('reader-dialog').close(); });
  $('reader-dialog').addEventListener('cancel', event => { if (creating) event.preventDefault(); });
  $('reader-form').addEventListener('submit', async event => {
    event.preventDefault();
    const form = event.currentTarget, button = form.querySelector('button');
    creating = true; button.disabled = true; $('create-error').hidden = true;
    try {
      const result = await api('users', Object.fromEntries(new FormData(form)));
      form.reset(); $('reader-dialog').close();
      notice(result.readerReady ? 'Аккаунт создан. Передайте читателю его логин и пароль.' : result.message, !result.readerReady);
      await users();
    } catch (error) {
      $('create-error').textContent = error.message || 'Нет ответа. Обновите список аккаунтов перед повтором.';
      $('create-error').hidden = false;
      if (error.status === 401) report(error);
    } finally { creating = false; button.disabled = false; }
  });

  $('repair-form').addEventListener('submit', async event => {
    event.preventDefault();
    const form = event.currentTarget, button = form.querySelector('button'); button.disabled = true;
    try {
      await api('reader-access', Object.fromEntries(new FormData(form))); form.reset();
      notice('Подключение завершено. Можно войти в библиотеку с этим логином и паролем.');
    } catch (error) { report(error); } finally { button.disabled = false; }
  });
  $('refresh-users').addEventListener('click', async () => { try { await users(); notice(); } catch (error) { report(error); } });
  $('logout').addEventListener('click', async () => {
    try { await api('logout', {}); $('identity-form').reset(); $('reader-form').reset(); $('repair-form').reset(); notice(); welcome(); }
    catch (error) { report(error); }
  });

  (async () => {
    try {
      configured = (await api('status')).configured;
      if (configured) {
        try { await dashboard(await api('me')); return; }
        catch (error) { if (error.status !== 401 && error.status !== 403) throw error; }
      }
      welcome();
    } catch (error) { report(error); }
    finally { $('loading').hidden = true; }
  })();
})();
