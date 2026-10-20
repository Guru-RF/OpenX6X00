# python3 lum.py x0 y0 x1 y1 : luminance map, one hex digit per pixel (0 dark .. f bright)
import sys
from PIL import Image
x0, y0, x1, y1 = map(int, sys.argv[1:5])
im = Image.open('rsba1.png').convert('L')
print('     ' + ''.join(str((x // 10) % 10) if x % 10 == 0 else ' ' for x in range(x0, x1)))
for y in range(y0, y1):
    print(f'{y:4d} ' + ''.join('0123456789abcdef'[im.getpixel((x, y)) // 16] for x in range(x0, x1)))
