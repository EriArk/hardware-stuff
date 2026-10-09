"""Regenerate the small four-shade header logo from the original project art."""
from pathlib import Path
from PIL import Image, ImageOps

ROOT = Path(__file__).resolve().parents[1]
source = ROOT.parents[1] / 'source/reference/logo.png'
image = Image.open(source).convert('L')
image = image.crop(image.point(lambda value: 255 if value < 235 else 0).getbbox())
image = ImageOps.contain(image, (42, 48), Image.Resampling.LANCZOS)
pixels = [round(value / 85) * 5 for value in image.getdata()]
(ROOT / 'include/bookish_logo.h').write_text(
    '// Derived from source/reference/logo.png (project owner artwork).\n#pragma once\n'
    f'constexpr int kBookishLogoWidth={image.width};\n'
    f'constexpr int kBookishLogoHeight={image.height};\n'
    'const uint8_t kBookishLogo[] = {' + ','.join(map(str, pixels)) + '};\n',
    encoding='utf-8')
