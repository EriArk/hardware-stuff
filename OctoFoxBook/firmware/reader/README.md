# AbyssBook reader firmware

Early **0.21.0-alpha5** firmware for the non-touch LILYGO T5 e-Paper S3
(4.7-inch H716 panel, 16 MB flash, 8 MB PSRAM).

Home, Library, Search and Favorites share the bookish design: logo, visible
tabs, serif headings and light dividers. Book lists show real local covers,
authors and reading status. Library sections, author/series/genre groups,
local search and favorite folders retain their functions. Book details and
reading dialogs still use the previous visual design.

## Controls

| Control | Action |
| --- | --- |
| UP / DOWN | Move selection; turn pages while reading. Hold to repeat. |
| OK | Open the selection; open the reading menu. |
| Hold OK | Focus the tabs from content/reading screens; return one level in editors and dialogs. |
| Double OK | Return Home, except in text entry: each click enters a character. |
| Sleep | Deep sleep; press again to wake. Holding Sleep also enters deep sleep. |

The currently fitted Sleep switch is **momentary**. For a future latching
switch, compile with `-DABYSS_SLEEP_LATCHING=1`: changing its stable position
enters deep sleep, and the opposite level wakes it. This optional configuration
has not been physically accepted yet. UP=GPIO39, DOWN=GPIO48, OK=GPIO45,
Sleep=GPIO10; switches connect to GND. Do not hold GPIO45 at reset.

There is no light-sleep mode. Manual sleep and the existing 30-minute idle
timeout both save the reading context and enter deep sleep. Waking restarts
the processor and restores the context; the wake press is consumed.

Wi-Fi starts for an explicitly requested synchronization or a scan/connection
check in Settings. It stops when that operation finishes, fails or is cancelled.
Boot, wake, browsing and reading
stay offline. Legacy standalone network diagnostics and remote cover fetches
cannot enable the radio outside the sync worker (`sync-only`). USB provisioning
and local book transfer remain available without Wi-Fi.

Home retains a Continue card and up to two recent / newly added rows. Moving
selection to a newly added book changes the lower section so the focused item
always remains visible. Sync and Settings are the bottom actions. Hold OK to choose a tab with
UP/DOWN and confirm it with OK.

Tabs are a separate top navigation level, not an item in the Home list.
UP/DOWN cycles through them, including wrapping at either end; short OK opens
the selected tab. Holding OK while already on the tabs keeps that level.
Moving UP past the first Home item also focuses the tabs. The current content
selection is preserved when leaving and reopening a tab.

Nested lists have a Back control above their rows. Move UP from the first
item and press OK to return to the parent list; an empty list focuses Back
automatically. Hold OK still goes directly to the tabs. Tab focus keeps the
current section underneath the header. Lists display five rows at a time
and scroll to keep the selected row visible. Local search accepts a first
Cyrillic or Latin letter, or a digit, matching title, author or series.

## Wi-Fi settings

On Home, move down to **Настройки (Settings)**, press OK, then open **Wi-Fi**.
Choose a nearby 2.4 GHz network, enter its password and select **Подключить
(Connect)**. The device saves the network only after a successful association
and IP connection, then turns the radio off. A failed check preserves the
previous saved network and lets you edit the password. This checks Wi-Fi;
it does not establish a server/account binding.

UP/DOWN selects a keyboard row; OK enters that row. UP/DOWN then selects a
character and OK inserts it. Hold OK to return to rows, then hold again to
return to the network list. Rapid OK presses enter repeated characters while
editing, without the double-click Home gesture. Lowercase, uppercase, numbers,
all printable ASCII symbols, space, delete and password visibility are available.
Passwords accept 8–63 printable ASCII characters or a 64-digit hexadecimal PSK.

The list offers Refresh and Hidden network (manual ASCII SSID, up to 32 bytes).
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

Text uses HIGH updates and periodic hard cleanup after 24 page turns; simple
menus use FAST with cleanup after 12 navigation steps. Home and book lists use
HIGH, with hard cleanup on transitions. Framebuffer captures show intended
pixels and cannot establish physical ghosting or contrast.

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
