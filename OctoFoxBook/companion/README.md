# OctoFox Companion

A desktop window for your OctoFox library. It finds a running server on this computer or your local network, then opens the server's own interface inside the app.

## Open your library

Install the build for your computer from the latest successful [Companion build](https://github.com/EriArk/hardware-stuff/actions/workflows/octofox-companion.yml). These early builds are available as GitHub Actions artifacts (GitHub sign-in required):

| Computer | Package |
| --- | --- |
| Windows | Setup `.exe`, or the portable `.zip` |
| macOS (Apple Silicon and Intel) | Universal `.dmg` or `.zip` |
| Linux x64 | `.AppImage` or `.deb` |

Launch **OctoFox Companion**. If one library is found, it opens automatically. If several are found, choose one. On later launches the app tries your previous library first.

For a new server, enter your name, email and the username/password you want to use. Your first account becomes the owner. **No setup key, terminal command or typed IP address is needed in the app.** For an existing library, sign in with its administrator account. Use **Library** in the top bar to read your books and **Companion** to manage the installation.

The app remembers the library and keeps its browser sign-in. It does not save your password separately. The server can expire a session or require sign-in again after a restart. **Switch library** returns to the chooser; **Search again** checks for running servers. **Connect by address** also accepts an existing HTTPS library address.

## Server requirements

The server needs the desktop discovery update in the adjacent [library](../library/) folder. The app connects to an already running server; it does not install Docker or start a stopped server.

New installations listen on the local network by default. Existing `.env` files are preserved: a server previously bound to `127.0.0.1` can still be found on that computer, but must be changed to a LAN bind to be found from another computer. The firewall must allow the library's HTTP port and UDP **49645** on the private network. Guest Wi-Fi, VPNs and network isolation can prevent discovery. IPv4 LAN discovery is supported; a public HTTPS address can be entered manually.

During automatic first setup, the local network is trusted: a Companion app on that network can claim a new server. After setup, normal account authentication is required. Public reverse-proxy requests cannot use the local setup handshake. Once the owner signs in through the app, the detected LAN address is saved when there is room in the address list, so a phone browser can use it too.

Books, accounts, reading progress and narration remain on the server. Closing Companion leaves the server running. Changing an app connection does not change the library's public domain.

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

`test:app` launches a hidden native window with a disposable local server; it does not use your library accounts. Linux GUI tests need a display, for example `xvfb-run -a npm run test:app`. The GitHub workflow runs the native test and builds packages separately on Windows, macOS and Linux. macOS packages are universal.

The server page runs in a sandbox without Node.js or the app's native bridge. Only the bundled connection screen can request discovery. The app probes a bounded set of local IPv4 addresses on one UDP service port; it does not scan Internet hosts or arbitrary ports. Discovery credentials stay in memory, are bound to the direct origin and client address, and expire automatically.

The icon uses the owner's supplied OctoFox artwork.
