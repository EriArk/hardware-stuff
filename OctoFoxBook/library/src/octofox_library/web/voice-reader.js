/* Replaceable speech backend + navigation controller. No book state/storage here. */
(function (root) {
  "use strict";
  function chunks(text, block, limit = 220) {
    const result = [];
    let start = 0;
    while (start < text.length) {
      while (/\s/.test(text[start] || "") && start < text.length) start++;
      if (start >= text.length) break;
      let end = Math.min(text.length, start + limit);
      const part = text.slice(start, end);
      const sentence = [...part.matchAll(/[.!?…][»”"']?(?:\s|$)/g)].pop();
      if (sentence) end = start + sentence.index + sentence[0].trimEnd().length;
      else if (end < text.length && part.lastIndexOf(" ") > 0)
        end = start + part.lastIndexOf(" ");
      // Never split a UTF-16 surrogate pair.
      if (/[\uD800-\uDBFF]/.test(text[end - 1])) end--;
      result.push({ block, start, end, text: text.slice(start, end) });
      start = end;
    }
    return result;
  }
  class BrowserSpeech {
    constructor(env = root) {
      this.env = env;
      this.synth = env.speechSynthesis;
      this.available = !!(this.synth && env.SpeechSynthesisUtterance);
      this.rate = 0.85;
      this.voiceURI = "";
      this.lang = "ru";
      this.serial = 0;
    }
    voices() { return this.available ? this.synth.getVoices() : []; }
    cancel() {
      this.serial++;
      clearTimeout(this.timer);
      if (this.utterance) {
        this.utterance.onend = this.utterance.onerror = this.utterance.onboundary = null;
        this.utterance.onstart = null;
      }
      this.utterance = null;
      if (this.available) this.synth.cancel();
    }
    speak(text, events) {
      this.cancel();
      if (!this.available) { events.error("unavailable"); return; }
      const serial = this.serial;
      const utterance = this.utterance = new this.env.SpeechSynthesisUtterance(text);
      const voices = this.voices();
      utterance.voice = voices.find(v => v.voiceURI === this.voiceURI) ||
        voices.find(v => v.lang.toLowerCase().startsWith(this.lang.split("-")[0]) && v.localService) ||
        voices.find(v => v.lang.toLowerCase().startsWith(this.lang.split("-")[0])) || null;
      utterance.lang = utterance.voice?.lang || this.lang;
      utterance.rate = this.rate;
      const current = fn => (...args) => { if (serial === this.serial) fn(...args); };
      const fail = current(code => { clearTimeout(this.timer); events.error(code); });
      this.timer = setTimeout(() => fail("not-allowed"), 8000);
      utterance.onstart = current(() => {
        clearTimeout(this.timer);
        // Bounded chunks make an absent end event detectable without a fake audio track.
        this.timer = setTimeout(() => fail("stalled"), 90000);
      });
      utterance.onboundary = current(event => events.boundary(event.charIndex));
      utterance.onend = current(() => { clearTimeout(this.timer); events.end(); });
      utterance.onerror = current(event => fail(event.error));
      try {
        if (this.synth.paused) this.synth.resume();
        this.synth.speak(utterance); // Initial call remains inside the user's click.
      } catch { fail("synthesis-failed"); }
    }
  }
  class VoiceController {
    constructor(backend, view) {
      this.backend = backend;
      this.view = view;
      this.status = "idle";
      this.epoch = 0;
    }
    get active() { return !["idle", "ended"].includes(this.status); }
    setStatus(status, error = "") { this.status = status; this.view.state(status, error); }
    play() {
      if (!this.backend.available) { this.setStatus("error", "unavailable"); return; }
      this.epoch++;
      this.setStatus("playing");
      this.speakAtPosition();
    }
    pause() {
      this.epoch++;
      this.backend.cancel();
      this.setStatus("paused");
      this.view.save();
    }
    stop() {
      this.epoch++;
      this.backend.cancel();
      this.setStatus("idle");
      this.view.save();
    }
    seek(position, resume = this.status === "playing") {
      this.epoch++;
      this.backend.cancel();
      this.view.follow(position);
      if (resume) { this.setStatus("playing"); this.speakAtPosition(); }
      else if (this.active) this.setStatus("paused");
    }
    speakAtPosition() {
      if (this.status !== "playing") return;
      const position = this.view.position();
      const segments = this.view.segments();
      const segment = segments.find(s => s.block > position.block ||
        (s.block === position.block && s.end > position.char));
      if (!segment) { this.advance(); return; }
      const start = segment.block === position.block ? Math.max(segment.start, position.char) : segment.start;
      const text = segment.text.slice(start - segment.start);
      const epoch = this.epoch;
      this.view.follow({ block: segment.block, char: start });
      this.backend.speak(text, {
        boundary: index => {
          if (epoch !== this.epoch || this.status !== "playing") return;
          if (Number.isInteger(index) && index >= 0 && index < text.length)
            this.view.follow({ block: segment.block, char: start + index });
        },
        end: () => {
          if (epoch !== this.epoch || this.status !== "playing") return;
          this.view.follow({ block: segment.block, char: segment.end });
          this.speakAtPosition();
        },
        error: code => {
          if (epoch !== this.epoch) return;
          this.epoch++;
          this.backend.cancel();
          this.setStatus("error", code);
          this.view.save();
        },
      });
    }
    async advance() {
      const epoch = this.epoch;
      this.setStatus("loading");
      try {
        const more = await this.view.next(() => epoch === this.epoch);
        if (epoch !== this.epoch) return;
        if (!more) {
          this.backend.cancel();
          this.setStatus("ended");
          await this.view.finish();
          return;
        }
        this.setStatus("playing");
        this.speakAtPosition();
      } catch {
        if (epoch !== this.epoch) return;
        this.setStatus("error", "load-failed");
      }
    }
  }
  root.BookVoice = { BrowserSpeech, VoiceController, chunks };
  if (typeof module !== "undefined") module.exports = root.BookVoice;
})(globalThis);
