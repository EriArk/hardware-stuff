# OctoFox Companion

Companion is the administration interface served by the OctoFox book server. This folder contains a portable desktop browser for that interface: it finds a running server on this computer or your local network and displays Companion inside its window.

The desktop browser needs no installer. Installing the book server is a separate operation; Companion itself is included with that server.

## Release candidate downloads

[Download OctoFox Book 0.9.0-rc.1](https://github.com/EriArk/hardware-stuff/releases/tag/octofox-v0.9.0-rc.1). Choose the portable Companion browser for your desktop and, separately, Server Setup for the computer hosting your books. The release includes checksums, source code and known limitations.

## Open your library

Use the development package for your computer, or build it using the commands below. GitHub Actions are disabled for this repository; automatic builds are not running. Package formats:

| Computer | Package |
| --- | --- |
| Windows | Extract the `.zip` and run `OctoFox Companion.exe` inside the folder |
| macOS (Apple Silicon and Intel) | Extract the `.zip` for your processor (arm64 for Apple Silicon, x64 for Intel) and open `OctoFox Companion.app` |
| Linux x64 | Make the `.AppImage` executable and open it |

Launch **OctoFox Companion**. If one library is found, it opens automatically. If several are found, choose one. On later launches the app tries your previous library first.

For a new server, enter your name, email and the username/password you want to use. Your first account becomes the owner. **No setup key, terminal command or typed IP address is needed in the app.** For an existing library, sign in with its administrator account. Use **Library** in the top bar to read your books and **Companion** to manage the installation.

The app remembers the library and keeps its browser sign-in. It does not save your password separately. The server can expire a session or require sign-in again after a restart. **Switch library** returns to the chooser; **Search again** checks for running servers. **Connect by address** also accepts an existing HTTPS library address.

## Pair an AbyssBook reader

Requires the updated server and AbyssBook **0.21.0-alpha8** firmware. Sign in as
the server owner, connect the awake reader by USB and select **Pair reader** in
the app's top bar. Connect one reader at a time.

Choose its account and a public HTTPS address already configured for the library.
For your current signed-in account, no second password is needed. For another
account, enter that account's password once. Pairing creates a separate device
key and writes it to the reader while preserving its Wi-Fi settings. The account
password is not stored on the reader. Each device key can be revoked in this panel.

On the reader, open **Library → Settings → Wi-Fi**, then **Library → Sync**.
Books, favorites, collections, reading position and bookmarks synchronize through
the HTTPS server, including away from home. Companion can be closed after pairing.
If both sides changed position, the device position becomes current and the
previous website position remains as a bookmark.

USB pairing belongs to the bundled local app screen. The server page has no USB
or native bridge access. Beta.4 fixes physical USB discovery in Electron. USB pairing
from Windows and a real HTTPS book download have been verified on a T5 e-Paper S3.
Native macOS/Linux USB pairing still needs device acceptance.

## Server requirements

The server needs the desktop discovery update in the adjacent [library](../library/) folder. The app connects to an already running server; it does not install Docker or start a stopped server.

New installations listen on the local network by default. Existing `.env` files are preserved: a server previously bound to `127.0.0.1` can still be found on that computer, but must be changed to a LAN bind to be found from another computer. The firewall must allow the library's HTTP port and UDP **49645** on the private network. Guest Wi-Fi, VPNs and network isolation can prevent discovery. IPv4 LAN discovery is supported; a public HTTPS address can be entered manually.

During automatic first setup, the local network is trusted: a Companion app on that network can claim a new server. After setup, normal account authentication is required. Public reverse-proxy requests cannot use the local setup handshake. Once the owner signs in through the app, the detected LAN address is saved when there is room in the address list, so a phone browser can use it too.

Books, accounts, reading progress and narration remain on the server. Closing Companion leaves the server running. Changing an app connection does not change the library's public domain.

Windows users should keep the extracted files together; the executable needs the files next to it. The app stores its remembered server and browser session in the operating system's application-data folder.

These development packages are not code-signed/notarized distribution releases. OS approval prompts may appear when opening them.

## Build and check

Requires Node.js 24 and npm:

```sh
npm ci
npm test
npm run test:app
npm start
npm run dist
```

`test:app` launches a hidden native window with a disposable local server; it does not use your library accounts. Linux GUI tests need a display, for example `xvfb-run -a npm run test:app`. Run packaging on the target operating system. For a universal macOS package, use `npm run dist -- --mac --universal`. The checked-in workflow is manual-only and remains inactive while GitHub Actions are disabled.

The server page runs in a sandbox without Node.js or the app's native bridge. Only the bundled connection screen can request discovery. The app probes a bounded set of local IPv4 addresses on one UDP service port; it does not scan Internet hosts or arbitrary ports. Discovery credentials stay in memory, are bound to the direct origin and client address, and expire automatically.

The icon uses the owner's supplied OctoFox artwork.
