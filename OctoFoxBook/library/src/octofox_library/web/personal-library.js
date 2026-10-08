(function (root) {
  'use strict';
  // Sequential requests bound memory and server load. One bad FB2 never aborts the batch.
  async function uploadBatch(files, {upload, alive = () => true, report = () => {},
    sleep = ms => new Promise(resolve => setTimeout(resolve, ms))}) {
    const result = {total: files.length, done: 0, added: 0, replaced: 0, duplicates: 0, kept: 0, errors: [], stopped: false};
    for (const file of files) {
      if (!alive()) { result.stopped = true; break; }
      report({...result, file: file.name});
      try {
        if (!/\.(fb2|epub)$/i.test(file.name) || file.size < 256 || file.size > 16 * 1024 * 1024)
          throw new Error('Нужен FB2 или EPUB размером от 256 байт до 16 МБ');
        let uploaded;
        for (let retry = 0; ; retry++) {
          if (!alive()) { result.stopped = true; return result; }
          try { uploaded = await upload(file); break; }
          catch (error) {
            if (error.status === 401 || error.status === 403) { result.stopped = true; throw error; }
            if (error.status !== 429 || retry >= 3) throw error;
            const wait = Math.min(120, Math.max(1, error.retryAfter || 60));
            report({...result, file: file.name, waiting: wait});
            // Check cancellation/session changes during cooldown, not just at its end.
            for (let second = 0; second < wait && alive(); second++) await sleep(1000);
          }
        }
        if (uploaded?.uploadResult === 'replaced') result.replaced++;
        else if (uploaded?.uploadResult === 'duplicate') result.duplicates++;
        else if (uploaded?.uploadResult === 'kept') result.kept++;
        else result.added++;
      } catch (error) {
        result.errors.push({name: file.name, message: error.message});
      }
      result.done++;
      if (!alive() || result.stopped) { result.stopped = true; break; }
      report({...result});
    }
    return result;
  }

  function createPersonalLibrary({api, session, onFilter, onChange}) {
    const $ = id => document.getElementById(id);
    const select = $('personalCollection'), dialog = $('personalCollectionsDialog');
    let selected = '', revision = 0, generation = 0, busy = false, items = [];
    const node = (tag, text, cls) => {
      const n = document.createElement(tag);
      if (text !== undefined) n.textContent = text;
      if (cls) n.className = cls;
      return n;
    };
    function render() {
      select.replaceChildren();
      const all = node('option', 'Все мои книги'); all.value = ''; select.append(all);
      for (const item of items) {
        const option = node('option', `${item.name} · ${item.count}`); option.value = item.id; select.append(option);
      }
      if (!items.some(c => c.id === selected)) selected = '';
      select.value = selected;
      const list = $('personalCollectionsList'); list.replaceChildren();
      if (!items.length) list.append(node('p', 'Создай первую коллекцию, затем добавляй книги из их карточек.', 'fine'));
      for (const item of items) {
        const row = node('div', undefined, 'personal-collection-row');
        const title = node('button', `${item.name} · ${item.count}`, 'text-button collection-name');
        title.type = 'button';
        title.onclick = () => { selected = item.id; select.value = selected; dialog.close(); onFilter(selected); };
        const rename = node('button', 'Переименовать', 'text-button'); rename.type = 'button';
        rename.onclick = () => {
          const name = root.prompt('Название коллекции', item.name);
          if (name !== null) mutate({action: 'rename', id: item.id, name});
        };
        const remove = node('button', 'Удалить', 'text-button'); remove.type = 'button';
        remove.onclick = () => {
          if (root.confirm(`Удалить коллекцию «${item.name}»? Книги останутся в моей библиотеке.`))
            mutate({action: 'delete', id: item.id});
        };
        row.append(title, rename, remove); list.append(row);
      }
    }
    async function refresh() {
      const id = ++revision, account = session();
      const result = await api('/personal-collections');
      if (id !== revision || account !== session()) return;
      const previous = selected;
      items = result.collections; render();
      if (previous && previous !== selected) onFilter(selected);
    }
    async function mutate(data) {
      if (busy) return;
      busy = true;
      const account = session(), stamp = generation;
      $('personalCollectionCreate').disabled = true;
      $('personalCollectionsStatus').textContent = 'Сохраняем…';
      try {
        await api('/personal-collections', data);
        if (account !== session() || stamp !== generation) return;
        $('personalCollectionName').value = '';
        await refresh();
        if (account === session() && stamp === generation) $('personalCollectionsStatus').textContent = '';
      } catch (error) {
        if (account === session() && stamp === generation) $('personalCollectionsStatus').textContent = error.message;
      } finally {
        if (stamp === generation) { busy = false; $('personalCollectionCreate').disabled = false; }
      }
    }
    select.onchange = () => { selected = select.value; onFilter(selected); };
    $('managePersonalCollections').onclick = () => {
      dialog.showModal();
      $('personalCollectionName').focus();
      const account = session();
      refresh().catch(error => { if (account === session()) $('personalCollectionsStatus').textContent = error.message; });
    };
    $('closePersonalCollections').onclick = () => dialog.close();
    $('personalCollectionForm').onsubmit = event => {
      event.preventDefault(); mutate({action: 'create', name: $('personalCollectionName').value});
    };

    function attachBook(host, book, changed) {
      const account = session(), stamp = generation;
      const active = () => host.isConnected && account === session() && stamp === generation;
      let saving = false, loadRevision = 0;
      const details = node('details', undefined, 'book-personal-collections');
      const summary = node('summary', 'В коллекции');
      const status = node('p', '', 'status'); status.setAttribute('role', 'status');
      const list = node('div', undefined, 'collection-memberships');
      const form = node('form', undefined, 'collection-create');
      const input = node('input'); input.placeholder = 'Новая коллекция'; input.maxLength = 80; input.required = true;
      input.setAttribute('aria-label', 'Название новой коллекции');
      const create = node('button', 'Создать', 'secondary'); create.type = 'submit';
      form.append(input, create); details.append(summary, list, form, status); host.append(details);
      async function load() {
        const request = ++loadRevision;
        status.textContent = 'Загружаем коллекции…';
        try {
          const data = await api(`/books/${book.id}/personal-collections`);
          if (!active() || request !== loadRevision) return;
          list.replaceChildren();
          const count = data.collections.filter(c => c.selected).length;
          summary.textContent = count ? `В коллекциях · ${count}` : 'В коллекции';
          if (!data.collections.length) list.append(node('p', 'Пока нет коллекций. Создай свою ниже.', 'fine'));
          for (const item of data.collections) {
            const label = node('label'), checkbox = node('input');
            checkbox.type = 'checkbox'; checkbox.checked = item.selected;
            checkbox.onchange = () => assign(item.id, checkbox.checked);
            label.append(checkbox, node('span', item.name)); list.append(label);
          }
          status.textContent = '';
        } catch (error) { if (active() && request === loadRevision) status.textContent = error.message; }
      }
      async function assign(identity, selectedValue) {
        if (saving) return;
        loadRevision++;
        saving = true; create.disabled = true;
        list.querySelectorAll('input').forEach(n => { n.disabled = true; });
        try {
          const result = await api(`/books/${book.id}/personal-collections`, {id: identity, selected: selectedValue});
          if (!active()) return;
          book.inLibrary = result.inLibrary;
          changed(result);
          await load();
          onChange();
        } catch (error) { if (active()) { await load(); status.textContent = error.message; } }
        finally { saving = false; create.disabled = false; list.querySelectorAll('input').forEach(n => { n.disabled = false; }); }
      }
      details.ontoggle = () => { if (details.open && !saving) load(); };
      form.onsubmit = async event => {
        event.preventDefault(); if (saving) return;
        saving = true; create.disabled = true;
        try {
          const result = await api('/personal-collections', {action: 'create', name: input.value});
          if (!active()) return;
          input.value = ''; saving = false;
          await assign(result.id, true);
        } catch (error) { if (active()) status.textContent = error.message; }
        finally { saving = false; create.disabled = false; }
      };
    }
    return {refresh, attachBook,
      async enter(value = '') { selected = value; await refresh(); },
      reset() { generation++; revision++; busy = false; selected = ''; items = []; dialog.close(); render(); $('personalCollectionCreate').disabled = false; $('personalCollectionName').value = ''; $('personalCollectionsStatus').textContent = ''; }
    };
  }
  root.BookPersonal = {uploadBatch, createPersonalLibrary};
  if (typeof module !== 'undefined' && module.exports) module.exports = root.BookPersonal;
})(typeof window !== 'undefined' ? window : globalThis);
