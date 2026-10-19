#!/usr/bin/env python3
"""Rasterize the launch notice on the game's integer pixel grid.

Uses a locally supplied ROM/font map, never the system font. Generated artwork
is a private build input, just like the existing menu artwork headers.
"""
import argparse
from pathlib import Path
from PIL import Image, ImageDraw

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--rom', type=Path, required=True)
p.add_argument('--symbols', type=Path, required=True)
p.add_argument('--output', type=Path, required=True)
a = p.parse_args()
a.output.mkdir(parents=True, exist_ok=True)
rom = a.rom.read_bytes()
if len(rom) % 32768 == 512:
    rom = rom[512:]
symbols = {line.split()[0]: int(line.split()[1][1:], 16)
           for line in a.symbols.read_text().splitlines()
           if len(line.split()) == 2 and line.split()[1].startswith('$')}

def read(address):
    assert (address & 0xffff) >= 0x8000
    return rom[((address >> 16) & 0x7f)*0x8000 + (address & 0x7fff)]

def glyph(c):
    base = {'Á':'A', 'É':'E', 'Í':'I', 'Ó':'O', 'Ú':'U'}.get(c, c)
    if base == ' ': return 4, []
    if base == '/': return 8, [(int((10-y)*6/9)+dx,y) for y in range(1,11) for dx in (0,1)]
    index = read(symbols['FONT0TRN'] + ord(base)-32)
    width = min(read(symbols['FONT0WID']+index),16)
    addr = symbols['FONT0FON']+index*24
    pixels = [(x,y) for y in range(12) for x in range(width)
              if (read(addr+y*2) | (read(addr+y*2+1)<<8)) & (0x8000>>x)]
    if c != base:
        pixels += [(width//2+dx,-3+dy) for dx,dy in [(0,1),(1,0),(1,1),(2,0)]]
    return width, pixels

BLACK=(0,0,0); BG=(41,56,74); NAVY=(18,30,46)
WHITE=(255,255,255); STEEL=(164,182,197); BLUE=(106,149,156)
ICE=(156,255,255); DARK=(90,89,98); MUTED=(98,133,131)

def rect(im,x,y,w,h,c):
    ImageDraw.Draw(im).rectangle((x,y,x+w-1,y+h-1),fill=c)

def text(im,s,y,color=WHITE,scale=1):
    shapes=[glyph(c) for c in s]
    width=(sum(w+1 for w,_ in shapes)-1)*scale
    assert width <= im.width-46, (s,width)
    x=(im.width-width)//2
    for w,pixels in shapes:
        for px,py in pixels: rect(im,x+px*scale,y+py*scale,scale,scale,color)
        x+=(w+1)*scale

def panel(im):
    # Exact Options plate layers, palette, tabs and 1-pixel hard edges.
    extra=im.width-320
    for x,y,w,h,c in [(14,23,292,193,WHITE),(16,26,288,187,STEEL),
                      (18,29,284,179,BLUE),(21,33,278,169,NAVY),
                      (23,36,274,162,BLACK)]:
        rect(im,x,y,w+extra,h,c)
    for x in [32,40,48,272+extra,280+extra,288+extra]:
        rect(im,x,31,3,3,MUTED if x in [32,288+extra] else ICE)
    for x in [11,302+extra]:
        rect(im,x,91,8,30,DARK)
        for y in [95,108]:rect(im,x+2,y,4,8,ICE)
    rect(im,(im.width-110)//2,202,110,12,WHITE)
    rect(im,(im.width-102)//2,205,102,7,NAVY)

top=Image.new('RGB',(400,240),BG);bottom=Image.new('RGB',(320,240),BG)
panel(top);panel(bottom)
text(top,'STAR FOX',49,ICE,2)
text(top,'VERSIÓN 1.1',82,STEEL)
text(top,'ESTE JUEGO TODAVÍA',112)
text(top,'NO ESTÁ DISPONIBLE',132)
text(top,'PARA LA OLD 3DS.',152)
text(top,'OLD 3DS / 2DS',179,STEEL)
text(bottom,'FUTURA',58,ICE)
text(bottom,'COMPATIBILIDAD',82,ICE)
text(bottom,'PLANEADA',106,ICE)
rect(bottom,47,134,226,2,BLUE)
x,y,w,h=70,157,180,27
rect(bottom,x,y,w,h,STEEL);rect(bottom,x+1,y,w-2,2,WHITE)
rect(bottom,x+1,y+2,2,h-4,WHITE);rect(bottom,x+3,y+3,w-6,h-6,BLUE)
rect(bottom,x+2,y+h-3,w-4,2,DARK)
text(bottom,'START  SALIR',y+(h-12)//2,ICE)
# PNGs are previews of the very same pixels encoded into the executable.
top.save(a.output/'old-3ds-top.png');bottom.save(a.output/'old-3ds-bottom.png')
preview=Image.new('RGB',(400,480),BLACK);preview.paste(top,(0,0));preview.paste(bottom,(40,240))
preview.resize((800,960),Image.Resampling.NEAREST).save(a.output/'Old-3DS-pixel-preview.png')
colors=sorted(set(top.getdata())|set(bottom.getdata()))
assert len(colors)<=16
header=['#pragma once','#include <array>','#include <cstdint>',
        'namespace starfox::platform_3ds {',
        'inline constexpr std::array<std::uint32_t,%d> old_notice_colors{{'%len(colors),
        ','.join('0x%02x%02x%02xffU'%c for c in colors),'}};']
for name,im in [('top',top),('bottom',bottom)]:
    runs=[]
    for pixel in im.getdata():
        index=colors.index(pixel)
        if runs and runs[-1][0]==index and runs[-1][1]<4095:runs[-1][1]+=1
        else:runs.append([index,1])
    assert sum(n for _,n in runs)==im.width*im.height
    header+=['inline constexpr std::array<std::uint16_t,%d> old_notice_%s{{'%(len(runs),name)]
    for start in range(0,len(runs),12):
        header += [','.join('0x%04xU'%((i<<12)|n) for i,n in runs[start:start+12])+',']
    header+=['}};']
header+=['} // namespace starfox::platform_3ds','']
(a.output/'old_3ds_notice_art_data.hpp').write_text('\n'.join(header))
print('Generated native 400x240 + 320x240 artwork; %d colors; no antialiasing.'%len(colors))
