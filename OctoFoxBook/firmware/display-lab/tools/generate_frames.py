"""Generate shared four-shade text/selection targets, not private book content.

Pillow is needed only to regenerate; firmware builds use the committed header.
Pass a TrueType font that includes Cyrillic. Font files are not redistributed.
"""
import argparse
import hashlib
import itertools
import json
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont


def pack_runs(levels):
    result = []
    for level, group in itertools.groupby(levels):
        count = sum(1 for _ in group)
        while count:
            n = min(count, 65535)
            result.append((n, level))
            count -= n
    return result


def native_levels(image):
    # Same portrait mapping as the reader: physical x=959-y, physical y=x.
    pixels = image.load()
    return bytes((255 - pixels[y, 959 - x] + 42) // 85
                 for y in range(540) for x in range(960))


def cover_image(variant, font):
    """Original geometric test covers: broad gray fields and fine details."""
    image = Image.new('L', (432, 660), 0 if variant == 0 else 255)
    draw = ImageDraw.Draw(image)
    if variant == 0:
        draw.ellipse((190, 70, 370, 250), fill=170)
        draw.polygon([(0, 450), (170, 230), (310, 470), (432, 340),
                      (432, 660), (0, 660)], fill=85)
        draw.polygon([(0, 560), (270, 390), (432, 550), (432, 660),
                      (0, 660)], fill=170)
        draw.text((28, 30), 'NIGHT / 01', font=font, fill=255)
    else:
        for x in range(0, 432, 24):
            draw.line((x, 100, 432 - x, 550), fill=170, width=2)
        draw.ellipse((60, 200, 350, 490), fill=85)
        draw.ellipse((130, 260, 290, 420), fill=255)
        draw.text((28, 30), 'LIGHT / 02', font=font, fill=0)
        # Alternating black/white marks expose residual high-frequency detail.
        for y in range(560, 620, 4):
            for x in range(24, 408, 4):
                if (x + y) % 8 == 0:
                    draw.rectangle((x, y, x + 1, y + 1), fill=0)
    return image


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--font', type=Path, required=True)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    output = root / 'fixtures'
    output.mkdir(exist_ok=True)
    header = ['#pragma once', '#include <stdint.h>',
              'struct LabRun { uint16_t count; uint8_t level; };']
    manifest = {'fixture_version': 2, 'width': 960, 'height': 540, 'levels': 4,
                'font_sha256': hashlib.sha256(args.font.read_bytes()).hexdigest(),
                'frames': []}
    text = [
        'The quiet shore was bright with morning light.',
        'A small boat waited beside the wooden bridge.',
        'We opened the book and turned another page.',
        'No wind disturbed the water near the stones.',
        'Чистая страница: тонкие линии и чёрный текст.',
        'На белом поле не должно быть старых букв.',
        'Сначала прочитай строку, затем переверни лист.',
        'Мягкие тени оставались только под деревьями.',
    ]
    font = ImageFont.truetype(str(args.font), 22)
    title = ImageFont.truetype(str(args.font), 28)
    covers = [cover_image(i, title) for i in range(2)]
    for index in range(10):
        img = Image.new('L', (540, 960), 255)
        draw = ImageDraw.Draw(img)
        draw.text((24, 20), 'OctoFox / display comparison', font=title, fill=0)
        draw.line((24, 66, 516, 66), fill=170, width=1)
        if index < 4:
            for row in range(20):
                # Shift baselines and line lengths to expose residual strokes.
                line = text[(row + index * 3) % len(text)]
                draw.text((24 + index * 3, 104 + row * 36 + index * 5),
                          line, font=font, fill=0)
        elif index < 6:
            for row in range(6):
                box = (24, 100 + row * 116, 516, 198 + row * 116)
                selected = row == index - 3
                draw.rounded_rectangle(box, 12, fill=0 if selected else 255,
                                       outline=85, width=2)
                draw.text((42, box[1] + 29), f'BOOK {row + 1} / КНИГА {row + 1}',
                          font=title, fill=255 if selected else 0)
        elif index < 8:
            img.paste(covers[index - 6], (54, 120))
            draw.text((54, 802), 'Cover detail / synthetic image', font=font, fill=0)
        else:
            for row in range(4):
                top = 94 + row * 185
                selected = row == index - 8
                draw.rounded_rectangle((24, top, 516, top + 170), 10,
                                       fill=0 if selected else 255, outline=85, width=2)
                thumb = covers[(row + index) % 2].resize((92, 140), Image.Resampling.NEAREST)
                img.paste(thumb, (36, top + 14))
                ink = 255 if selected else 0
                draw.text((146, top + 22), f'BOOK {row + 1 + (index - 8) * 4}', font=title, fill=ink)
                draw.text((146, top + 68), 'Cover / author / title', font=font, fill=ink)
                draw.rectangle((146, top + 114, 460, top + 122), fill=170)
                draw.rectangle((146, top + 114, 210 + row * 50, top + 122), fill=85)
        for level in range(4):
            draw.rectangle((24 + level * 72, 870, 76 + level * 72, 894),
                           fill=255 - level * 85)
        draw.text((24, 916), f'TARGET {index + 1} / inspect blank margins', font=font, fill=0)
        levels = native_levels(img)
        runs = pack_runs(levels)
        assert len(levels) == 960 * 540 and max(levels) == 3
        assert bytes(v for n, v in runs for _ in range(n)) == levels
        # Preview exactly the quantized pixels used by both backends.
        preview = Image.new('L', (540, 960))
        out = preview.load()
        for y in range(540):
            for x in range(960):
                out[y, 959 - x] = 255 - levels[y * 960 + x] * 85
        preview.save(output / f'target-{index + 1}.png')
        header.append(f'static const LabRun kFrame{index}[] = {{')
        header.extend(','.join(f'{{{n},{v}}}' for n, v in runs[i:i + 16]) + ','
                      for i in range(0, len(runs), 16))
        header.append('};')
        manifest['frames'].append({'index': index, 'sha256': hashlib.sha256(levels).hexdigest(),
                                   'runs': len(runs)})
    header.append('static const LabRun* const kFrames[] = {' +
                  ','.join(f'kFrame{i}' for i in range(10)) + '};')
    header.append('static const size_t kFrameRuns[] = {' +
                  ','.join(f'sizeof(kFrame{i})/sizeof(LabRun)' for i in range(10)) + '};')
    (root / 'src' / 'frames.h').write_text('\n'.join(header) + '\n', encoding='utf8')
    (output / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n', encoding='utf8')


if __name__ == '__main__':
    main()
