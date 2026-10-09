'use strict';
window.createCompanionBackups = ({api, report, csrf}) => {
  const $ = id => document.getElementById(id);
  const storageKey = 'octofox-backup-operation';
  let busy = false, timer = null, preview = null, uploadId = null;
  const size = bytes => bytes >= 1024 ** 3 ? (bytes / 1024 ** 3).toFixed(2) + ' GB' : bytes >= 1024 ** 2 ? (bytes / 1024 ** 2).toFixed(1) + ' MB' : Math.max(1, Math.round(bytes / 1024)) + ' KB';
  function message(text, error = false) {
    $('backup-notice').textContent = text;
    $('backup-notice').hidden = !text;
    $('backup-notice').classList.toggle('error', error);
  }
  function controls(value) {
    busy = value;
    for (const id of ['backup-create', 'backup-file', 'backup-check', 'backup-restore', 'backup-refresh']) $(id).disabled = value;
    $('backup-progress').hidden = !value;
  }
  function showScreen() {
    $('dashboard').hidden = true; $('network-screen').hidden = true; $('backups-screen').hidden = false;
    for (const id of ['readers-tab', 'network-tab']) $(id).removeAttribute('aria-current');
    $('backups-tab').setAttribute('aria-current', 'page');
  }
  async function load() {
    const data = await api('backups');
    $('backup-unavailable').hidden = data.available;
    $('backup-actions').hidden = !data.available;
    const list = $('backup-list'); list.replaceChildren();
    for (const row of data.archives) {
      const item = document.createElement('li'); item.className = 'backup-item';
      const info = document.createElement('div');
      const title = document.createElement('strong'); title.textContent = new Date(row.created).toLocaleString('en');
      const details = document.createElement('p'); details.className = 'hint';
      details.textContent = `${size(row.bytes)} Ã‚Â· ${row.files} files${row.safety ? ' Ã‚Â· Safety copy before restore' : ''}`;
      info.append(title, details);
      const buttons = document.createElement('div'); buttons.className = 'backup-buttons';
      const download = document.createElement('a'); download.className = 'text-link'; download.textContent = 'Download ZIP';
      download.href = `/companion-api/backups/${row.id}/download`; download.setAttribute('download', '');
      const remove = document.createElement('button'); remove.className = 'quiet'; remove.textContent = 'Delete';
      remove.onclick = async () => {
        if (busy || !window.confirm('Delete this server copy? Keep a downloaded copy somewhere safe first.')) return;
        remove.disabled = true;
        try { await api(`backups/${row.id}/delete`, {}); await load(); } catch (error) { report(error); }
        finally { remove.disabled = false; }
      };
      buttons.append(download, remove); item.append(info, buttons); list.append(item);
    }
    $('backup-empty').hidden = data.archives.length > 0;
    if (data.job?.state === 'running' || data.job?.state === 'recovery-required') follow(data.job);
  }
  function follow(job) {
    clearTimeout(timer); controls(true); showScreen();
    sessionStorage.setItem(storageKey, job.token);
    $('backup-phase').textContent = job.phase;
    async function poll() {
      try {
        const result = await api('backups/progress', {token: job.token});
        $('backup-phase').textContent = result.phase;
        if (result.state !== 'running') {
          sessionStorage.removeItem(storageKey); controls(false);
          if (result.state !== 'done') {
            message(result.error || 'The operation could not be completed.', true);
            $('backup-return').hidden = result.action === 'verify';
            return;
          }
          if (result.action === 'verify') {
            preview = result.preview;
            $('backup-preview').hidden = false;
            $('backup-preview-text').textContent = `Created ${new Date(preview.created).toLocaleString('en')} Ã‚Â· ${preview.files} files Ã‚Â· ${size(preview.bytes)} unpacked. All file checksums passed.`;
            message('Backup checked. Review the details before restoring.');
          } else {
            $('backup-actions').hidden = true;
            preview = null; $('backup-preview').hidden = true;
            message(result.action === 'restore' ? 'Restore complete. Sign in with an administrator account from the restored backup. Your safety copy is kept on the server.' : 'Backup ready. Sign in again to download it.');
            $('backup-return').hidden = false;
          }
          return;
        }
      } catch (error) {
        if ([403, 409].includes(error.status)) {
          sessionStorage.removeItem(storageKey); controls(false);
          message('This operation is no longer available. Sign in to view the latest backup status.', true);
          $('backup-return').hidden = false; return;
        }
        $('backup-phase').textContent = 'The library is paused. Waiting for the server to returnÃ¢â‚¬Â¦';
      }
      timer = setTimeout(poll, 2500);
    }
    poll();
  }
  $('backup-create').onclick = async () => {
    if (busy) return;
    controls(true); message();
    try { follow(await api('backups/create', {})); }
    catch (error) { controls(false); message(error.message, true); }
  };
  $('backup-upload-form').onsubmit = async event => {
    event.preventDefault(); if (busy) return;
    const file = $('backup-file').files[0]; if (!file) return;
    controls(true); preview = null; $('backup-preview').hidden = true; message();
    try {
      const start = await api('backups/upload', {bytes: file.size}); uploadId = start.id;
      for (let offset = 0; offset < file.size;) {
        $('backup-phase').textContent = `Uploading backupÃ¢â‚¬Â¦ ${Math.floor(offset / file.size * 100)}%`;
        const blob = file.slice(offset, offset + start.chunkBytes);
        const response = await fetch(`/companion-api/backups/upload/${uploadId}/chunk`, {
          method: 'POST', credentials: 'same-origin',
          headers: {'Content-Type': 'application/octet-stream', 'X-CSRF-Token': csrf(), 'X-Upload-Offset': String(offset)}, body: blob,
        });
        const result = await response.json();
        if (!response.ok) throw new Error(result.error || 'Upload failed.');
        offset = result.offset;
      }
      const job = await api(`backups/upload/${uploadId}/finish`, {}); uploadId = null; follow(job);
    } catch (error) {
      if (uploadId) { try { await api(`backups/upload/${uploadId}/cancel`, {}); } catch (_) { /* expires on the server */ } }
      uploadId = null; controls(false); message(error.message || 'Upload interrupted. Choose the file and try again.', true);
    }
  };
  $('backup-restore-form').onsubmit = async event => {
    event.preventDefault(); if (busy || !preview) return;
    controls(true); message();
    try { follow(await api('backups/restore', {id: preview.id, confirm: $('backup-confirm').value})); }
    catch (error) { controls(false); message(error.message, true); }
  };
  $('backup-refresh').onclick = () => load().catch(report);
  $('backup-return').onclick = () => { sessionStorage.removeItem(storageKey); location.reload(); };
  window.addEventListener('beforeunload', event => { if (uploadId) { event.preventDefault(); event.returnValue = ''; } });
  return {
    async show() { showScreen(); try { await load(); } catch (error) { message(error.message, true); } },
    hide() { $('backups-screen').hidden = true; $('backups-tab').removeAttribute('aria-current'); },
    reset() { clearTimeout(timer); controls(false); $('backups-screen').hidden = true; preview = null; $('backup-preview').hidden = true; },
    resume() { const token = sessionStorage.getItem(storageKey); if (!token) return false; follow({token, phase: 'Checking the previous operationÃ¢â‚¬Â¦'}); return true; },
  };
};
