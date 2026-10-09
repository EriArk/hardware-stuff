# Firmware UI fonts

The inherited Roboto Condensed headers were generated from the static Android builds in the
official Roboto 3.015 release. They include the Cyrillic glyphs used by the
reader interface.

- Source: <https://github.com/googlefonts/roboto-3-classic/releases/tag/v3.015>
- License: SIL Open Font License 1.1 (`OFL.txt`)
- Files used: Regular, Medium and Bold

The firmware does not parse TTF files at runtime. `generate_reader_font.py`
converts the required Unicode subset to compressed `GFXfont` headers committed
under `include/`. The original TTF files are not needed for a normal build.

The new bookish Home uses Lora and Arimo, including Cyrillic. Their vendored
fonts, source URLs, SHA-256 hashes and OFL licenses are in `assets/fonts/`.
Run `python tools/generate_bookish_fonts.py` after building once to regenerate
the pixel-sized compressed headers. These assets are bundled at build time;
the reader does not fetch fonts from the network.
