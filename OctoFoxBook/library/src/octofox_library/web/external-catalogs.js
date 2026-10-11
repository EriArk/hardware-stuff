/* Catalog metadata is untrusted: insert it only as text, never as markup. */
'use strict';
window.ExternalCatalogs = ({api, session, onChange, openBook, toast}) => {
  const $ = id => document.getElementById(id);
  let catalog = '', page = null, history = [], jobs = [], timer = null, epoch = 0, request = 0, polling = false;
  const pending = new Set();
  const labels = {queued: 'В очереди', downloading: 'Скачивается', importing: 'Добавляется в библиотеку',
    complete: 'В библиотеке', failed: 'Не удалось скачать', cancelled: 'Отменено'};
  function node(tag, text, cls) {
    const e = document.createElement(tag);
    if (text) e.textContent = text;
    if (cls) e.className = cls;
    return e;
  }
  function button(text, action, cls = 'secondary') {
    const b = node('button', text, cls);
    b.type = 'button';
    b.onclick = async () => {
      b.disabled = true;
      try { await action(); } catch (e) { toast(e.message); }
      finally { if (b.dataset.externalItem) updateMarks(); else b.disabled = false; }
    };
    return b;
  }
  const matching = item => jobs.find(j => j.catalog === catalog && j.item === item);
  function updateMarks() {
    document.querySelectorAll('[data-external-item]').forEach(b => {
      const job = matching(b.dataset.externalItem);
      const selected = job && !['failed', 'cancelled'].includes(job.state);
      b.textContent = job ? (selected ? '✓ ' : '') + labels[job.state] : '＋ Скачать';
      b.setAttribute('aria-pressed', String(!!selected));
      b.disabled = !!selected || pending.has(b.dataset.externalItem);
    });
  }
  async function refresh() {
    if (!session() || polling) return;
    const stamp = epoch;
    polling = true;
    try {
      const result = await api('/external/downloads');
      if (stamp !== epoch) return;
      const queueChanged = JSON.stringify(jobs) !== JSON.stringify(result.downloads);
      const changed = result.downloads.some(j => j.state === 'complete' && !jobs.some(old => old.id === j.id && old.state === 'complete'));
      jobs = result.downloads;
      const active = jobs.filter(j => ['queued', 'downloading', 'importing'].includes(j.state)).length;
      $('externalCount').textContent = active || '';
      $('externalQueueLink').textContent = 'Загрузки' + (active ? ` · ${active}` : '');
      updateMarks();
      if (queueChanged && !$('downloadsScreen').hidden) renderDownloads();
      if (changed) onChange();
    } finally { if (stamp === epoch) polling = false; }
  }
  function renderDownloads() {
    const target = $('externalJobs');
    // Do not replace a focused action while keyboard users navigate the queue.
    const focused = document.activeElement?.dataset.externalAction;
    target.replaceChildren();
    if (!jobs.length) target.append(node('p', 'Отметьте книги в каталоге — они появятся здесь.', 'external-empty'));
    for (const job of jobs) {
      const row = node('article', '', 'external-job');
      const text = node('div');
      text.append(node('h3', job.title));
      let status = labels[job.state];
      if (job.state === 'downloading') status += job.total ? ` · ${Math.min(99, Math.floor(job.received / job.total * 100))}%` : ` · ${Math.round(job.received / 1024)} KB`;
      text.append(node('p', status, 'external-meta'));
      if (job.state === 'failed') text.append(node('p', 'Проверьте доступ к каталогу и формат книги, затем повторите.', 'external-meta'));
      if (job.state === 'downloading') {
        const progress = node('progress'); progress.max = job.total || 1;
        if (job.total) progress.value = job.received;
        progress.setAttribute('aria-label', status); text.append(progress);
      }
      const actions = node('div', '', 'external-actions');
      function action(label, kind) {
        const b = button(label, async () => { await api('/external/action', {id: job.id, action: kind}); await refresh(); });
        b.dataset.externalAction = job.id + kind; actions.append(b);
      }
      if (['queued', 'downloading'].includes(job.state)) action('Отменить', 'cancel');
      if (['failed', 'cancelled'].includes(job.state)) action('Повторить', 'retry');
      if (job.state === 'complete') actions.append(button('Открыть книгу', () => openBook(job.book), 'primary'));
      if (['complete', 'failed', 'cancelled'].includes(job.state)) action('Убрать из списка', 'clear');
      row.append(text, actions); target.append(row);
    }
    if (focused) Array.from(target.querySelectorAll('[data-external-action]')).find(b => b.dataset.externalAction === focused)?.focus({preventScroll:true});
  }
  async function catalogs() {
    const stamp = epoch;
    const data = await api('/external/catalogs');
    if (stamp !== epoch) return;
    $('externalSources').replaceChildren();
    for (const c of data.catalogs) {
      const row = node('article', '', 'external-source');
      const info = node('div'); info.append(node('h3', c.name), node('p', new URL(c.url).hostname, 'external-meta'));
      row.append(info, button('Открыть каталог', async () => { catalog = c.id; page = null; history = []; await browse(); }),
        button('Отключить', async () => {
          if (!confirm('Отключить каталог? Незавершённые загрузки будут отменены. Скачанные книги останутся.')) return;
          await api('/external/remove', {catalog:c.id});
          if (catalog === c.id) { catalog = ''; page = null; $('externalBrowser').hidden = true; }
          await catalogs(); await refresh();
        }, 'text-button'));
      $('externalSources').append(row);
    }
    $('externalNoSources').hidden = data.catalogs.length > 0;
  }
  async function browse(url = '', terms = '', back = false) {
    const stamp = epoch, turn = ++request, old = page;
    $('externalBrowser').hidden = false;
    $('externalStatus').textContent = 'Открываем каталог…';
    $('externalItems').setAttribute('aria-busy', 'true');
    try {
      const data = await api('/external/browse?' + new URLSearchParams({catalog, url, q:terms}));
      if (stamp !== epoch || turn !== request) return;
      if (old && old.url !== data.url && !back) history.push(old.url);
      page = data;
      $('externalTitle').textContent = data.title;
      $('externalSearchForm').hidden = !data.search;
      $('externalBack').disabled = !history.length;
      $('externalItems').replaceChildren();
      for (const item of data.items) {
        const card = node('article', '', 'external-book');
        const cover = node('div', '◇', 'external-cover');
        if (item.cover) {
          const img = node('img'); img.alt = ''; img.loading = 'lazy'; img.referrerPolicy = 'no-referrer';
          img.src = '/reader-api/external/cover?' + new URLSearchParams({catalog,id:item.id});
          img.onerror = () => img.remove(); cover.append(img);
        }
        const body = node('div', '', 'external-book-body');
        body.append(node('h3', item.title), node('p', item.author || item.summary, 'external-meta'));
        const actions = node('div', '', 'external-actions');
        if (item.available) {
          const mark = button('＋ Скачать', async () => {
            const stamp = epoch, source = catalog;
            pending.add(item.id); updateMarks();
            try {
              const job = matching(item.id);
              if (job && ['failed', 'cancelled'].includes(job.state)) await api('/external/action', {id:job.id,action:'retry'});
              else await api('/external/downloads', {catalog:source, item:item.id});
              if (stamp === epoch) { await refresh(); toast('Книга добавлена в загрузки'); }
            } finally { pending.delete(item.id); updateMarks(); }
          });
          mark.dataset.externalItem = item.id; actions.append(mark);
        }
        if (item.navigate) actions.append(button(item.available ? 'Подробнее' : 'Открыть раздел', () => browse(item.navigate), 'text-button'));
        if (!item.available && !item.navigate) body.append(node('p', 'Нет EPUB или FB2 для скачивания', 'external-meta'));
        if (item.formats.length) body.append(node('small', item.formats.join(' · '), 'external-meta'));
        body.append(actions); card.append(cover, body); $('externalItems').append(card);
      }
      $('externalPaging').replaceChildren();
      if (data.previous) $('externalPaging').append(button('← Предыдущая страница', () => browse(data.previous)));
      if (data.next) $('externalPaging').append(button('Следующая страница →', () => browse(data.next)));
      $('externalStatus').textContent = data.items.length ? '' : 'Ничего не найдено';
      updateMarks();
    } catch (e) { if (stamp === epoch && turn === request) $('externalStatus').textContent = e.message; }
    finally { if (turn === request) $('externalItems').removeAttribute('aria-busy'); }
  }
  $('externalBack').onclick = () => { const url = history.pop(); if (url) browse(url, '', true); };
  $('externalRoot').onclick = () => { history = []; page = null; browse(); };
  $('externalSearchForm').onsubmit = event => { event.preventDefault(); browse(page.url, $('externalQuery').value); };
  $('externalAddForm').onsubmit = async event => {
    event.preventDefault(); const b = event.submitter; b.disabled = true;
    const stamp = epoch;
    $('externalAddStatus').textContent = 'Проверяем подключение…';
    try {
      const form = new FormData(event.target);
      const result = await api('/external/catalogs', Object.fromEntries(form));
      if (stamp !== epoch) return;
      event.target.reset(); $('externalAddStatus').textContent = ''; $('externalAdd').open = false;
      await catalogs(); catalog = result.id; page = null; history = []; await browse();
    } catch (e) { if (stamp === epoch) $('externalAddStatus').textContent = e.message; }
    finally { b.disabled = false; }
  };
  $('externalGutenberg').onclick = () => {
    $('externalAdd').open = true;
    const form = $('externalAddForm'); form.elements.name.value = 'Project Gutenberg · English';
    form.elements.url.value = 'https://www.gutenberg.org/ebooks/search.opds/?query=l.en';
    form.elements.username.value = ''; form.elements.password.value = '';
    form.querySelector('button[type="submit"]').focus();
  };
  return {
    start() { clearInterval(timer); refresh().catch(() => {}); timer = setInterval(() => refresh().catch(() => {}), 3000); },
    async enter(view) { await refresh(); if (view === 'external') await catalogs(); else renderDownloads(); },
    reset() { epoch++; request++; clearInterval(timer); polling = false; pending.clear(); jobs = []; page = null; catalog = ''; history = [];
      $('externalAddForm').reset(); $('externalBrowser').hidden = true; $('externalItems').replaceChildren(); $('externalSources').replaceChildren(); $('externalJobs').replaceChildren(); }
  };
};
