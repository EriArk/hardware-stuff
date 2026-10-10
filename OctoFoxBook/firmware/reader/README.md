# AbyssBook reader firmware

Early **0.21.0-alpha9** firmware for the non-touch LILYGO T5 e-Paper S3
(4.7-inch H716 panel, 16 MB flash, 8 MB PSRAM).

Home, Library, Search, Collections and book details share the bookish design: logo, visible
tabs, serif headings and light dividers. Book lists show real local covers,
authors and reading status. Library sections, author/series/genre groups,
local search, annotations and collections retain their functions. Reading menus
still use the previous visual design. Interface redraws use HIGH quality, with
cleanup on content changes and after four UI paints; continuous text reading
keeps its separate refresh policy.

## Controls

| Control | Action |
| --- | --- |
| UP / DOWN | Move selection; turn pages while reading. Hold to repeat. |
| OK | Open the selection; open the reading menu. |
| Hold OK | Focus the current screen's owning tab, including from editors and dialogs. Fires after 800 ms while still held; release adds no action. |
| Double OK | Return one level, including in text entry. From tab focus, restore the underlying section. |
| Sleep | Deep sleep; press again to wake. Holding Sleep also enters deep sleep. |

Choose **Library → Settings → Sleep → Sleep button type** for a momentary
button (default) or a latching switch. The setting persists without rebuilding.
For latching switches, changing the stable position enters deep sleep; changing
it again wakes the reader. The current physical button is momentary; the latching
hardware path still needs an actual-switch acceptance test. UP=GPIO39, DOWN=GPIO48, OK=GPIO45,
Sleep=GPIO10; switches connect to GND. Do not hold GPIO45 at reset.

There is no light-sleep mode. Manual sleep and the configurable idle timeout
(30 minutes by default) both save the reading context and enter deep sleep. Waking restarts
the processor and restores the context; the wake press is consumed.

### Low-battery protection

At an estimated **15%**, a dismissible warning appears over the current screen,
including reading, settings and synchronization. OK dismisses it without acting
on the screen underneath. It warns again after a recharge to at least 20% and a
new discharge. At **8% or below**, three consecutive averaged samples trigger
protection: cancel network work, wait for file writers, preserve reading context,
show the charge screen, turn off the display circuitry and enter deep sleep.
Telemetry continues during sync. Samples are taken every five seconds.

Press Sleep after charging. A protected reader only resumes when its estimate
is **above 12%**; otherwise it returns to protection sleep. Resume context is also
stored in NVS to survive battery removal; page position and bookmarks are already
persisted when changed. Percentages are voltage estimates, not a fuel gauge.

**This is not a complete electrical power disconnect.** The H716 board cannot
switch off its own main regulator in software and has no VBUS wake input.
Connecting USB alone does not automatically wake this protection mode. True
power-off with USB-start requires a hardware change. Battery protection circuitry
is still necessary; firmware does not replace it.

Wi-Fi starts for an explicitly requested synchronization or a scan/connection
check in Settings. It stops when that operation finishes, fails or is cancelled.
Boot, wake, browsing and reading
stay offline. Legacy standalone network diagnostics and remote cover fetches
cannot enable the radio outside the sync worker (`sync-only`). USB provisioning
and local book transfer remain available without Wi-Fi.

Home retains a Continue card and up to two recent / newly added rows. Moving
selection to a newly added book changes the lower section so the focused item
always remains visible. Sync and Settings are at the bottom of Library, outside Home. Hold OK to choose a tab with
UP/DOWN and confirm it with OK.

Tabs are a separate top navigation level, not an item in the Home list.
UP/DOWN cycles through them, including wrapping at either end; short OK opens
the selected tab. Holding OK while already on the tabs keeps that level.
Moving UP past the first Home item also focuses the tabs. The current content
selection is preserved when leaving and reopening a tab.

Nested lists have a Back control above their rows. Move UP from the first
item and press OK to return to the parent list; an empty list focuses Back
automatically. Hold OK still goes directly to the tabs. Tab focus keeps the
current section underneath the header. Lists display four larger rows at a time
and scroll to keep the selected row visible. Local search accepts a first
Cyrillic or Latin letter, or a digit, matching title, author or series.

## Settings and language

Open **Library → Settings**. Every settings page has a visible **Back** row.
Available sections are Reading (text size and line spacing), Screen (reading
and interface cleanup intervals, clear now), Sleep (5/15/30/60 minutes and switch
type), Language, Wi-Fi, Library connection, About and Keyboard languages.

English is the default; Russian is available in **Language**. The selection
persists across restarts. Book text, titles and personal collection names keep
their original language. Firmware updates preserve existing saved preferences.

## Wi-Fi settings

At the bottom of Library, select **Settings**, press OK,
then open **Wi-Fi**. Library also has a **Синхронизировать (Sync)** row at the bottom.
Choose a nearby 2.4 GHz network, enter its password and select **Подключить
(Connect)**. The device saves the network only after a successful association
and IP connection, then turns the radio off. A failed check preserves the
previous saved network and lets you edit the password. This checks Wi-Fi;
it does not establish a server/account binding.

## Favorites and collections

The Collections tab contains a separate Favorites list and named collections.
Use **Новая коллекция (New collection)** to enter a name on the reader, with
any enabled keyboard layout and symbols. Double OK leaves character selection;
double OK again cancels the editor. Holding OK goes directly to the owning tab.
In a book card, Favorites toggles independently;
**В коллекции (Collections)** lets a book belong to several collections.

