# OctoFox Server Setup

A separate installer for the book server, including Library, narration and the server-side Companion panel. The portable Companion browser is a separate download and does not need installation.

## Release candidate downloads

[Download OctoFox Book 0.9.0-rc.1](https://github.com/EriArk/hardware-stuff/releases/tag/octofox-v0.9.0-rc.1). Choose the portable Companion browser for your desktop and, separately, Server Setup for the computer hosting your books. The release includes checksums, source code and known limitations.

## Install your server

Open **OctoFox-Server-Setup** on the computer that will host your books. The setup page opens in your browser. Keep the suggested folder and narration option, then choose **Install server**.

The installer checks the computer, prepares missing runtime components, copies the server files, generates private settings and waits for the services to become healthy. If a standard HTTP port is occupied, an available port is selected automatically. Existing installations keep their settings and persistent data.

Your operating system may ask for administrator permission. On Windows, WSL setup may require a restart: reopen the installer afterwards and your folder/options will be remembered. Docker Desktop presents its own terms when needed; accept them there to continue. The installer does not accept third-party terms on your behalf.

When setup says **Your server is ready**, open the [portable Companion browser](../companion/). It finds the server and displays its Companion panel. Create the first owner account there, then upload your books. Closing the installer or the Companion browser leaves the server running.

## Supported hosts

This server package currently targets **x64 Linux containers**. The portable browser's platform support is independent of the server's requirements.

| Server computer | Automatic runtime preparation |
| --- | --- |
| Windows x64 | Official Docker Desktop installer, WSL installation/update, private-LAN firewall rules |
| Ubuntu / Debian x64 | Docker Engine, Compose and Buildx from Docker's signed package repository; service startup and current-user Docker access |
| Intel macOS | Official Docker Desktop download and signed application installation; macOS confirmation when needed |
| Other Linux distributions | An existing local Docker Engine and Compose 2.20+ are required |

Windows and Linux standalone packages are currently available as development builds. The macOS installer path is implemented in source; a packaged macOS installer and clean-machine dependency installation still require platform acceptance testing. These packages are not signed/notarized releases.

An Internet connection is needed for initial runtime/container/voice downloads. Hardware virtualization must be available for Docker Desktop; software cannot enable a disabled BIOS setting. System restarts and permission/terms prompts are shown explicitly rather than reported as a successful installation.

The server listens on the local network. Windows rules are restricted to the Private network profile and local subnet. Existing Linux firewall policies are preserved; allow the selected library TCP port and UDP 49645 on the private network if a firewall blocks discovery. UDP 49645 must be available for automatic discovery.

## Existing data and repeat runs

Choose an empty folder for a new installation. Repeat runs recognize folders created by this installer, retain `.env`, and keep accounts, books, reading positions and enabled narration. Unrelated folders, modified server files and different server versions are preserved and rejected. This installer does not implement a version-upgrade or data-migration workflow.

Books and accounts live in persistent Docker volumes; the chosen folder contains server files and private configuration. Keep both the volumes and `.env` when backing up or moving the server. Stopping a server does not delete its volumes. Linux starts Docker as a service; on Windows/macOS the server resumes when Docker Desktop starts.

## Servers without a desktop

Run the standalone Linux binary on the server:

```sh
chmod +x OctoFox-Server-Setup
./OctoFox-Server-Setup --headless
```

It prepares missing runtime packages with the system's `sudo` prompt when required. No account password is collected by the web installer. Optional flags: `--directory /absolute/path`, `--no-speech`, `--port 8080`, `--admin-port 8081`. `--headless --check` only checks the computer and does not install dependencies or start services.

The installed `tools/manage.py` remains available to operators with Python for status/start/stop; pass the Docker project name recorded in `.octofox-install.json` using `--project`. The source launcher is documented in [Library](../library/).

## Build from source

Python 3.12+ is needed only for development/source runs; standalone installers include Python. Build on the target OS:

```sh
python -m pip install -r requirements-build.txt
python build.py
python -m unittest discover -s tests -v
```

`dist/` receives the installer. To run from source, use `python installer.py`. UI verification uses `tests/verify-ui.cjs` with Playwright and a disposable fixture, without installing system components. GitHub Actions remain disabled.

Runtime installation follows the official guides for [Windows](https://docs.docker.com/desktop/setup/install/windows-install/), [macOS](https://docs.docker.com/desktop/setup/install/mac-install/), [Ubuntu](https://docs.docker.com/engine/install/ubuntu/) and [Debian](https://docs.docker.com/engine/install/debian/). Standalone packaging uses [PyInstaller](https://pyinstaller.org/en/stable/usage.html).
