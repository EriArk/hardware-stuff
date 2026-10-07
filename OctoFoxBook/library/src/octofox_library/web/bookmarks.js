/* User-created bookmarks are independent of automatic reading progress. */
(function(root) {
  'use strict';
  class Bookmarks {
    constructor(api, view, nodes, env = root) {
      this.api = api; this.view = view; this.nodes = nodes; this.env = env; this.serial = 0;
      nodes.open.onclick = () => this.open();
      nodes.close.onclick = () => this.close();
      nodes.dialog.addEventListener('close', () => {this.serial++; this.reader = null;});
      nodes.form.onsubmit = event => {event.preventDefault(); this.add();};
    }
    close() { this.serial++; this.reader = null; this.nodes.dialog.close(); }
    async open() {
      const reader = this.view.reader(), position = this.view.position();
      if (!reader || !position) return;
      const serial = ++this.serial; this.reader = reader; this.position = position;
      this.nodes.label.value = position.label;
      this.nodes.list.replaceChildren(); this.nodes.status.textContent = 'Загружаем закладки…';
      this.nodes.add.disabled = true;
      this.nodes.dialog.showModal();
      try {
        const data = await this.api(`/books/${position.book}/bookmarks`);
        if (!this.current(serial)) return;
        this.render(data.bookmarks); this.nodes.add.disabled = false;
      } catch (error) { if (this.current(serial)) {this.nodes.status.textContent = error.message; this.nodes.add.disabled = false;} }
    }
    current(serial) { return serial === this.serial && this.reader === this.view.reader() && this.nodes.dialog.open; }
    render(items) {
      this.items = items;
      this.nodes.status.textContent = items.length ? `${items.length} закладок` : 'Закладок пока нет';
      this.nodes.list.replaceChildren();
      for (const item of items) {
        const row = this.env.document.createElement('li');
        const go = this.env.document.createElement('button');
        go.type = 'button'; go.className = 'bookmark-go';
        const label = this.env.document.createElement('strong'); label.textContent = item.label;
        const excerpt = this.env.document.createElement('span');
        excerpt.textContent = item.excerpt || `Глава ${item.chapter + 1}`;
        go.append(label, excerpt); row.append(go);
        go.onclick = async () => {
          if (this.busy) return;
          const reader = this.reader;
          this.close();
          if (reader !== this.view.reader()) return;
          try {await this.view.jump(item);} catch (error) {this.view.error(error.message);}
        };
        for (const [action, title, glyph] of [['rename','Переименовать','✎'], ['remove','Удалить закладку','×']]) {
          const button = this.env.document.createElement('button');
          button.type = 'button'; button.className = 'icon-button'; button.textContent = glyph;
          button.setAttribute('aria-label', `${title}: ${item.label}`); button.title = title;
          button.onclick = () => {
            if (this.busy) return;
            if (action === 'remove') {
              if (this.env.confirm(`Удалить закладку «${item.label}»?`)) this.mutate({action, id: item.id});
            } else {
              const label = this.env.prompt('Название закладки', item.label);
              if (label !== null) this.mutate({action, id: item.id, label});
            }
          };
          row.append(button);
        }
        this.nodes.list.append(row);
      }
    }
    add() {
      const p = this.position;
      if (p) this.mutate({action: 'add', chapter: p.chapter, anchor: p.anchor,
        label: this.nodes.label.value, excerpt: p.excerpt});
    }
    async mutate(data) {
      if (this.busy || !this.reader) return;
      const serial = this.serial; this.busy = true; this.nodes.add.disabled = true;
      try {
        const result = await this.api(`/books/${this.position.book}/bookmarks`, data);
        if (this.current(serial)) this.render(result.bookmarks);
      } catch (error) { if (this.current(serial)) this.nodes.status.textContent = error.message; }
      finally {this.busy = false; if (this.current(serial)) this.nodes.add.disabled = false;}
    }
  }
  if (typeof module !== 'undefined' && module.exports) module.exports = {Bookmarks};
  else root.BookBookmarks = {Bookmarks};
})(typeof window !== 'undefined' ? window : globalThis);
