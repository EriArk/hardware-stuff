'use strict';
(async () => {
  const result = document.getElementById('connection-result');
  const token = location.hash.slice(1);
  history.replaceState(null, '', '/connection-check');
  if (!/^[A-Za-z0-9_-]{43}$/.test(token)) { result.textContent = 'Создайте новую ссылку в разделе «Подключение» Companion.'; return; }
  try {
    const response = await fetch('/companion-api/network/confirm', {method:'POST', credentials:'omit',
      headers:{'Content-Type':'application/json'}, body:JSON.stringify({token})});
    const data = await response.json();
    result.textContent = response.ok ? 'Подключение подтверждено. Этот адрес ведёт в вашу библиотеку.' : data.error;
  } catch (_) { result.textContent = 'Не удалось подтвердить подключение. Попробуйте новую ссылку.'; }
})();
