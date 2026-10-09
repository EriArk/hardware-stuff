"""Render two code-defined Home concepts into identical four-shade panel frames.

Requires Pillow and local Georgia/Arial fonts; font binaries are not distributed.
All book names, progress and artwork here are fictional demonstration content.
"""
import argparse
import hashlib
import itertools
import json
import math
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont, ImageOps

S = 3
ROOT = Path(__file__).resolve().parents[1]


class Page:
    def __init__(self, fonts, fantasy):
        self.image = Image.new('L', (540 * S, 960 * S), 255)
        self.d = ImageDraw.Draw(self.image)
        self.fonts = fonts
        self.fantasy = fantasy

    def box(self, xy, fill=None, outline=None, width=1, radius=0):
        coords = tuple(round(x * S) for x in xy)
        if radius:
            self.d.rounded_rectangle(coords, radius*S, fill, outline, width*S)
        else:
            self.d.rectangle(coords, fill, outline, width*S)

    def line(self, points, fill=0, width=1):
        self.d.line([(round(x*S), round(y*S)) for x,y in points], fill, width*S, joint='curve')

    def text(self, xy, text, size=20, face='sans', ink=0, anchor=None):
        font = ImageFont.truetype(str(self.fonts[face]), size*S)
        # Check all text bounds before export so off-screen glyphs cannot hide.
        pos = tuple(round(v*S) for v in xy)
        bounds = self.d.textbbox(pos, text, font=font, anchor=anchor)
        assert bounds[0] >= 0 and bounds[2] <= 540*S, (text, bounds)
        assert bounds[1] >= 0 and bounds[3] <= 960*S, (text, bounds)
        self.d.text(pos, text, font=font, fill=ink, anchor=anchor)

    def diamond(self, x, y, size=4, fill=0):
        self.d.polygon([(x*S,(y-size)*S),((x+size)*S,y*S),
                        (x*S,(y+size)*S),((x-size)*S,y*S)], fill=fill)

    def ornament(self, y, left=30, right=510):
        self.line([(left,y),(245,y)], 85)
        self.line([(295,y),(right,y)], 85)
        for x, size in [(253,2),(270,6),(287,2)]:
            self.diamond(x,y,size)

    def tentacle(self, mirror=False):
        # A tapered scroll restricted to the outer margin, away from labels.
        points=[]
        for i in range(121):
            t=i/120
            x=(1-t)**3*(-8)+3*(1-t)**2*t*52+3*(1-t)*t*t*(-22)+t**3*14
            y=(1-t)**3*960+3*(1-t)**2*t*850+3*(1-t)*t*t*715+t**3*643
            if mirror: x=540-x
            points.append((x,y))
        for i in range(1,61):
            t=i/60
            angle=math.pi/2-t*math.pi*2.2
            radius=14*(1-t)
            x=14+radius*math.cos(angle);y=629+radius*math.sin(angle)
            if mirror: x=540-x
            points.append((x,y))
        for i in range(len(points)-1):
            self.line(points[i:i+2],0,max(1,round(7*(1-i/180))))
        for i in range(8,122,14):
            x,y=points[i]
            self.d.ellipse(((x-1.2)*S,(y-2)*S,(x+1.2)*S,(y+2)*S),fill=255)


def book_cover(fonts, kind=0):
    im=Image.new('L',(144*S,222*S),0 if kind==0 else 170)
    d=ImageDraw.Draw(im)
    d.rectangle((7*S,7*S,137*S,215*S),outline=170,width=S)
    d.ellipse((45*S,58*S,100*S,113*S),fill=255)
    for j in range(6):
        points=[(x*S,(133+j*10+8*math.sin(x/28+j*.8))*S) for x in range(10,135)]
        d.line(points,fill=[85,170,255][j%3],width=2*S)
    f=ImageFont.truetype(str(fonts['serif']),16*S)
    for y,word in [(22,'МОРЕ'),(41,'ТИШИНЫ')]:
        d.text((72*S,y*S),word,font=f,fill=255,anchor='mt')
    return im


