#!/usr/bin/env python3
"""Draw a restrained 320x240 menu frame in the existing Starwing UI palette."""
from __future__ import annotations
from collections import Counter
from pathlib import Path
from PIL import Image, ImageDraw

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'assets' / 'entry-panel.png'
HEADER = ROOT / 'source' / 'entry_panel_art_data.hpp'
TRANSPARENT=(0,0,0,0)
BLACK=(0,0,0,255)
NAVY=(18,30,46,255)
STEEL=(164,182,197,255)
WHITE=(255,255,255,255)
TEAL=(106,149,156,255)
MUTED=(98,133,131,255)
ICE=(156,255,255,255)
DARK=(90,89,98,255)

im=Image.new('RGBA',(320,240),TRANSPARENT)
d=ImageDraw.Draw(im)
# Use the same five flat, rectangular layers and square side tabs as the
# existing Options root panel. Only its size changes to fit two buttons.
def rect(x,y,w,h,colour):
    d.rectangle((x,y,x+w-1,y+h-1),fill=colour)
rect(48,68,224,106,WHITE)
rect(50,70,220,102,STEEL)
rect(52,72,216,98,TEAL)
rect(55,75,210,92,NAVY)
rect(57,78,206,86,BLACK)
for x in (63,68,73,244,249,254):
    rect(x,73,3,3,MUTED if x in (63,254) else ICE)
for x in (45,272):
    rect(x,109,4,26,DARK)
    rect(x+1,112,2,7,ICE)
    rect(x+1,124,2,7,ICE)
im.save(OUT)

pixels=list(im.get_flattened_data())
counts=Counter(pixels)
if len(counts)>16:raise SystemExit(f'palette too large: {len(counts)}')
colors=[TRANSPARENT]+sorted((c for c in counts if c!=TRANSPARENT),key=lambda c:(c[3],c[0],c[1],c[2]))
index={c:i for i,c in enumerate(colors)}
runs=[]
for px in pixels:
    i=index[px]
    if runs and runs[-1][0]==i and runs[-1][1]<4095:runs[-1]=(i,runs[-1][1]+1)
    else:runs.append((i,1))
packed=[(i<<12)|length for i,length in runs]
rgba=lambda c:(c[0]<<24)|(c[1]<<16)|(c[2]<<8)|c[3]
with HEADER.open('w') as f:
    f.write('#pragma once\n#include <array>\n#include <cstdint>\nnamespace starfox::platform_3ds {\n')
    f.write(f'inline constexpr std::array<std::uint32_t,{len(colors)}> entry_panel_colors{{{{\n')
    f.write(','.join(f'0x{rgba(c):08x}U' for c in colors)+'\n}};\n')
    f.write(f'inline constexpr std::array<std::uint16_t,{len(packed)}> entry_panel_runs{{{{\n')
    for start in range(0,len(packed),12):
        f.write(','.join(f'0x{v:04x}U' for v in packed[start:start+12])+',\n')
    f.write('}};\n} // namespace starfox::platform_3ds\n')
# Preview with the existing buttons, to check spacing against the real art.
base=Image.new('RGB',(320,240),(49,64,82))
stars=[(17,19),(49,30),(83,13),(112,45),(157,23),(207,31),(264,17),
       (300,39),(24,65),(74,80),(132,68),(180,91),(235,77),(286,99),
       (41,116),(102,126),(157,111),(221,129),(279,143),(13,159),
       (64,175),(124,162),(192,183),(255,169),(305,191),(38,214),
       (91,226),(207,217),(279,228)]
for x,y in stars:base.putpixel((x,y),(131,149,131))
base.paste(im,mask=im.getchannel('A'))
for name,y in [('options-button.png',85),('main-menu-button.png',129)]:
    button=Image.open(ROOT.parent.parent.parent/'assets'/'bottom-screen'/name).convert('RGBA')
    base.paste(button,(70,y),button)
preview=ROOT.parent.parent.parent/'Build'/'hardware-0.33'/'entry-panel-preview.png'
preview.parent.mkdir(parents=True,exist_ok=True)
base.save(preview)
print(f'{OUT}: {len(colors)} colors, {len(runs)} RLE runs; preview {preview}')
