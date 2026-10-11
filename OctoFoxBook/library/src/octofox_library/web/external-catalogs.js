/* Catalog metadata is untrusted; only escaped text enters shared book markup. */
'use strict';
window.ExternalCatalogs = ({api, session, onChange, onSourcesChanged, openBook, toast, cover, bindCovers, esc}) => {
  const $ = id => document.getElementById(id);
  let jobs = [], timer, epoch = 0, generation = 0, polling = false, feeds = [];
  const pending = new Set(), sections = new Map(), imports = new Map();
  const labels = {queued:'В очереди', downloading:'Скачивается', importing:'Добавляется в библиотеку', complete:'В библиотеке', failed:'Не удалось скачать', cancelled:'Отменено'};
  const key = (catalog, item) => `${catalog}:${item}`;
  const matching = (catalog, item) => jobs.find(j => j.catalog === catalog && j.item === item) ||
    (imports.has(key(catalog,item)) ? {state:'complete',book:imports.get(key(catalog,item))} : null);
  function node(tag, text = '', cls = '') {
    const e = document.createElement(tag); e.textContent = text; e.className = cls; return e;
  }
  function button(text, action, cls = 'text-button') {
    const b = node('button', text, cls); b.type = 'button';
    b.onclick = async () => {
      b.disabled = true;
      try { await action(); } catch(e) { toast(e.message); }
      finally { b.disabled = false; updateMarks(); }
    }; return b;
  }
  function updateMarks() {
    document.querySelectorAll('[data-external-item]').forEach(b => {
      const job = matching(b.dataset.externalCatalog, b.dataset.externalItem);
      const active = job && ['queued','downloading','importing'].includes(job.state);
      const label = job ? (job.state === 'complete' ? 'Открыть книгу' : labels[job.state]) : 'Скачать';
      b.textContent = b.dataset.compact ? (job?.state === 'complete' ? '✓' : active ? '…' : '↓') : label;
      b.title = label; b.setAttribute('aria-label', `${label}: ${b.dataset.bookTitle}`);
      b.setAttribute('aria-pressed', String(!!active || job?.state === 'complete'));
      b.disabled = !!active || pending.has(key(b.dataset.externalCatalog, b.dataset.externalItem));
    });
  }
  async function select(catalog, item) {
    const job = matching(catalog, item.id);
    if (job?.state === 'complete') { $('externalBookDialog').close(); return openBook(job.book); }
    const stamp = epoch, id = key(catalog,item.id);
    pending.add(id); updateMarks();
    try {
      if (job && ['failed','cancelled'].includes(job.state)) await api('/external/action', {id:job.id,action:'retry'});
      else await api('/external/downloads', {catalog,item:item.id});
      if (stamp === epoch) { await refresh(); toast('Книга добавлена в загрузки'); }
    } finally { if (stamp === epoch) { pending.delete(id); updateMarks(); } }
  }
  function mark(catalog,item,compact = false) {
    const b = button('Скачать', () => select(catalog,item), compact ? 'external-download' : 'primary');
    Object.assign(b.dataset, {externalCatalog:catalog, externalItem:item.id, bookTitle:item.title});
    if (compact) b.dataset.compact = 'true'; return b;
  }
  function asBook(catalog,item) {
    return {...item, id:key(catalog,item.id), cover:item.cover ? '/reader-api/external/cover?' + new URLSearchParams({catalog,id:item.id}) : ''};
  }
  function details(catalog,item) {
    const job = matching(catalog,item.id);
    if (job?.state === 'complete') return openBook(job.book);
    const target = $('externalBookDetails');
    target.innerHTML = `<div class="detail-cover">${cover(asBook(catalog,item),false)}</div><h2 class="detail-title">${esc(item.title)}</h2><p class="detail-author">${esc(item.author)}</p>`;
    const actions = node('div','','detail-actions'); actions.append(mark(catalog,item));
    target.append(actions);
    if (item.summary && item.summary !== item.author) target.append(node('div',item.summary,'detail-summary'));
    bindCovers(target); updateMarks(); $('externalBookDialog').showModal();
  }
  function renderItem(catalog,item,root) {
    const tile = node('article','','book-tile external-tile');
    Object.assign(tile.dataset,{publication:key(catalog,item.id),title:item.title,author:item.author});
    const complete = matching(catalog,item.id);
    if (complete?.state === 'complete') tile.dataset.localBook = String(complete.book);
    tile.innerHTML = `${cover(asBook(catalog,item))}<div class="tile-caption"><h3>${esc(item.title)}</h3><p>${esc(item.author || 'Автор не указан')}</p></div>`;
    const open = button('', () => details(catalog,item), 'external-open');
    open.setAttribute('aria-label', `${item.title}, ${item.author}. Открыть карточку`);
    tile.append(open,mark(catalog,item,true)); root.append(tile); bindCovers(tile);
  }
  async function refresh() {
    if (!session() || polling) return;
    const stamp = epoch; polling = true;
    try {
      const result = await api('/external/downloads');
      if (stamp !== epoch) return;
      const changed = JSON.stringify(jobs) !== JSON.stringify(result.downloads);
      const completed = result.downloads.filter(j => j.state === 'complete' && !jobs.some(old => old.id === j.id && old.state === 'complete'));
      jobs = result.downloads;
      $('externalCount').textContent = jobs.filter(j => ['queued','downloading','importing'].includes(j.state)).length || '';
      updateMarks(); if (changed && !$('downloadsScreen').hidden) renderDownloads();
      for (const job of completed) {
        imports.set(key(job.catalog,job.item),String(job.book));
        const tile = Array.from($('bookGrid').children).find(e => e.dataset.publication === key(job.catalog,job.item));
        if (!tile) continue;
        const duplicate = Array.from($('bookGrid').children).find(e => e !== tile && e.dataset.localBook === String(job.book));
        if (duplicate) { tile.remove(); continue; }
        tile.dataset.localBook = String(job.book);
        try {
          const book = await api('/books/' + encodeURIComponent(job.book));
          if (stamp !== epoch || !tile.isConnected) continue;
          const temp = node('div'); temp.innerHTML = cover(book,false);
          tile.querySelector('.cover-wrap').replaceWith(temp.firstElementChild); bindCovers(tile);
        } catch (_) { /* The imported book remains available even if its cover refresh fails. */ }
      }
      if (completed.length && stamp === epoch) onChange();
    } finally { if (stamp === epoch) polling = false; }
  }
  function renderDownloads() {
    const target = $('externalJobs');
    // Do not replace a focused action while keyboard users navigate the queue.
    const focused = document.activeElement?.dataset.externalAction;
    target.replaceChildren();
    if (!jobs.length) target.append(node('p', 'Выберите книги для скачивания в общей библиотеке.', 'external-empty'));
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
    const stamp = epoch, data = await api('/external/catalogs');
    if (stamp !== epoch) return;
    $('externalSources').replaceChildren();
    for (const c of data.catalogs) {
      const row = node('article','','external-source'), info = node('div');
      info.append(node('strong',c.name),node('p',new URL(c.url).hostname,'external-meta'));
      row.append(info,button('Отключить', async () => {
        if (!confirm('Отключить каталог? Незавершённые загрузки будут отменены. Скачанные книги останутся.')) return;
        await api('/external/remove',{catalog:c.id}); sections.delete(c.id);
        await catalogs(); await refresh(); onSourcesChanged();
      })); $('externalSources').append(row);
    }
    $('externalNoSources').hidden = data.catalogs.length > 0;
    $('externalAdd').open = !data.catalogs.length;
  }
  async function shelf(append,root,{query = '', alive = () => true, filters = {}, scope = 'all'} = {}) {
    const stamp = epoch, turn = append ? generation : ++generation;
    const current = () => stamp === epoch && turn === generation && alive();
    const status = $('externalShelfStatus');
    if (!append) {
      $('externalSections').replaceChildren(); $('externalSections').hidden = true;
      status.textContent = ''; status.hidden = true;
      try {
        const data = await api('/external/catalogs'); if (!current()) return false;
        feeds = data.catalogs.map(c => ({...c,next:sections.get(c.id) || '',started:false,seen:new Set()}));
      } catch(e) { if (current()) { status.textContent = e.message; status.hidden = false; } return false; }
    }
    const activeFeeds = feeds;
    const errors = [];
    status.textContent = 'Загружаем книги из каталогов…'; status.hidden = !activeFeeds.length;
    await Promise.all(activeFeeds.map(async f => {
      if (f.started && !f.next) return;
      try {
        const data = await api('/external/browse?' + new URLSearchParams({catalog:f.id,url:f.next,q:f.started ? '' : query}));
        if (!current()) return;
        f.started = true; f.next = data.next;
        const local = new Set(Array.from(root.children).map(e => e.dataset.localBook).filter(Boolean));
        if (!append && sections.has(f.id)) {
          const b = button('← ' + f.name, () => { sections.delete(f.id); onSourcesChanged(); },'chip');
          $('externalSections').append(b); $('externalSections').hidden = false;
        }
        for (const item of data.items) {
          if (item.book) imports.set(key(f.id,item.id),String(item.book));
          if (f.seen.has(item.id)) continue; f.seen.add(item.id);
          if (!item.available) {
            if (item.navigate) {
              const b = button(item.title, () => { sections.set(f.id,item.navigate); onSourcesChanged(); },'chip');
              b.title = f.name; $('externalSections').append(b); $('externalSections').hidden = false;
            } continue;
          }
          // Only match metadata the provider supplies; never show unfiltered remote results.
          if (Object.entries(filters).some(([field,value]) => !String(item[field] || '').toLocaleLowerCase().includes(String(value.label || value.value).toLocaleLowerCase()))) continue;
          if (query && scope !== 'all' && !String(item[scope] || '').toLocaleLowerCase().includes(query.toLocaleLowerCase())) continue;
          const job = matching(f.id,item.id);
          if (job?.state === 'complete' && local.has(String(job.book))) continue;
          root.querySelector('.empty-state')?.remove(); renderItem(f.id,item,root);
          if (job?.state === 'complete') local.add(String(job.book));
        }
      } catch(e) { if (current()) errors.push(`${f.name}: ${e.message}`); }
    }));
    if (!current()) return false;
    updateMarks(); status.textContent = errors.join(' · '); status.hidden = !errors.length;
    return activeFeeds.some(f => !f.started || !!f.next);
  }
  $('openExternalSources').onclick = async () => {
    $('externalSourcesDialog').showModal();
    try { await catalogs(); } catch(e) { $('externalAddStatus').textContent = e.message; }
  };
  $('closeExternalSources').onclick = () => $('externalSourcesDialog').close();
  $('closeExternalBook').onclick = () => $('externalBookDialog').close();
  $('externalAddForm').onsubmit = async event => {
    event.preventDefault(); const b = event.submitter; b.disabled = true; const stamp = epoch;
    $('externalAddStatus').textContent = 'Проверяем подключение…';
    try {
      await api('/external/catalogs',Object.fromEntries(new FormData(event.target)));
      if (stamp !== epoch) return;
      event.target.reset(); $('externalAddStatus').textContent = ''; $('externalSourcesDialog').close(); onSourcesChanged();
    } catch(e) { if (stamp === epoch) $('externalAddStatus').textContent = e.message; }
    finally { b.disabled = false; }
  };
  $('externalGutenberg').onclick = () => {
    $('externalAdd').open = true; const form = $('externalAddForm');
    form.elements.name.value = 'Project Gutenberg · English';
    form.elements.url.value = 'https://www.gutenberg.org/ebooks/search.opds/?query=l.en';
    form.elements.username.value = ''; form.elements.password.value = '';
    form.querySelector('button[type="submit"]').focus();
  };
  return {
    start() { clearInterval(timer); refresh().catch(() => {}); timer = setInterval(() => refresh().catch(() => {}),3000); },
    async enter() { await refresh(); renderDownloads(); }, shelf,
    invalidate() { generation++; },
    reset() { epoch++; generation++; clearInterval(timer); polling = false; pending.clear(); sections.clear(); imports.clear(); jobs = []; feeds = [];
      $('externalAddForm').reset(); $('externalSourcesDialog').close(); $('externalBookDialog').close();
      $('externalSources').replaceChildren(); $('externalJobs').replaceChildren(); }
  };
};
