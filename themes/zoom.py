# python3 zoom.py x0 y0 x1 y1 scale out.png  -> enlarged crop with a 10 px grid, to read coordinates
import sys
from PIL import Image, ImageDraw
x0, y0, x1, y1, s = map(int, sys.argv[1:6])
im = Image.open('rsba1.png').crop((x0, y0, x1, y1)).resize(((x1 - x0) * s, (y1 - y0) * s), Image.NEAREST)
d = ImageDraw.Draw(im)
for x in range(x0 - x0 % 10, x1, 10):
    if x >= x0: d.line([((x - x0) * s, 0), ((x - x0) * s, 6)], fill=(255, 0, 255))
for y in range(y0 - y0 % 10, y1, 10):
    if y >= y0: d.line([(0, (y - y0) * s), (6, (y - y0) * s)], fill=(255, 0, 255))
im.save(sys.argv[6])
