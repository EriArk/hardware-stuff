"""Render production Canvas calls with the shipped fonts (requires Pillow and g++).

This is a layout preview, not evidence of physical E-Ink quality.
Usage: python tools/render_wifi_settings.py OUTPUT_DIRECTORY
"""
from pathlib import Path
import argparse
import json
import shutil
import subprocess
import tempfile
from PIL import Image, ImageDraw, ImageFont

ROOT = Path(__file__).resolve().parents[1]

def render(output: Path):
    output.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory() as temporary:
        binary = Path(temporary)/'wifi-ui.exe'
        subprocess.run([shutil.which('g++'), '-std=c++17',
            '-I', str(ROOT/'tools/wifi_host_stubs'), '-I', str(ROOT/'include'),
            str(ROOT/'tools/host_wifi_ui.cpp'), str(ROOT/'src/wifi_setup.cpp'),
            str(ROOT/'src/wifi_credentials.cpp'), '-o', str(binary)], check=True)
        calls = subprocess.check_output([str(binary)], encoding='utf-8')
    fonts = [ImageFont.truetype(str(ROOT/f'assets/fonts/{family.lower()}/{family}[wght].ttf'), size)
             for family, size in [('Arimo',10),('Arimo',18),('Arimo',22),('Lora',25),('Lora',31),('Lora',39)]]
    scenes = {}
    for line in calls.splitlines():
        if line.startswith('SCENE '):
            name = line.split(' ',1)[1]
            canvas = Image.new('RGB',(540,960),'white')
            draw = ImageDraw.Draw(canvas)
            scenes[name] = canvas
        elif line.startswith('['):
            item = json.loads(line)
            if item[0] == 'box':
                _,x,y,w,h,r,border,fill,t = item
                draw.rounded_rectangle((x,y,x+w-1,y+h-1),radius=r,fill=(fill*17,)*3,
                                       outline=(border*17,)*3,width=t)
            else:
                _,font,text,x,y,w,ink,_ = item
                # Firmware fits labels to the supplied width; emulate that bound.
                if draw.textlength(text,font=fonts[font]) > w:
                    while text and draw.textlength(text+'…',font=fonts[font]) > w:
                        text = text[:-1]
                    text += '…'
                draw.text((x,y),text,font=fonts[font],fill=(ink*17,)*3,anchor='ls')
    for name,canvas in scenes.items():
        canvas.save(output/f'{name}.png')
    overview = Image.new('RGB',(1620,960),'white')
    for i,name in enumerate(['settings','networks','typing']):
        overview.paste(scenes[name],(i*540,0))
    overview.save(output/'overview.png')
    print(f'Rendered {len(scenes)} production UI states')

if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('output',type=Path)
    render(parser.parse_args().output)
