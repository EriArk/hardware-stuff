/* Native audio/HLS server narration; browser speech remains a separate choice. */
(function (root) {
  'use strict';
  class ServerVoice {
    constructor(api, view, info, env = root) {
      this.api = api; this.view = view; this.env = env;
      this.status = 'idle'; this.epoch = 0; this.engine = 'server';
      this.backend = {available: !!info.available, rate: 0.85, voiceURI: info.voice || 'eugene',
        voices: () => (info.voices || []).map(v => ({...v, localService: true}))};
    }
    get active() { return !['idle', 'ended'].includes(this.status); }
    setRate(rate) {
      if (!Number.isFinite(rate) || rate < 0.5 || rate > 2) return;
      this.backend.rate = rate;
      if (this.audio) this.audio.playbackRate = rate;
    }
    state(value, error = '') { this.status = value; this.view.state(value, error); }
    signature() { return JSON.stringify(this.view.position()); }
    play() {
      if (this.audio && this.voice === this.backend.voiceURI &&
          (this.position === this.signature() || JSON.stringify(this.spokenPosition) === this.signature()) &&
          ['paused', 'playing'].includes(this.status)) {
        this.audio.playbackRate = this.backend.rate;
        this.wantsPlay = true;
        this.startAudio(this.epoch);
        return;
      }
      // Capture the reader's choice before disposing the old audio. Cleanup
      // must not follow/save an old cue over a newly selected page.
      const position = structuredClone(this.view.startPosition?.() || this.view.position());
      this.stop(false);
      const epoch = this.epoch;
      const id = this.id = this.env.crypto.randomUUID();
      this.voice = this.backend.voiceURI;
      this.position = this.signature(); this.wantsPlay = true; this.info = null; this.part = null;
      this.retryMedia = false; this.mediaFailures = 0; this.nextPart = null;
      this.recoverAt = null; this.atEnd = false; this.statusFailures = 0;
      this.following = null; this.spokenPosition = null;
      this.startPosition = position; this.initialSeek = true;
      this.audio = new this.env.Audio();
      const audio = this.audio;
      this.native = !!audio.canPlayType('application/vnd.apple.mpegurl');
      audio.preload = 'auto'; audio.playbackRate = this.backend.rate;
      audio.preservesPitch = true;
      audio.onplaying = () => {
        if (epoch !== this.epoch) return;
        if (!this.wantsPlay) { audio.pause(); return; }
        this.applySeek(); this.atEnd = false; this.state('playing'); this.follow(epoch);
      };
      audio.onwaiting = () => { if (epoch === this.epoch && this.wantsPlay) this.state('loading'); };
      audio.onpause = () => { if (epoch === this.epoch && this.status === 'playing') this.state('paused'); };
      audio.ontimeupdate = () => this.follow(epoch);
      audio.onloadedmetadata = () => {
        if (epoch !== this.epoch) return;
        if (this.native) this.applySeek();
        else { try { audio.currentTime = this.partSeek || 0; } catch {} }
      };
      audio.oncanplay = () => { if (epoch === this.epoch) { this.applySeek(); this.follow(epoch); } };
      audio.onerror = () => {
        if (epoch !== this.epoch) return;
        if (++this.mediaFailures > 3) return this.fail('Не удалось воспроизвести аудио. Нажмите ▶ или выберите другой голос');
        // The cache may not have been ready before the first media request timed out.
        if (this.native && audio.currentTime > 0) this.recoverAt = audio.currentTime;
        this.retryMedia = true;
        this.state('loading');
      };
      audio.onended = () => {
        if (epoch !== this.epoch) return;
        this.atEnd = true;
        if (this.native) {
          this.recoverAt = audio.currentTime;
        } else {
          this.nextPart = (this.part ?? 0) + 1;
        }
        // A drained live buffer is not the end of a book. Refresh the manifest
        // state before finishing, and preserve the last audible position.
        this.state('loading'); this.poll(epoch);
      };
      this.state('loading');
      try {
        if (this.env.navigator?.audioSession) {
          this.previousAudioType = this.env.navigator.audioSession.type;
          this.env.navigator.audioSession.type = 'playback';
        }
      } catch {}
      this.api(`/books/${position.book}/speech`, {id, chapter: position.chapter, anchor: position.anchor,
        voice: this.voice}).then(() => {
          if (epoch !== this.epoch) return this.api(`/speech/streams/${id}/stop`, {}).catch(() => {});
          this.poll(epoch);
        }).catch(e => { if (epoch === this.epoch) this.fail(e.message); });
      if (this.native) {
        audio.src = `/reader-api/speech/streams/${id}/audio.m3u8`;
        this.startAudio(epoch); // Same user gesture; no speech-synthesis on iPhone.
      }
    }
    applySeek() {
      if (!this.audio || !this.native) return;
      const target = this.recoverAt ?? (this.initialSeek ? 0 : null);
      if (target === null) return;
      try {
        this.audio.currentTime = target;
        this.recoverAt = null; this.initialSeek = false;
      } catch {} // Metadata/can-play will retry if the timeline is not seekable yet.
    }
    navigate() {
      const resume = this.wantsPlay && ['playing', 'loading'].includes(this.status);
      this.stop(false);
      this.state('paused');
      return resume;
    }
    startAudio(epoch) {
      if (!this.audio || !this.wantsPlay) return;
      this.audio.playbackRate = this.backend.rate;
      Promise.resolve(this.audio.play()).catch(error => {
        if (epoch !== this.epoch) return;
        if (error.name === 'NotAllowedError') {
          this.wantsPlay = false;
          this.state('paused', 'Готово. Нажмите ▶ для воспроизведения');
        } else if (error.name !== 'AbortError') {
          if (++this.mediaFailures > 3) return this.fail('Не удалось воспроизвести аудио. Нажмите ▶ ещё раз');
          this.retryMedia = true; this.state('loading');
        }
      });
    }
    async poll(epoch) {
      if (epoch !== this.epoch || this.polling === epoch) return;
      this.env.clearTimeout(this.timer);
      this.polling = epoch;
      try {
        const data = await this.api(`/speech/streams/${this.id}/status`);
        if (epoch !== this.epoch) return;
        this.info = data;
        this.statusFailures = 0;
        const last = data.segments.at(-1), end = last ? last.at + last.duration : 0;
        const exhausted = this.atEnd && (this.native
          ? this.audio.currentTime >= end - 0.1 : this.part === last?.index);
        if (data.complete && exhausted) {
          const cue = last?.cues.at(-1);
          if (cue) {
            this.spokenPosition = {book: this.view.position().book, chapter:last.chapter,
              anchor:cue.endAnchor || cue.anchor};
            Promise.resolve(this.view.follow(this.spokenPosition, () => epoch === this.epoch)).catch(() => {});
          }
          if (epoch !== this.epoch) return;
          this.wantsPlay = false; this.state('ended');
          this.view.save?.(this.spokenPosition); this.view.finish?.(this.spokenPosition);
          return;
        }
        const stalled = this.status === 'loading' && this.native && this.audio.currentTime >= end - 0.5;
        if (data.blocked?.failed && (exhausted || stalled || !last)) {
          this.fail('Не удалось озвучить следующий фрагмент. Позиция сохранена. Нажмите ▶ для повтора');
          return;
        }
        if (data.state === 'ready') {
          if (this.native && (this.retryMedia || (this.atEnd && end > this.audio.currentTime + 0.1))) {
            this.retryMedia = false;
            this.audio.load(); this.startAudio(epoch);
          } else if (!this.native) {
            const part = data.segments.find(s => s.index === (this.nextPart ?? this.part)) ||
              (this.part === null ? data.segments[0] : null);
            if (part && (part.index !== this.part || this.retryMedia)) {
              const first = this.part === null;
              this.part = part.index; this.partSeek = first ? data.seek : 0;
              this.nextPart = null; this.retryMedia = false;
              this.audio.src = `/reader-api/speech/streams/${this.id}/${part.index}.wav`;
              this.startAudio(epoch);
            }
          }
          this.follow(epoch);
        }
      } catch (error) {
        if (epoch === this.epoch && ++this.statusFailures >= 3) this.fail(error.message);
      } finally {
        if (this.polling === epoch) this.polling = null;
        if (epoch === this.epoch && this.active) this.timer = this.env.setTimeout(() => this.poll(epoch), 2500);
      }
    }
    follow(epoch) {
      if (epoch !== this.epoch || !this.wantsPlay || !this.info?.segments.length || !this.audio ||
          this.status !== 'playing' || this.audio.seeking || (this.native && this.initialSeek)) return;
      const seconds = this.audio.currentTime;
      const part = this.native
        ? [...this.info.segments].reverse().find(s => s.at <= seconds)
        : this.info.segments.find(s => s.index === this.part);
      if (!part) return;
      const at = seconds - (this.native ? part.at : 0);
      const cue = [...part.cues].reverse().find(c => c.start <= at) || part.cues[0];
      if (!cue) return;
      const position = {book: this.startPosition.book, chapter: part.chapter, anchor: cue.anchor};
      const start = this.startPosition;
      if (position.chapter < start.chapter || (position.chapter === start.chapter &&
          (position.anchor.block < start.anchor.block || (position.anchor.block === start.anchor.block && position.anchor.char < start.anchor.char)))) return;
      const signature = JSON.stringify(position);
      if (signature === this.position) return;
      if (JSON.stringify(this.spokenPosition) !== signature) {
        this.spokenPosition = position;
        // Persist audio progress even if the DOM is waiting on a chapter/layout.
        this.view.save?.(position);
      }
      // A chapter can still be loading when a newer cue arrives. Do not mark
      // that cue applied until the view confirms it; retry on the next tick.
      if (this.following === epoch) return;
      this.following = epoch;
      Promise.resolve(this.view.follow(position, () => epoch === this.epoch && this.wantsPlay)).then(applied => {
        if (epoch === this.epoch && this.wantsPlay && applied !== false) this.position = signature;
      }).catch(() => {}).finally(() => {
        if (this.following === epoch) this.following = null;
      });
    }
    pause() {
      this.follow(this.epoch); this.wantsPlay = false;
      this.audio?.pause(); this.state('paused'); this.view.save?.(this.spokenPosition);
    }
    fail(message) {
      this.stop(); this.state('error', message || 'Не удалось подготовить озвучку');
    }
    stop(save = true) {
      if (save && this.audio) { this.follow(this.epoch); this.view.save?.(this.spokenPosition); }
      ++this.epoch; this.env.clearTimeout(this.timer); this.wantsPlay = false;
      this.spokenPosition = null;
      const id = this.id; this.id = null;
      const audio = this.audio; this.audio = null;
      if (audio) {
        audio.onplaying = audio.onwaiting = audio.onpause = audio.onended = audio.onerror = audio.ontimeupdate = audio.onloadedmetadata = audio.oncanplay = null;
        audio.pause(); audio.removeAttribute('src'); audio.load();
      }
      if (id) this.api(`/speech/streams/${id}/stop`, {}).catch(() => {});
      try {
        if (this.previousAudioType !== undefined) this.env.navigator.audioSession.type = this.previousAudioType;
      } catch {}
      this.state('idle');
    }
  }
  if (typeof module !== 'undefined' && module.exports) module.exports = {ServerVoice};
  else root.BookServerVoice = {ServerVoice};
})(typeof window !== 'undefined' ? window : globalThis);