Explicit synchronization exchanges collections with the linked account on an
updated OctoFox server. Pending changes survive restarts and failed requests.
The device receives collection membership for books delivered to that device;
creating a collection does not download its books. Books copied solely over USB
retain their memberships locally. The original favorites file is preserved during
migration. Renaming/deleting collections is currently available on the website.
Changing the linked account requires resolving the old collection binding first;
the reader refuses to upload one account's saved collection changes to another.

UP/DOWN selects a keyboard row; OK enters that row. UP/DOWN then selects a
character and OK inserts it. Double OK returns to rows, then double OK again
returns to the network list. A single OK waits 350 ms for a possible second
click; wait for the insertion before repeating a character. Lowercase, uppercase, numbers,
all printable ASCII symbols, space, delete and password visibility are available.
Passwords accept 8–63 UTF-8 bytes or a 64-digit hexadecimal PSK. The access point
must use the same password bytes; Unicode normalization is not applied.

**Keyboard languages** is independent of the interface language. English and
Russian are initially enabled; German, French, Spanish, Portuguese and Italian
can be added individually. English stays available. Select the EN/RU/etc. key
in the fourth row and press OK to cycle through enabled layouts without losing
the entered text. The same layouts work for collection names and Wi-Fi entry.

The list offers Refresh and Hidden network (manual UTF-8 SSID, up to 32 bytes).
Open networks need no password; leave the password blank for a hidden open
network. WEP and enterprise authentication are explicitly unsupported.
Exiting Settings or entering deep sleep cancels radio work and clears the draft.
Scans and connection checks have an 18-second timeout. Saved credentials use
an independent atomic NVS record, preserving existing server credentials;
older provisioned Wi-Fi remains readable until a new network is saved.

`SETTINGS OPEN`, `SETTINGS CLOSE` and `WIFI STATUS` are USB diagnostics.
Other mutating USB commands are blocked while Settings owns the network session.
The optional `tools/render_wifi_settings.py OUTPUT_DIRECTORY` uses the production
layout and bundled fonts to create host previews (requires Pillow); these do
not demonstrate physical E-Ink quality or successful radio association.

By default, text uses HIGH updates and periodic hard cleanup after 24 page turns; simple
menus use FAST with cleanup after 12 navigation steps. Home and book lists use
HIGH, with hard cleanup on transitions. Framebuffer captures show intended
pixels and cannot establish physical ghosting or contrast.

## Pairing and reading-state synchronization

In the updated desktop Companion, sign in as the server owner and open **Pair
reader**. Connect the awake reader by USB, select its account and your configured
public HTTPS library address, then pair it. The current signed-in account needs
no second password; another reader account requires its password once. Companion
writes a separate revocable device key, not the account password, and preserves
the reader's Wi-Fi settings. Configure Wi-Fi on the reader before the first sync.

**Library → Sync** exchanges books, favorites, collections, reading position,
finished status and bookmarks. Positions use logical text anchors rather than
page numbers, so font changes do not shift the saved place. Bookmarks support
up to 500 entries per book. Local USB-only books stay local.

If both the website and device moved since the last exchange, the device's
position becomes current and the previous website position is kept as a bookmark.
Failed requests can be retried without duplicating bookmark changes. The server
checks the book content and account before applying state. An incompatible book
copy, unsupported anchor or older percentage-only position stops the exchange
for review rather than guessing a place. Opening the book on the website and
saving a new text position can replace an older percentage-only position.

The configured HTTPS address works over any supported Wi-Fi network with Internet
access; Companion need not remain running. Device access can be revoked from its
Pair reader panel. Replacing an account does not automatically migrate the old
account's local state. Alpha8 preserves older local books that are absent from the
paired server and have never synchronized reading state. Windows USB pairing and
a real HTTPS book download have been verified on a T5 e-Paper S3. Native macOS/Linux
USB pairing and broader device acceptance remain unverified.

## Build and test

Use Python 3, PlatformIO and, for host tests, a C++17 `g++` on PATH:

```sh
python -m pip install -r requirements-dev.txt
pio run
python -m unittest discover -s tools -p 'test_*.py'
```

The platform and both display libraries are pinned in `platformio.ini`.
The build applies a hash-checked H716 power fix to EPD Painter: after its idle
timeout, it also disables the peripheral supply shared with the blue LED.
The supply is restored for display updates, so the LED can still light while
refreshing. USB charging indicators are hardware-controlled. This change
requires an on-battery hardware check; a successful build does not prove LED
state or standby current on a particular board revision.
The same build hook places the large display buffer in PSRAM directly, keeping
internal RAM available for synchronization without briefly enabling Wi-Fi at boot.
The application is `.pio/build/t5_epaper_s3_fast/firmware.bin`. Generated font
headers are committed; normal builds do not need font conversion.

Preserve the installed partition table, NVS settings and SD card when updating
an existing device. Confirm the active application slot and save a verified
rollback copy covering every sector that the new application will erase. Do
not use a full-chip erase or a generic upload that replaces boot/partition data
as an application-only update. USB boot/reset behavior differs between boards.

Credentials are provisioned separately with `tools/provision_reader.py` and
stored on the device. A private `.env` is ignored by Git; credentials are never
build flags. Firmware updates do not require reprovisioning an existing device.

USB tools require an explicitly chosen serial port. `test_home_navigation.py`
exercises the current Home/reading/tab route using USB input events; it does
not prove physical button operation. Other hardware scripts were inherited
from the earlier firmware and may contain assertions for its old navigation.

## Source and fonts

Application code was migrated from the author's `mybookopds` reader work.
The reader keeps the `abyss-reader` USB identity for existing tooling.
Lora and Arimo are distributed under SIL OFL; licenses and exact source hashes
are in `assets/fonts/`. Inherited Roboto headers are covered by
`tools/fonts/OFL.txt`. The logo is the project's original artwork.
