# OctoFox display comparison lab

An experimental screen-only firmware for the **non-touch LILYGO
Screen-4.7-S3 V2.4 / H716**, 960 × 540. It compares display transactions using
the same six pre-rendered targets. This is not the reader application.

Two separate build targets avoid running two display drivers at once:

- `painter`: pinned EPD_Painter H716 preset, four shades, direct grey-to-grey
  transitions disabled as in the existing reader. HIGH, NORMAL and FAST quality;
  optional periodic HARD, SOFT or local SOFT cleaning of changed areas.
- `lilygo`: pinned official ESP32-S3 driver. HIGH uses the standard full clear;
  NORMAL uses two clear cycles at 45; FAST uses one at 30. All three draw the
  whole frame and switch off the panel afterward. They are reference sequences,
  not equivalent waveforms to the Painter quality names.

EPDiy is **not implemented** here. Its classic ESP32 EPD47 board and newer
T5 S3 Pro board definitions are not drop-in profiles for this H716 board.

## Preserve the installed reader first

Before installing this lab, save and verify the device's existing flash, identify
its active application partition, and retain a tested restoration route. A full
flash backup may contain Wi-Fi/account credentials: keep it private, outside Git.
Do not use erase-flash or replace the partition table as part of this comparison.
Write only the confirmed application slot, with a matching bootloader/layout.
Restore that application's original bytes after testing.

The lab does not mount microSD, start Wi-Fi, provision accounts or write application
settings. It waits for an explicit `LAB START` command before drawing. The upstream
driver may read its own calibration configuration. Normal reader buttons are not
implemented; USB controls the experiment. Do not leave this firmware installed
as a daily reader.

## Build

PlatformIO Core 6.1.19 and the pinned Espressif32 6.12.0 platform:

```sh
pio run -e painter
pio run -e lilygo
```

The board JSON follows the existing reader/LILYGO board definition. The supplied
targets use identical four-shade pixel values, converted to each driver's input
format. They contain original test text, not private books. PNGs in `fixtures/`
are intended targets; they cannot show physical ghosting.

Regenerate with Pillow and a local Cyrillic-capable TrueType font:

```sh
python tools/generate_frames.py --font /path/to/font.ttf
```

## One trial at a time

Install `pyserial` on the host. Once the lab is running:

```sh
python tools/run_trial.py --quality HIGH --cleanup HARD --every 6 --steps 11 --output results/high-hard6.json
python tools/run_trial.py --quality HIGH --cleanup HARD --every 12 --steps 11 --output results/high-hard12.json
python tools/run_trial.py --quality HIGH --cleanup NONE --every 0 --steps 23 --output results/high-none.json
python tools/run_trial.py --quality HIGH --cleanup SOFT --every 6 --steps 11 --output results/high-soft6.json
```

Inspect the panel between commands. Start with HIGH; compare NORMAL only after
an acceptable cleaning interval is found. FAST is a contrast/speed reference,
not a suggested default for text. For local changes use `--scene MENU`; LOCAL
clears the changed rectangles, which may span much of the screen for text pages.
It does not guarantee removal of residue outside those rectangles.

For the `lilygo` build use `--cleanup NONE --every 0`: that means no additional
periodic policy; the backend still clears the full area on **every frame**.

Each start performs a strong baseline clear and shows target 1 (or menu target 5).
Text cycles through targets 1–4, menu through 5–6. STEP counts are page transitions,
not target numbers. At most 48 transitions are allowed before another START.
Use 5/11/23 steps to inspect just before a proposed 6/12/24-transition cleanup.
Use 6/12/24 to measure the cleanup itself. USB never automatically advances to
another profile. STOP leaves the final image visible.

Measurements include decode, cleanup, paint-completion and total times. Host
round-trip includes USB overhead; it is not physical button latency. First-frame
cleanup is excluded from trial statistics. Compare ordinary transitions and
cleanup transitions separately, not just the median.

Record physical observations for each profile: text darkness, residue in white
gaps, gray patches, obvious flashes and final target number. Photograph under
the same lighting/exposure. A successful USB response or framebuffer capture
does not establish image quality. No default reader policy should be changed
until a physical comparison has been accepted.

## USB protocol

At 115200 baud, native CDC DTR on / RTS off:

```text
LAB STATUS
LAB START HIGH HARD 6 TEXT
LAB STEP
LAB STOP
```

START syntax: `LAB START <HIGH|NORMAL|FAST> <NONE|HARD|SOFT|LOCAL>
<0..24> <TEXT|MENU>`. NONE requires 0; other policies require 1..24. Each
completed frame reports timing and the actual cleanup. A Painter timeout latches
a fault until reset; no further frames are submitted.
