# Server narration

The optional speech container provides the existing Russian Piper and Silero voices. It receives bounded text through a local Unix socket and returns audio to the library. No cloud speech account is required.

The image is built from a public Python base, rather than another project's private image. `requirements.txt` and `torch-requirements.txt` pin the runtime packages. `voices.json` pins Piper downloads to a publisher revision and SHA-256; the Silero download also verifies its SHA-256. Model cards and the Silero license are kept in `/models` in the built image. The current backend uses the Russian models; the inherited Piper manifest also includes an English model, which is not yet exposed as a supported voice.

Build and run from the parent directory using `docker compose --profile speech up -d --build`. The service has no network at runtime. The first build needs network access to package indexes and the model publishers.

The library uses finite page audio, with prefetch and saved text anchors. Automatic page changes and background playback still depend on the browser; a passing server health check does not verify uninterrupted iPhone playback.
