from PIL import ImageFont
import os

ttf_path = r"e:\Code\esp2\scripts\CascadiaCode-Light.ttf"
font_size = 14
font = ImageFont.truetype(ttf_path, font_size)

for char in ['A', 'g']:
    bbox = font.getbbox(char)
    print(f"Char: {char}, BBox: {bbox}")

ascent, descent = font.getmetrics()
print(f"Metrics: ascent={ascent}, descent={descent}")
