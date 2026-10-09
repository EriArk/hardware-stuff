'use strict';
window.createAccountControls = ({api, refresh, report, signedOut, notice}) => {
  const $ = id => document.getElementById(id), dialog = $('account-dialog');
  let user, busy = false;
  function render() {
    $('account-title').textContent = user.name || user.username;
    $('account-username').textContent = '@' + user.username;
    $('account-profile').elements.name.value = user.name || '';
    $('account-profile').elements.email.value = user.email || '';
    $('account-access').hidden = user.admin;
    $('account-access-confirm').hidden = user.accessEnabled === false;
    $('account-disable-confirm').checked = false;
    $('account-disable-confirm').required = user.accessEnabled !== false;
    $('account-access-button').textContent = user.accessEnabled === false ? 'Включить доступ' : 'Отключить доступ';
    $('account-state').textContent = user.passwordPending ? 'Нужно завершить смену пароля' : user.accessEnabled === false ? 'Доступ отключён' : 'Доступ включён';
  }
  async function mutate(action, data) {
    if (busy) return;
    busy = true; $('account-error').hidden = true;
    dialog.querySelectorAll('button').forEach(b => b.disabled = true);
    try {
      const result = await api(`users/${user.id}/${action}`, data);
      if (result.signedOut) {
        dialog.close(); signedOut(); notice('Пароль изменён. Войдите снова с новым паролем.'); return;
      }
      if (action === 'password') $('account-password').reset();
      if (result.user) user = result.user;
      if (result.passwordPending !== undefined) user.passwordPending = result.passwordPending;
      render();
      $('account-error').textContent = result.message || 'Изменения сохранены.';
      $('account-error').classList.toggle('error', result.ok === false);
      $('account-error').hidden = false;
      $('account-error').scrollIntoView({block:'nearest'});
      await refresh();
    } catch (error) {
      $('account-error').textContent = error.message || 'Нет ответа. Обновите список перед повтором.';
      $('account-error').classList.add('error'); $('account-error').hidden = false;
      $('account-error').scrollIntoView({block:'nearest'});
      if (error.status === 401) { dialog.close(); report(error); }
    } finally { busy = false; dialog.querySelectorAll('button').forEach(b => b.disabled = false); }
  }
  $('account-profile').onsubmit = event => { event.preventDefault(); mutate('profile', Object.fromEntries(new FormData(event.currentTarget))); };
  $('account-password').onsubmit = event => {
    event.preventDefault();
    const {password, confirm} = event.currentTarget.elements;
    confirm.setCustomValidity(password.value === confirm.value ? '' : 'Пароли не совпадают.');
    if (!confirm.reportValidity()) return;
    mutate('password', {password: password.value});
  };
  $('account-password').elements.confirm.oninput = event => event.target.setCustomValidity('');
  $('account-password').elements.password.oninput = () => $('account-password').elements.confirm.setCustomValidity('');
  $('account-access').onsubmit = event => { event.preventDefault(); mutate('access', {enabled: user.accessEnabled === false}); };
  $('close-account-dialog').onclick = () => { if (!busy) dialog.close(); };
  dialog.addEventListener('cancel', event => { if (busy) event.preventDefault(); });
  dialog.addEventListener('close', () => $('account-password').reset());
  return {open(value) { user = {...value}; $('account-error').hidden = true; $('account-password').reset(); render(); dialog.showModal(); },
    reset() { dialog.close(); $('account-password').reset(); }};
};
