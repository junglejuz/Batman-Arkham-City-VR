import sys
from PIL import Image, ImageStat
im = Image.open(sys.argv[1]).convert('HSV')
s = ImageStat.Stat(im).mean[1]
print('PAUSED' if s < 60 else 'PLAYING', round(s, 1))
