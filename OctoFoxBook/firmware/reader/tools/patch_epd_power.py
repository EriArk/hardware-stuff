"""Apply the reviewed H716 idle-power fix to the pinned EPD Painter source.

Runs after PlatformIO resolves dependencies. Only the exact upstream source or
our exact patched result is accepted; a dependency update requires review.
"""
from hashlib import sha256
from pathlib import Path

UPSTREAM_SHA256 = "2cf04263b387d9a617514260e6cfc2c2062f937149891d42d451e59ca09955dc"
STARTUP_OLD = "  // Safe startup state: power disabled, scan direction set.\n  _pin_pwr_dis.set(true);\n  _pin_scan_dir.set(true);"
STARTUP_NEW = "  // H716 QP5 also enables the peripheral 3V3 rail and blue LED.\n  _pin_pwr_dis.set(true);\n  _pin_scan_dir.set(false);"
OFF_OLD = "  _pin_pwr_dis.set(true);    // re-engage power-disable\n}"
OFF_NEW = "  _pin_pwr_dis.set(true);    // re-engage power-disable\n  // Remove peripheral 3V3 only after the panel drive supplies are off.\n  // powerOn() restores QP5 before enabling those supplies again.\n  _pin_scan_dir.set(false);\n}"
PAINTER_SHA256 = "df155706d69c6904ad7a474568467e2c61ad252094bce3ae83ce81dd1d619f98"
BUFFER_OLD = "  packed_fastbuffer = static_cast<uint8_t *>(\n    heap_caps_aligned_alloc(16, packed_size, MALLOC_CAP_INTERNAL));"
BUFFER_NEW = "  // Keep internal RAM available for on-demand Wi-Fi without booting the radio.\n  packed_fastbuffer = static_cast<uint8_t *>(\n    heap_caps_aligned_alloc(16, packed_size, MALLOC_CAP_SPIRAM));"


def patched_memory_source(source: str) -> str:
    source = source.replace("\r\n", "\n")
    original = source.replace(BUFFER_NEW, BUFFER_OLD)
    if sha256(original.encode("utf-8")).hexdigest() != PAINTER_SHA256:
        raise RuntimeError("EPD Painter allocator changed; review the PSRAM patch before building")
    if original.count(BUFFER_OLD) != 1:
        raise RuntimeError("Expected packed framebuffer allocation exactly once")
    return original.replace(BUFFER_OLD, BUFFER_NEW)


def patched_source(source: str) -> str:
    source = source.replace("\r\n", "\n")
    original = source.replace(STARTUP_NEW, STARTUP_OLD).replace(OFF_NEW, OFF_OLD)
    if sha256(original.encode("utf-8")).hexdigest() != UPSTREAM_SHA256:
        raise RuntimeError("EPD Painter power source changed; review the H716 patch before building")
    if original.count(STARTUP_OLD) != 1 or original.count(OFF_OLD) != 1:
        raise RuntimeError("Expected H716 power sequences were not found exactly once")
    return original.replace(STARTUP_OLD, STARTUP_NEW).replace(OFF_OLD, OFF_NEW)


def patch_driver(path: Path) -> None:
    source = path.read_bytes().decode("utf-8")
    patched = patched_source(source)
    if patched != source:
        path.write_bytes(patched.encode("utf-8"))
        print("Applied H716 peripheral power / blue LED idle fix")


if "Import" in globals():
    Import("env")  # noqa: F821 - supplied by PlatformIO/SCons
    library = Path(env.subst("$PROJECT_LIBDEPS_DIR/$PIOENV")) / "EPD Painter/src"  # noqa: F821
    patch_driver(library / "epd_painter_powerctl.cpp")
    painter = library / "EPD_Painter.cpp"
    source = painter.read_bytes().decode("utf-8")
    patched = patched_memory_source(source)
    if patched != source:
        painter.write_bytes(patched.encode("utf-8"))
        print("Applied EPD PSRAM allocation for radio-off boot")
