'use strict';
window.createCompanionNetwork = ({api, report}) => {
  const $ = id => document.getElementById(id);
  let revision = null, token = '', timer = null, expiryTimer = null, generation = 0;
  const message = (text = '', error = false) => {
    $('network-notice').textContent = text; $('network-notice').hidden = !text;
    $('network-notice').classList.toggle('error', error);
  };
  function stop() { clearTimeout(timer); timer = null; }
  function clearCheck() {
    stop(); clearTimeout(expiryTimer); expiryTimer = null; token = '';
    $('check-link-panel').hidden = true; $('check-qr').removeAttribute('src');
    $('check-url').value = ''; $('open-check').removeAttribute('href');
  }
  function addressHint() {
    const host = new URL($('check-origin').value).hostname;
    const local = host === 'localhost' || host.endsWith('.localhost') || host === '[::1]' || host.startsWith('127.');
    $('check-address-hint').textContent = local
      ? 'Этот адрес работает только на самом сервере. Для телефона выберите сохранённый сетевой адрес, а для мобильного Интернета — внешний адрес библиотеки.'
      : 'Для проверки через мобильный Интернет выберите внешний адрес. Домашний IP доступен только из домашней сети или через VPN.';
  }
  function render(data) {
    revision = data.revision;
    $('primary-origin').textContent = data.primaryOrigin;
    $('additional-origins').value = data.additionalOrigins.join('\n');
    $('check-origin').replaceChildren(...[data.primaryOrigin, ...data.additionalOrigins].map(value => {
      const option = document.createElement('option'); option.value = option.textContent = value; return option;
    }));
    addressHint();
    const port = data.publishedPort;
    $('lan-status').textContent = data.publishedBind === '127.0.0.1'
      ? `Порт ${port || ''} открыт только на компьютере с библиотекой. Для локальной сети выполните шаги ниже.`
      : data.publishedBind === '0.0.0.0'
        ? `Порт ${port || ''} опубликован на сетевых интерфейсах. Доступ также зависит от брандмауэра и роутера.`
        : 'Параметры опубликованного порта неизвестны. Проверьте настройки запуска на сервере.';
    const host = data.publishedBind && data.publishedBind !== '0.0.0.0' ? data.publishedBind : '127.0.0.1';
    $('cloudflare-service').textContent = port ? `http://${host}:${port}` : 'http://127.0.0.1:LIBRARY_PORT';
  }
  function failure(error) {
    message(error.message || 'Соединение прервалось. Обновите настройки перед повтором.', true);
    if (error.status === 401 || error.status === 403) report(error);
  }
  async function load() {
    const active = generation;
    const data = await api('network');
    if (active === generation) render(data);
  }
  async function check() {
    if (!token) return;
    const active = generation, requested = token;
    try {
      const data = await api('network/check-status', {token});
      if (active !== generation || token !== requested) return;
      $('check-status').textContent = data.confirmedAt
        ? `Адрес открыт в браузере · ${new Date(data.confirmedAt * 1000).toLocaleTimeString()}. Если Wi-Fi был выключен, вы проверили подключение извне.`
        : 'Ожидаем открытия ссылки. Проверка работает без входа в аккаунт.';
      stop();
      if (!data.confirmedAt) timer = setTimeout(check, 4000);
    } catch (error) {
      stop();
      if (active === generation && token === requested) {
        if (error.status === 404) clearCheck();
        failure(error);
      }
    }
  }
  $('network-form').addEventListener('submit', async event => {
    event.preventDefault();
    if (revision === null) return;
    const button = event.currentTarget.querySelector('button'), active = generation; button.disabled = true;
    try {
      const data = await api('network', {revision, additionalOrigins: $('additional-origins').value.split('\n').map(v => v.trim()).filter(Boolean)});
      if (active !== generation) return;
      render(data); clearCheck();
      message('Адреса сохранены. Вход по ним разрешён сразу; порт, DNS и туннель настраиваются отдельно.');
    } catch (error) { if (active === generation) failure(error); } finally { button.disabled = false; }
  });
  $('network-refresh').addEventListener('click', async () => { try { await load(); message(); } catch (error) { failure(error); } });
  $('external-ip').addEventListener('click', async event => {
    const button = event.currentTarget, active = generation; button.disabled = true;
    try {
      const data = await api('network/external-ip', {});
      if (active === generation) $('external-ip-result').textContent = `${data.address} · ipify · ${new Date(data.checkedAt * 1000).toLocaleTimeString()}`;
    } catch (error) { if (active === generation) failure(error); } finally { button.disabled = false; }
  });
  $('connection-form').addEventListener('submit', async event => {
    event.preventDefault();
    const button = event.currentTarget.querySelector('button'), active = generation; button.disabled = true;
    try {
      const data = await api('network/check', {origin: $('check-origin').value});
      if (active !== generation) return;
      clearCheck(); token = data.token; message();
      $('check-url').value = data.url; $('open-check').href = data.url;
      $('check-qr').src = data.qrDataUrl; $('check-link-panel').hidden = false;
      expiryTimer = setTimeout(() => {
        clearCheck(); message('Срок QR-кода истёк. Создайте новый для повторной проверки.');
      }, data.expiresIn * 1000);
      await check();
    } catch (error) { if (active === generation) failure(error); } finally { button.disabled = false; }
  });
  $('check-refresh').addEventListener('click', check);
  $('check-origin').addEventListener('change', () => { clearCheck(); addressHint(); });
  $('check-url').addEventListener('click', event => event.currentTarget.select());
  return {
    async show() {
      $('dashboard').hidden = true; $('network-screen').hidden = false;
      $('readers-tab').removeAttribute('aria-current'); $('network-tab').setAttribute('aria-current', 'page');
      try { await load(); if (token) await check(); } catch (error) { failure(error); }
    },
    hide() { stop(); $('network-screen').hidden = true; $('network-tab').removeAttribute('aria-current'); $('readers-tab').setAttribute('aria-current', 'page'); },
    reset() { generation++; clearCheck(); revision = null; $('network-screen').hidden = true; $('external-ip-result').textContent = ''; message(); },
  };
};
