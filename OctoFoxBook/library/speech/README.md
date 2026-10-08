# Server narration

The optional speech container reads books in Russian, English (US), German, French, Spanish (Spain) and Portuguese (Brazil). It receives bounded text through a local Unix socket and returns audio to the library. No cloud speech account is required.

The image is built from a public Python base. `requirements.txt` and `torch-requirements.txt` pin the runtime packages. `voices.json` pins every Piper model, configuration and model card to a publisher revision, byte length and SHA-256; the Silero download also verifies its SHA-256. Model cards and the Silero license are kept in `/models` in the built image.

| Language | Voice | Source |
| --- | --- | --- |
| Russian | Eugene (Silero), Ruslan (Piper) | [Silero](https://github.com/snakers4/silero-models), [Ruslan model card](https://huggingface.co/rhasspy/piper-voices/blob/1162a9173d0ce503555aed757976b7a9912eae4c/ru/ru_RU/ruslan/medium/MODEL_CARD) |
| English (US) | LJ Speech | [Model card](https://huggingface.co/rhasspy/piper-voices/blob/1162a9173d0ce503555aed757976b7a9912eae4c/en/en_US/ljspeech/medium/MODEL_CARD) |
| German | Thorsten | [Model card](https://huggingface.co/rhasspy/piper-voices/blob/1162a9173d0ce503555aed757976b7a9912eae4c/de/de_DE/thorsten/medium/MODEL_CARD), [Thorsten Voice dataset](https://github.com/thorstenMueller/Thorsten-Voice) (CC0) |
| French | SIWIS | [Model card](https://huggingface.co/rhasspy/piper-voices/blob/1162a9173d0ce503555aed757976b7a9912eae4c/fr/fr_FR/siwis/medium/MODEL_CARD), [SIWIS French Speech Synthesis Database](https://datashare.is.ed.ac.uk/handle/10283/2353) (CC BY 4.0) |
| Spanish (Spain) | DaveFX | [Model card](https://huggingface.co/rhasspy/piper-voices/blob/1162a9173d0ce503555aed757976b7a9912eae4c/es/es_ES/davefx/medium/MODEL_CARD), [voice datasets](https://github.com/OHF-Voice/voice-datasets) (CC0) |
| Portuguese (Brazil) | Faber | [Model card](https://huggingface.co/rhasspy/piper-voices/blob/1162a9173d0ce503555aed757976b7a9912eae4c/pt/pt_BR/faber/medium/MODEL_CARD), [voice datasets](https://github.com/OHF-Voice/voice-datasets) (CC0) |

Choose a voice matching the book in the reader's narration settings. Interface language, book language and voice are independent: selecting English for the interface does not translate a book or change its voice. Browser/device voices remain a separate option and depend on the installed operating-system voices.

The worker retains at most two loaded Piper voices at once to bound memory when switching languages. Audio caches include the owner, book, voice and exact page boundaries. Russian OCR correction is applied only to Russian voices, preserving accented and mixed-script text for the other voices.

Build and run from the parent directory using `docker compose --profile speech up -d --build`. The service has no network at runtime. The first build needs network access to package indexes and the model publishers.

The library uses finite page audio, with prefetch and saved text anchors. Automatic page changes and background playback still depend on the browser; a passing server health check does not verify uninterrupted iPhone playback.

`python speech/verify.py` inside the built speech image verifies all seven voices with real PCM and HLS output. Set `VERIFY_OUTPUT` to a writable directory to save WAV samples, source text and measurements. This is a technical synthesis check, not a native-speaker pronunciation review.
