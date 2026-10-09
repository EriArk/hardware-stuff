'use strict';
(() => {
  const $ = id => document.getElementById(id), dialog = $('domain-guide');
  const panels = [...dialog.querySelectorAll('[data-guide-panel]')];
  const steps = [...dialog.querySelectorAll('[data-guide-step]')];
  let step = 0, returnFocus = $('open-domain-guide');
  function hostname() {
    const value = $('guide-hostname').value.trim().toLowerCase();
    if (!value || value.length > 253 || !value.includes('.') ||
        !value.split('.').every(label => /^[a-z0-9](?:[a-z0-9-]{0,61}[a-z0-9])?$/.test(label)) ||
        /^\d+(?:\.\d+){3}$/.test(value)) return '';
    return value;
  }
  function preview() {
    const host = hostname() || 'books.example.org';
    dialog.querySelectorAll('.guide-domain-preview').forEach(n => n.textContent = host);
    dialog.querySelectorAll('.guide-origin-preview').forEach(n => n.textContent = 'https://' + host);
    const docker = $('guide-connector').value === 'docker';
    $('guide-service-url').textContent = docker ? 'http://library:8080' : $('cloudflare-service').textContent;
    $('guide-connector-hint').textContent = docker
      ? 'Connect the connector to the same Docker network as library. In a separate container, localhost points to the connector itself.'
      : 'This address opens on the computer hosting your library. It uses the port configured for your installation.';
  }
  function show(index, focus = true) {
    step = Math.max(0, Math.min(panels.length - 1, index));
    panels.forEach((panel, i) => panel.hidden = i !== step);
    steps.forEach((button, i) => i === step ? button.setAttribute('aria-current', 'step') : button.removeAttribute('aria-current'));
    $('guide-back').disabled = step === 0;
    $('guide-next').hidden = step === panels.length - 1;
    $('guide-progress').textContent = `${step + 1} / ${panels.length}`;
    preview(); dialog.scrollTop = 0;
    if (focus) panels[step].querySelector('h3').focus({preventScroll: true});
  }
  $('open-domain-guide').onclick = () => { returnFocus = $('open-domain-guide'); dialog.showModal(); show(0); };
  $('close-domain-guide').onclick = () => dialog.close();
  dialog.addEventListener('close', () => { returnFocus.focus(); returnFocus.scrollIntoView({block:'center'}); });
  $('guide-back').onclick = () => show(step - 1);
  $('guide-next').onclick = () => show(step + 1);
  steps.forEach(button => button.onclick = () => show(Number(button.dataset.guideStep)));
  $('guide-hostname').oninput = preview;
  $('guide-connector').onchange = preview;
  $('guide-add-origin').onclick = () => {
    const host = hostname(), error = $('guide-origin-error');
    if (!host) {
      error.textContent = 'Enter a valid hostname on the first step, for example books.example.org.';
      error.hidden = false; return;
    }
    const origin = 'https://' + host;
    const field = $('additional-origins');
    const values = field.value.split(/\r?\n/).map(v => v.trim()).filter(Boolean);
    if (origin !== $('primary-origin').textContent && !values.includes(origin)) values.push(origin);
    if (values.length > 8) {
      error.textContent = 'The form already has eight addresses. Remove an unused address before adding another.';
      error.hidden = false; return;
    }
    error.hidden = true; field.value = values.join('\n');
    returnFocus = field; dialog.close();
  };
  $('guide-go-check').onclick = () => {
    returnFocus = $('check-origin'); dialog.close();
  };
})();
