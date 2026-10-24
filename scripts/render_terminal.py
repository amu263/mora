"""Rasterize MORA's actual ANSI frame for review, without inventing UI content."""
import re
import sys
import unicodedata
from pathlib import Path
from PIL import Image, ImageDraw, ImageFont

source, dest = map(Path, sys.argv[1:3])
data = source.read_text(encoding='utf-8')
cell_w, cell_h = 12, 24
image = Image.new('RGB', (240*cell_w, 100*cell_h), '#151920')
draw = ImageDraw.Draw(image)
max_x = max_y = 0
latin = ImageFont.truetype('C:/Windows/Fonts/CascadiaMono.ttf', 18)
cjk = ImageFont.truetype('C:/Windows/Fonts/msyh.ttc', 18)
x = y = 0
fg, bg = (225,228,222), (21,25,32)
pattern = re.compile(r'\x1b\[([0-9;?]*)([A-Za-z])')
position = 0
while position < len(data):
    match = pattern.match(data, position)
    if match:
        args, command = match.groups()
        vals = [int(v) for v in args.split(';') if v.isdigit()]
        if command == 'H':
            y, x = ((vals + [1,1])[:2]); y -= 1; x -= 1
        elif command == 'm' and len(vals) >= 5:
            if vals[0] == 38: fg = tuple(vals[2:5])
            elif vals[0] == 48: bg = tuple(vals[2:5])
        position = match.end(); continue
    char = data[position]; position += 1
    if char == '\n': x=0; y+=1; continue
    wide = 2 if unicodedata.east_asian_width(char) in ('W','F') else 1
    max_x = max(max_x, x + wide); max_y = max(max_y, y + 1)
    if char in '▀▄█':
        draw.rectangle((x*cell_w,y*cell_h,(x+wide)*cell_w-1,(y+1)*cell_h-1), fill=bg)
        if char == '▀': draw.rectangle((x*cell_w,y*cell_h,(x+wide)*cell_w-1,y*cell_h+cell_h//2-1),fill=fg)
        elif char == '▄': draw.rectangle((x*cell_w,y*cell_h+cell_h//2,(x+wide)*cell_w-1,(y+1)*cell_h-1),fill=fg)
        else: draw.rectangle((x*cell_w,y*cell_h,(x+wide)*cell_w-1,(y+1)*cell_h-1),fill=fg)
    else:
        draw.rectangle((x*cell_w,y*cell_h,(x+wide)*cell_w-1,(y+1)*cell_h-1),fill=bg)
        draw.text((x*cell_w,y*cell_h-1),char,font=cjk if wide==2 else latin,fill=fg)
    x += wide
image = image.crop((0, 0, max(1, max_x)*cell_w, max(1, max_y)*cell_h))
image.save(dest)
print(dest)
