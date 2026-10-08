/* Each finite recording is one measured page, never a live whole-book timeline. */
(function(root) {
  'use strict';
  const LOOKAHEAD_PAGES = 3, MAX_CACHED_PAGE_BYTES = 6 * 1024 * 1024;
  class PageVoice {
    constructor(api, view, info, env = root) {
      this.api = api; this.view = view; this.env = env; this.engine = 'server';
      this.info = info;
      this.epoch = 0; this.status = 'idle'; this.entries = new Set();
      this.backend = {available: !!info.available, voiceURI: info.voice || 'eugene', rate: .85,
        voices: () => (this.info.voices || []).map(v => ({...v, localService: true}))};
    }
    updateInfo(info) {
      this.info = info;
      this.backend.available = !!info.available;
      // A refreshed voice catalog must not restart playback or change a valid
      // choice the reader has already made.
      if (!this.active && !(info.voices || []).some(v => v.voiceURI === this.backend.voiceURI))
        this.backend.voiceURI = info.voice || info.voices?.[0]?.voiceURI || '';
    }
    get active() { return !['idle', 'ended'].includes(this.status); }
    now() { return (this.env.Date || Date).now(); }
    state(status, error = '') { this.status = status; this.view.state(status, error); }
    setRate(rate) {
      if (Number.isFinite(rate) && rate >= .5 && rate <= 2) {
        // Validate progress at the old rate before rebasing the playback clock.
        this.follow();
        if (this.audio && !this.audio.seeking && this.seekingTo === null) {
          this.lastTime = this.audio.currentTime; this.lastWall = this.now();
        }
        this.backend.rate = rate;
        if (this.audio) this.audio.playbackRate = rate;
      }
    }
    setVoice(voice) {
      if (voice === this.backend.voiceURI) return;
      this.backend.voiceURI = voice;
      if (this.active || this.resumeRequest) this.restart(this.current?.page || this.resumeRequest?.page);
    }
    restart(page = null) {
      this.follow();
      const position = this.current?.completed ? this.position(this.current.page.end)
        : this.spokenPosition || this.resumeRequest?.position;
      const resume = this.wantsPlay;
      const audio = this.audio || this.resumeAudio;
      this.stop(false);
      this.resumeRequest = {page, position};
      this.resumeAudio = audio;
      this.state('paused');
      if (resume) this.play();
    }
    play() {
      if (this.audio && this.status === 'paused' && this.voice === this.backend.voiceURI) {
        this.wantsPlay = true;
        this.fillAhead(this.epoch);
        if (this.current.completed) this.advance(this.current, this.epoch);
        else this.startAudio(this.epoch);
        return;
      }
      const resume = this.resumeRequest;
      const audio = this.resumeAudio;
      this.stop(false); this.wantsPlay = true; this.state('loading');
      const epoch = this.epoch;
      try {
        this.voice = this.backend.voiceURI;
        let page = resume?.page || this.view.page(resume?.position);
        const position = resume?.position, anchor = position?.anchor;
        const compare = (a, b) => a.block - b.block || a.char - b.char;
        if (position?.book === page.book && position.chapter === page.chapter &&
            compare(anchor, page.anchor) >= 0 && compare(anchor, page.end) <= 0) {
          page = {...page, anchor: {...anchor}, empty: page.empty || compare(anchor, page.end) === 0};
        }
        this.current = this.prepare(page, epoch);
        this.audio = audio || new this.env.Audio();
        // A bounded page is a single PCM file, including on iOS. HLS adds an
        // unnecessary estimated/discontinuous timeline to this finite recording.
        this.audio.preload = 'auto'; this.audio.preservesPitch = true;
        try {
          this.previousAudioType = this.env.navigator?.audioSession?.type;
          if (this.env.navigator?.audioSession) this.env.navigator.audioSession.type = 'playback';
        } catch {}
        this.activate(this.current, epoch);
      } catch(error) { this.fail(error, epoch); }
    }
    prepare(page, epoch) {
      const entry = {page, id: this.env.crypto.randomUUID(), info: null, error: null, ready: false, failures: 0};
      this.entries.add(entry);
      entry.register = page.empty ? Promise.resolve() : this.api(`/books/${page.book}/speech`, {
        id: entry.id, chapter: page.chapter, anchor: page.anchor, end: page.end, voice: this.voice,
      });
      entry.register.then(() => {
        if (epoch !== this.epoch) return this.dispose(entry);
        if (page.empty) { entry.info = {empty: true, complete: true, segments: []}; entry.ready = true; }
        this.poll(entry, epoch);
      }).catch(error => { entry.error = error; if (entry === this.current) this.fail(error, epoch); });
      return entry;
    }
    dispose(entry) {
      entry.cacheController?.abort();
      if (entry.objectURL) { this.env.URL.revokeObjectURL(entry.objectURL); entry.objectURL = null; }
      this.env.clearTimeout(entry.pauseTimer);
      this.env.clearTimeout(entry.timer); this.entries.delete(entry);
      if (!entry.page.empty) this.api(`/speech/streams/${entry.id}/stop`, {}).catch(() => {});
    }
    async prefetch(entry, epoch) {
      if (entry.nextTask) return entry.nextTask;
      entry.nextTask = Promise.resolve(this.view.next(entry.page)).then(page => {
        if (epoch !== this.epoch || !this.entries.has(entry)) return null;
        entry.next = page ? this.prepare(page, epoch) : null;
        entry.hasNext = !!page; return entry.next;
      });
      entry.nextTask.catch(error => { entry.nextError = error; });
      return entry.nextTask;
    }
    async fillAhead(epoch) {
      const origin = this.current;
      let entry = origin;
      try {
        for (let ahead = 0; ahead < LOOKAHEAD_PAGES; ahead++) {
          // Queue in reading order: distant pages must not delay the current
          // page's synthesis. Each ready status extends this bounded window.
          if (epoch !== this.epoch || this.current !== origin || !this.wantsPlay || !entry?.ready) return;
          entry = await this.prefetch(entry, epoch);
        }
      } catch {} // A failed future page is reported only when it is needed.
    }
    async cacheAudio(entry, epoch) {
      if (!entry.ready || entry.info.empty || entry === this.current || entry.objectURL || entry.cacheController ||
          entry.cacheSkipped || (entry.cacheAttempts || 0) >= 2 || !this.env.fetch ||
          !this.env.URL?.createObjectURL || !this.env.AbortController) return;
      const controller = entry.cacheController = new this.env.AbortController();
      entry.cacheAttempts = (entry.cacheAttempts || 0) + 1;
      try {
        const response = await this.env.fetch(this.url(entry), {credentials: 'same-origin', signal: controller.signal});
        const bytes = Number(response.headers.get('Content-Length'));
        if (!response.ok || !bytes || bytes > MAX_CACHED_PAGE_BYTES) {
          if (bytes > MAX_CACHED_PAGE_BYTES) entry.cacheSkipped = true;
          await response.body?.cancel(); return;
        }
        const blob = await response.blob();
        if (epoch !== this.epoch || !this.entries.has(entry) || entry === this.current || controller.signal.aborted) return;
        if (blob.size <= MAX_CACHED_PAGE_BYTES) entry.objectURL = this.env.URL.createObjectURL(blob);
      } catch {} // Slow/offline preloading never prevents normal URL playback.
      finally { entry.cacheController = null; }
    }
    activate(entry, epoch) {
      if (epoch !== this.epoch) return;
      this.current = entry; this.spokenPosition = this.position(entry.page.anchor);
      this.view.save?.(this.spokenPosition);
      this.applied = null; this.following = false; this.mediaFailures = 0;
      this.lastTime = 0; this.lastWall = this.now(); this.seekingTo = 0;
      this.state('loading');
      this.fillAhead(epoch);
      entry.started = false;
      entry.source = entry.objectURL || this.url(entry);
      const audio = this.audio, current = () => epoch === this.epoch && entry === this.current &&
        (!audio.currentSrc || audio.currentSrc.endsWith(entry.source));
      audio.onloadedmetadata = () => { if (current()) this.seek(); };
      audio.oncanplay = () => { if (current()) this.seek(); };
      audio.onseeked = () => {
        if (current() && this.seekingTo !== null && Math.abs(audio.currentTime - this.seekingTo) < .35) {
          this.lastTime = audio.currentTime; this.lastWall = this.now(); this.seekingTo = null;
        }
      };
      audio.onplaying = () => {
        if (!current()) return;
        if (!this.wantsPlay) { audio.pause(); return; }
        this.env.clearTimeout(entry.pauseTimer);
        entry.started = true;
        this.state('playing'); this.follow(epoch);
      };
      audio.ontimeupdate = () => {
        if (!current()) return;
        if (!this.continueAtEnd(entry, epoch)) this.follow(epoch);
      };
      audio.onwaiting = () => { if (current() && this.wantsPlay) this.state('loading'); };
      audio.onpause = () => {
        if (!current() || !this.wantsPlay || !entry.started) return;
        // Native pause/ended can arrive in separate tasks, with ended still
        // false at pause. Do not discard intent until the media state settles.
        this.env.clearTimeout(entry.pauseTimer);
        entry.pauseTimer = this.env.setTimeout(() => {
          if (!current() || !this.wantsPlay || !audio.paused) return;
          if (!this.continueAtEnd(entry, epoch)) this.pause();
        }, 200);
      };
      audio.onerror = () => {
        if (!current()) return;
        if (entry.objectURL && entry.source === entry.objectURL) {
          this.env.URL.revokeObjectURL(entry.objectURL); entry.objectURL = null;
          entry.source = this.url(entry); audio.src = entry.source;
          audio.load(); this.startAudio(epoch); return;
        }
        if (++this.mediaFailures > 3) return this.fail(new Error('Аудио не загрузилось. Нажмите ▶ для повтора'), epoch);
        entry.retry = true; this.state('loading'); this.poll(entry, epoch);
      };
      audio.onended = () => {
        if (!current() || !this.wantsPlay || !entry.started) return;
        if (!this.continueAtEnd(entry, epoch)) {
          entry.retry = true; this.poll(entry, epoch); return;
        }
      };
      if (entry.page.empty) {
        entry.register.then(() => this.advance(entry, epoch));
      } else {
        audio.src = entry.source;
        audio.load();
        this.startAudio(epoch); // First request stays in the user's gesture; reuse this element thereafter.
      }
    }
    url(entry) { return `/reader-api/speech/streams/${entry.id}/page.wav`; }
    atMediaEnd(entry, epoch) {
      const audio = this.audio;
      if (epoch !== this.epoch || entry !== this.current || !this.wantsPlay || !audio ||
          !entry.started || audio.seeking || this.seekingTo !== null ||
          this.env.navigator?.audioSession?.state === 'interrupted') return false;
      const seconds = audio.currentTime, duration = audio.duration;
      return (audio.ended || audio.paused) && Number.isFinite(duration) && duration > 0 &&
        seconds >= duration - .12 && seconds <= duration + .12 &&
        seconds >= this.lastTime - .5 &&
        seconds <= this.lastTime + (this.now() - this.lastWall) / 1000 * this.backend.rate + 2;
    }
    continueAtEnd(entry, epoch) {
      if (!this.atMediaEnd(entry, epoch)) return false;
      if (!entry.ready) {
        // Media and status use independent requests. Under queue/network load a
        // short WAV can finish before its completion metadata reaches the player.
        // Retain play intent, but never advance until the PCM timeline is verified.
        entry.awaitingEnd = true;
        this.state('loading'); this.poll(entry, epoch);
        return true;
      }
      const parts = entry.info?.segments || [];
      const preparedDuration = parts.reduce((end, part) => Math.max(end,
        (part.pcmAt ?? part.at) + (part.pcmDuration ?? part.duration)), 0);
      if (Math.abs(this.audio.duration - preparedDuration) > .25) return false;
      entry.awaitingEnd = false;
      entry.completed = true;
      this.env.clearTimeout(entry.pauseTimer);
      this.advance(entry, epoch);
      return true;
    }
    position(anchor) { return {book: this.current.page.book, chapter: this.current.page.chapter, anchor: {...anchor}}; }
    seek() {
      if (this.seekingTo === null || !this.audio) return;
      try {
        if (Math.abs(this.audio.currentTime - this.seekingTo) > .05) this.audio.currentTime = this.seekingTo;
        if (!this.audio.seeking && Math.abs(this.audio.currentTime - this.seekingTo) < .35) {
          this.lastTime = this.audio.currentTime; this.lastWall = this.now(); this.seekingTo = null;
        }
      } catch {}
    }
    startAudio(epoch) {
      if (!this.audio || !this.wantsPlay) return;
      const entry = this.current;
      this.audio.playbackRate = this.backend.rate;
      Promise.resolve(this.audio.play()).catch(error => {
        if (epoch !== this.epoch || entry !== this.current || error.name === 'AbortError') return;
        if (error.name === 'NotAllowedError') {
          this.wantsPlay = false; this.state('paused', 'Готово. Нажмите ▶ для продолжения');
        } else if (++this.mediaFailures <= 3) {
          this.current.retry = true; this.state('loading');
        } else this.fail(error, epoch);
      });
    }
    async poll(entry, epoch) {
      if (epoch !== this.epoch || entry.polling || !this.entries.has(entry)) return;
      this.env.clearTimeout(entry.timer); entry.polling = true;
      try {
        await entry.register;
        if (!entry.page.empty) entry.info = await this.api(`/speech/streams/${entry.id}/status`);
        if (epoch !== this.epoch || !this.entries.has(entry)) return;
        entry.failures = 0;
        if (entry.info.blocked?.failed) throw new Error('Не удалось озвучить страницу. Нажмите ▶ для повтора');
        entry.ready = !!entry.info.complete;
        this.cacheAudio(entry, epoch);
        this.fillAhead(epoch);
        if (entry === this.current) {
          if (entry.info.empty) { this.advance(entry, epoch); return; }
          if (this.continueAtEnd(entry, epoch)) return;
          if (entry.awaitingEnd && entry.ready && this.wantsPlay) {
            // A truncated/mismatched file is not permission to skip unread text.
            entry.awaitingEnd = false; this.pause(); return;
          }
          if (entry.ready && entry.retry) {
            entry.retry = false; this.seekingTo = this.lastTime || 0;
            this.audio.load(); this.startAudio(epoch);
          }
          this.follow(epoch);
        }
      } catch(error) {
        if (epoch !== this.epoch) return;
        if (++entry.failures >= 3 || entry.info?.blocked?.failed) {
          entry.error = error;
          if (entry === this.current) this.fail(error, epoch);
          return;
        }
      } finally {
        entry.polling = false;
        // Keep prepared streams alive while paused, without generating more pages.
        if (epoch === this.epoch && this.entries.has(entry) && !entry.error)
          entry.timer = this.env.setTimeout(() => this.poll(entry, epoch), entry.ready ? 15000 : 1000);
      }
    }
    follow(epoch = this.epoch) {
      if (epoch !== this.epoch || !this.audio || !this.wantsPlay || this.status !== 'playing' ||
          !this.current?.ready || this.audio.seeking) return;
      if (this.seekingTo !== null) { this.seek(); if (this.seekingTo !== null) return; }
      const now = this.now(), seconds = this.audio.currentTime;
      // Reject unsolicited native-media timeline jumps before they reach the bookmark.
      if (seconds < this.lastTime - .5 || seconds > this.lastTime + (now - this.lastWall) / 1000 * this.backend.rate + 2) {
        this.seekingTo = this.lastTime; this.seek(); return;
      }
      this.lastTime = seconds; this.lastWall = now;
      const segments = this.current.info.segments;
      const part = [...segments].reverse().find(s => (s.pcmAt ?? s.at) <= seconds);
      const cue = part && [...part.cues].reverse().find(c => c.start <= seconds - (part.pcmAt ?? part.at));
      if (!cue) return;
      const position = this.position(cue.anchor), key = JSON.stringify(position);
      if (JSON.stringify(this.spokenPosition) !== key) { this.spokenPosition = position; this.view.save?.(position); }
      if (key === this.applied || this.following) return;
      const entry = this.current; this.following = true;
      Promise.resolve(this.view.follow(position, () => epoch === this.epoch && this.current === entry && this.wantsPlay))
        .then(applied => { if (epoch === this.epoch && entry === this.current && applied !== false) this.applied = key; })
        .catch(() => {}).finally(() => { if (entry === this.current) this.following = false; });
    }
    async advance(entry, epoch) {
      if (epoch !== this.epoch || entry !== this.current || entry.advancing) return;
      entry.advancing = true;
      entry.completed = true;
      this.state('loading');
      try {
        const next = await this.prefetch(entry, epoch);
        if (epoch !== this.epoch || entry !== this.current || !this.wantsPlay) return;
        if (next?.error) throw next.error;
        this.spokenPosition = this.position(entry.page.end); this.view.save?.(this.spokenPosition);
        if (!next) {
          this.wantsPlay = false; this.state('ended'); this.view.finish?.(this.spokenPosition); this.dispose(entry); return;
        }
        this.dispose(entry); this.activate(next, epoch);
      } catch(error) { this.fail(error, epoch); }
      finally { entry.advancing = false; }
    }
    pause() {
      this.env.clearTimeout(this.current?.pauseTimer);
      this.follow(); this.wantsPlay = false; this.state('paused');
      this.audio?.pause(); this.view.save?.(this.spokenPosition);
    }
    navigate() {
      const resume = this.wantsPlay;
      this.stop(false); this.state('paused'); return resume;
    }
    reflow() {
      this.env.clearTimeout(this.reflowTimer);
      if (!this.current || !this.view.layout || this.view.layout() === this.current.page.layout) return;
      const epoch = this.epoch;
      // Dragging the font/spacing slider must not register a new set of streams
      // every animation frame and exhaust the per-account stream bound.
      this.reflowTimer = this.env.setTimeout(() => {
        this.reflowTimer = null;
        if (epoch !== this.epoch || !this.current || this.view.layout() === this.current.page.layout) return;
        // Re-measure at the spoken anchor. A new chapter may not be in the DOM yet.
        const position = this.view.position?.();
        this.restart(position && position.chapter !== this.current.page.chapter ? this.current.page : null);
      }, 200);
    }
    fail(error, epoch) {
      if (epoch !== this.epoch) return;
      this.stop(); this.state('error', error.message || 'Не удалось подготовить страницу. Нажмите ▶');
    }
    stop(save = true) {
      if (save) { this.follow(); if (this.spokenPosition) this.view.save?.(this.spokenPosition); }
      ++this.epoch; this.wantsPlay = false;
      this.resumeRequest = null;
      this.resumeAudio = null;
      this.env.clearTimeout(this.reflowTimer); this.reflowTimer = null;
      for (const entry of [...this.entries]) this.dispose(entry);
      if (this.audio) {
        const a = this.audio;
        a.onplaying = a.onpause = a.onended = a.onwaiting = a.onerror = a.ontimeupdate = a.oncanplay = a.onloadedmetadata = a.onseeked = null;
        a.pause(); a.removeAttribute('src'); a.load();
      }
      this.audio = null; this.current = null; this.spokenPosition = null;
      try { if (this.previousAudioType !== undefined) this.env.navigator.audioSession.type = this.previousAudioType; } catch {}
      this.state('idle');
    }
  }
  if (typeof module !== 'undefined' && module.exports) module.exports = {PageVoice};
  else root.BookPageVoice = {PageVoice};
})(typeof window !== 'undefined' ? window : globalThis);
