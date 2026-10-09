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


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--font', type=Path, required=True)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    output = root / 'fixtures'
    output.mkdir(exist_ok=True)
    header = ['#pragma once', '#include <stdint.h>',
              'struct LabRun { uint16_t count; uint8_t level; };']
    manifest = {'width': 960, 'height': 540, 'levels': 4,
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
    for index in range(6):
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
        else:
            for row in range(6):
                box = (24, 100 + row * 116, 516, 198 + row * 116)
                selected = row == index - 3
                draw.rounded_rectangle(box, 12, fill=0 if selected else 255,
                                       outline=85, width=2)
                draw.text((42, box[1] + 29), f'BOOK {row + 1} / КНИГА {row + 1}',
                          font=title, fill=255 if selected else 0)
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
                  ','.join(f'kFrame{i}' for i in range(6)) + '};')
    header.append('static const size_t kFrameRuns[] = {' +
                  ','.join(f'sizeof(kFrame{i})/sizeof(LabRun)' for i in range(6)) + '};')
    (root / 'src' / 'frames.h').write_text('\n'.join(header) + '\n', encoding='utf8')
    (output / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n', encoding='utf8')


if __name__ == '__main__':
    main()
