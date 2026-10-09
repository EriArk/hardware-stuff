# AbyssBook reader firmware

Early **0.21.0-alpha1** firmware for the non-touch LILYGO T5 e-Paper S3
(4.7-inch H716 panel, 16 MB flash, 8 MB PSRAM).

The new Home displays real SD-card books, covers and reading progress in the
bookish design. Reading, library sections, local search, book details,
favorites, bookmarks, reading settings and explicit server sync are inherited
from the working reader firmware. The remaining screens still use the previous
visual design. This is the first functional redesign increment, not a finished
replacement for every screen.

## Controls

| Control | Action |
| --- | --- |
| UP / DOWN | Move selection; turn pages while reading. Hold to repeat. |
| OK | Open the selection; open the reading menu. |
| Hold OK | Return one level; focus the tab strip at a section root. |
| Double OK | Return Home without executing a single-click action first. |
| Sleep, short press | Light sleep; press again to wake. |
| Sleep, long press | Deep sleep; press again to wake. |

The currently fitted Sleep switch is **momentary**. For a future latching
switch, compile with `-DABYSS_SLEEP_LATCHING=1`: changing its stable position
enters deep sleep, and the opposite level wakes it. This optional configuration
has not been physically accepted yet. UP=GPIO39, DOWN=GPIO48, OK=GPIO45,
Sleep=GPIO10; switches connect to GND. Do not hold GPIO45 at reset.

Home retains a Continue card and up to two recent / newly added rows. Moving
selection to a newly added book changes the lower section so the focused item
always remains visible. Sync is the last action. Hold OK to choose a tab with
UP/DOWN and confirm it with OK.

Text uses HIGH updates and periodic hard cleanup after 24 page turns; simple
menus use FAST with cleanup after 12 navigation steps. Cover-bearing Home uses
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
