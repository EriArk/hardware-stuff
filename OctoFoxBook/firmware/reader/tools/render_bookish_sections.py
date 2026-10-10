"""Preview production section layouts with example data; requires Pillow and g++."""
from pathlib import Path
import argparse
import json
import re
import shutil
import subprocess
import tempfile
from PIL import Image, ImageDraw, ImageFont
ROOT=Path(__file__).resolve().parents[1]

def render(output):
    output.mkdir(parents=True,exist_ok=True)
    with tempfile.TemporaryDirectory() as folder:
        exe=Path(folder)/'sections.exe'
        subprocess.run([shutil.which('g++'),'-std=c++17','-I',str(ROOT/'include'),
            str(ROOT/'tools/host_bookish_sections.cpp'),str(ROOT/'src/bookish_ui.cpp'),'-o',str(exe)],check=True)
        calls=subprocess.check_output([str(exe)],encoding='utf-8')
    fonts=[ImageFont.truetype(str(ROOT/f'assets/fonts/{family.lower()}/{family}[wght].ttf'),size)
        for family,size in [('Arimo',10),('Arimo',18),('Arimo',22),('Lora',25),('Lora',31),('Lora',39)]]
    logo_source=(ROOT/'include/bookish_logo.h').read_text()
    width=int(re.search(r'kBookishLogoWidth\s*=\s*(\d+)',logo_source)[1])
    height=int(re.search(r'kBookishLogoHeight\s*=\s*(\d+)',logo_source)[1])
    values=re.findall(r'\b\d+\b',logo_source.split('{',1)[1].split('}',1)[0])
    logo=Image.frombytes('L',(width,height),bytes(int(v)*17 for v in values)).convert('RGB')
    scenes={}
    for line in calls.splitlines():
        if line.startswith('SCENE '):
            name=line[6:];canvas=Image.new('RGB',(540,960),'white');draw=ImageDraw.Draw(canvas);scenes[name]=canvas
            continue
        item=json.loads(line)
        if item[0]=='box':
            _,x,y,w,h,r,b,f,t=item
            draw.rounded_rectangle((x,y,x+w-1,y+h-1),radius=r,outline=(b*17,)*3,fill=(f*17,)*3,width=t)
        elif item[0]=='logo':canvas.paste(logo,(item[1],item[2]))
        elif item[0]=='cover':
            _,x,y,w,h=item
            draw.rectangle((x,y,x+w-1,y+h-1),fill='#555555')
            draw.rectangle((x+3,y+3,x+w-4,y+h-4),outline='#aaaaaa')
            draw.ellipse((x+w//3,y+h//3,x+2*w//3,y+h//3+w//3),fill='white')
        else:
            _,font,value,x,y,w,ink,paper=item
            if draw.textlength(value,font=fonts[font])>w:
                while value and draw.textlength(value+'…',font=fonts[font])>w:value=value[:-1]
                value+='…'
            draw.text((x,y),value,font=fonts[font],fill=(ink*17,)*3,anchor='ls')
    for name,canvas in scenes.items():canvas.save(output/f'{name}.png')
    overview=Image.new('RGB',(1620,960),'white')
    for i,name in enumerate(['books','search','favorites']):overview.paste(scenes[name],(540*i,0))
    overview.save(output/'overview.png')
    print(f'Rendered {len(scenes)} section layouts')
if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('output',type=Path);render(p.parse_args().output)