def render(fonts, logo, fantasy):
    p=Page(fonts,fantasy)
    face='serif' if fantasy else 'sans'
    emblem=Image.open(logo).convert('L')
    mask=emblem.point(lambda v: 255 if v<235 else 0)
    bounds=mask.getbbox()
    emblem=emblem.crop(bounds)
    emblem=ImageOps.contain(emblem,(42*S,48*S),Image.Resampling.LANCZOS)
    p.image.paste(emblem,(30*S,22*S))
    p.text((86,26),'AbyssBook',27,'serif')
    p.text((87,61),'ВАША ЛИЧНАЯ БИБЛИОТЕКА',10,'sans',85)
    p.text((508,35),'НА УСТРОЙСТВЕ',11,'sans',85,anchor='rt')
    if fantasy:
        p.line([(30,87),(510,87)],0)
        p.diamond(270,87,4)
    else:
        p.line([(30,87),(510,87)],170)

    for x,label in [(30,'Главная'),(145,'Библиотека'),(283,'Каталог'),(390,'Избранное')]:
        p.text((x,107),label,18,face,0 if x==30 else 85)
    p.line([(30,139),(105,139)],0,3)
    p.line([(30,147),(510,147)],170)

    p.text((30,173),'Продолжим',39,'serif')
    p.text((30,220),'историю?',39,'italic' if fantasy else 'serif')
    p.text((32,277),'Тихий вечер. Хорошая книга.',19,'italic' if fantasy else 'sans',85)
    if fantasy:
        # Re-read the vector-like source at useful resolution for the larger seal.
        seal=Image.open(logo).convert('L').crop(bounds)
        seal=ImageOps.contain(seal,(86*S,96*S),Image.Resampling.LANCZOS)
        p.image.paste(seal,(408*S,181*S))
        p.box((27,318,513,584),outline=0)
        p.box((32,323,508,579),outline=170)
        for x in [27,513]:
            for y in [318,584]: p.diamond(x,y,6)
    else:
        p.box((28,318,512,584),outline=170,radius=9)
    cover=book_cover(fonts)
    p.image.paste(cover,(42*S,339*S))
    p.text((211,339),'Море',31,'serif')
    p.text((211,377),'тишины',31,'serif')
    p.text((213,426),'Елена Север',19,'sans',85)
    p.text((213,466),'Глава 8  ·  34%',18,'sans',85)
    p.line([(214,503),(491,503)],170,3)
    p.line([(214,503),(308,503)],0,3)
    p.box((210,526,494,569),fill=0,radius=0 if fantasy else 5)
    p.text((235,533),'Продолжить',22,face,255)
    p.line([(465,541),(475,548),(465,555)],255,2)

    if fantasy: p.ornament(611)
    p.text((31,627),'Недавно открывали',25,'serif')
    for y,title,author,progress in [(677,'Сад за горизонтом','Александр Лис','12%'),
                                    (762,'Письма с маяка','Мария Ветрова','67%')]:
        thumb=cover.resize((40*S,62*S),Image.Resampling.LANCZOS)
        p.image.paste(thumb,(34*S,y*S))
        p.text((92,y+1),title,23,'serif')
        p.text((93,y+34),author,17,'sans',85)
        p.text((506,y+37),progress,16,'sans',85,anchor='rt')
        p.line([(30,y+76),(510,y+76)],170)
    if fantasy:
        p.tentacle();p.tentacle(True)
        p.ornament(865)
    else: p.line([(30,865),(510,865)],170)
    p.text((270,884),'OK / UP / DOWN — сменить вариант',17,'sans',0,anchor='mt')
    p.text((270,923),'02 / ФЭНТЕЗИЙНЫЙ' if fantasy else '01 / КНИЖНЫЙ',13,'sans',85,anchor='mt')
    small=p.image.resize((540,960),Image.Resampling.LANCZOS)
    return small.point(lambda v: min(3,(v+42)//85)*85)


def main():
    parser=argparse.ArgumentParser()
    parser.add_argument('--font-dir',type=Path,required=True)
    parser.add_argument('--logo',type=Path,default=ROOT.parents[1]/'source/reference/logo.png')
    args=parser.parse_args()
    fonts={k:args.font_dir/v for k,v in {'sans':'arial.ttf','serif':'georgia.ttf','italic':'georgiai.ttf'}.items()}
    out=ROOT/'fixtures';out.mkdir(parents=True,exist_ok=True)
    header=['#pragma once','#include <stdint.h>','struct UiRun { uint16_t count; uint8_t level; };']
    manifest={'width':960,'height':540,'portrait':[540,960],'shades':4,'frames':[],
              'font_sha256':{k:hashlib.sha256(v.read_bytes()).hexdigest() for k,v in fonts.items()},
              'logo_sha256':hashlib.sha256(args.logo.read_bytes()).hexdigest()}
    for index in range(2):
        im=render(fonts,args.logo,bool(index));im.save(out/f'home-{index+1}.png')
        pixels=im.load()
        raw=bytes((255-pixels[y,959-x])//85 for y in range(540) for x in range(960))
        runs=[]
        for value,group in itertools.groupby(raw):
            n=sum(1 for _ in group)
            while n:
                take=min(n,65535);runs.append((take,value));n-=take
        assert b''.join(bytes([v])*n for n,v in runs)==raw
        header.append(f'static const UiRun kUi{index}[] = {{')
        header.extend(','.join(f'{{{n},{v}}}' for n,v in runs[i:i+16])+',' for i in range(0,len(runs),16))
        header.append('};')
        manifest['frames'].append({'index':index,'sha256':hashlib.sha256(raw).hexdigest(),'runs':len(runs)})
    header+=['static const UiRun* const kUiFrames[] = {kUi0,kUi1};',
             'static const size_t kUiCounts[] = {sizeof(kUi0)/sizeof(UiRun),sizeof(kUi1)/sizeof(UiRun)};']
    (ROOT/'src/ui_frames.h').write_text('\n'.join(header)+'\n',encoding='utf8')
    (out/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n',encoding='utf8')


if __name__=='__main__': main()
