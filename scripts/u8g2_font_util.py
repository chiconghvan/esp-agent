import os
import sys
import subprocess
import urllib.request
import math
from PIL import Image, ImageDraw, ImageFont

# --- Settings ---
BDFCONV_URL = "https://github.com/olikraus/u8g2/raw/master/tools/font/bdfconv/bdfconv.exe"
BDFCONV_EXE = os.path.join("scripts", "bdfconv.exe")
# Dải mã Tiếng Việt đầy đủ (ASCII, Latin-1, Ext-A, Ext-B, Ext-Additional)
VN_MAP = "32-126,160-255,256-383,384-591,7840-7929"

def download_bdfconv():
    if not os.path.exists(BDFCONV_EXE):
        print("[*] Downloading bdfconv.exe...")
        os.makedirs("scripts", exist_ok=True)
        urllib.request.urlretrieve(BDFCONV_URL, BDFCONV_EXE)

def generate_bdf_from_ttf(ttf_path, font_size, output_bdf):
    """
    Renders TTF glyphs to a BDF file with precise baseline alignment.
    """
    print(f"[*] Rendering TTF to BDF (Size: {font_size})...")
    font = ImageFont.truetype(ttf_path, font_size)
    
    chars_to_render = []
    for part in VN_MAP.split(','):
        if '-' in part:
            start, end = part.split('-')
            chars_to_render.extend(range(int(start), int(end) + 1))
        else:
            chars_to_render.append(int(part))

    # Lấy thông số chung của font
    ascent, descent = font.getmetrics()
    # font_height = ascent + descent

    with open(output_bdf, "w", encoding="ascii") as f:
        f.write("STARTFONT 2.1\n")
        f.write(f"FONT {os.path.basename(ttf_path)}\n")
        f.write(f"SIZE {font_size} 75 75\n")
        # FONTBOUNDINGBOX width height xoff yoff
        # yoff is the distance from baseline to the bottom of the bounding box (usually -descent)
        f.write(f"FONTBOUNDINGBOX {font_size} {ascent + descent} 0 {-descent}\n")
        f.write(f"CHARS {len(chars_to_render)}\n")

        for codepoint in chars_to_render:
            char = chr(codepoint)
            mask = font.getmask(char, mode='1')
            width, height = mask.size
            
            bbox = font.getbbox(char) # (left, top, right, bottom)
            if bbox and (bbox[2] > bbox[0]) and (bbox[3] > bbox[1]):
                offset_x = bbox[0]
                # y_offset in BDF = distance from baseline to bottom of glyph
                # In Pillow, bottom is distance from EM top. Baseline is at y=ascent.
                # So distance from baseline to bottom = ascent - bbox[3]
                y_offset = ascent - bbox[3]
            else:
                offset_x, y_offset = 0, 0
                width, height = 0, 0
            
            advance_width = int(font.getlength(char))
            
            f.write(f"STARTCHAR U{codepoint:04X}\n")
            f.write(f"ENCODING {codepoint}\n")
            f.write(f"SWIDTH 1000 0\n")
            f.write(f"DWIDTH {advance_width} 0\n")
            f.write(f"BBX {width} {height} {offset_x} {y_offset}\n")
            f.write("BITMAP\n")
            
            for row in range(height):
                row_bits = 0
                for col in range(width):
                    if mask.getpixel((col, row)):
                        row_bits |= (1 << (7 - (col % 8)))
                    if (col % 8 == 7) or (col == width - 1):
                        f.write(f"{row_bits:02X}")
                        row_bits = 0
                f.write("\n")
            f.write("ENDCHAR\n")
        f.write("ENDFONT\n")
    print(f"[+] Created BDF: {output_bdf}")

def generate_u8g2_preview(ttf_path, font_size, font_name, output_png):
    print("[*] Generating Preview PNG...")
    img_w, img_h = 450, 950
    image = Image.new('RGB', (img_w, img_h), color=(255, 255, 255))
    draw = ImageDraw.Draw(image)
    label_font = ImageFont.load_default()
    target_font = ImageFont.truetype(ttf_path, font_size)

    y = 10
    draw.text((10, y), f"u8g2_font_{font_name} (Fixed Baseline & VN Map)", fill=0, font=label_font)
    y += 40

    # Hiển thị dải mã bao gồm cả các ký tự Vietnamese mở rộng
    ranges = [32, 48, 64, 80, 96, 112, 160, 192, 224, 256, 7840, 7856, 7872, 7888, 7904, 7920]
    for start in ranges:
        draw.text((10, y), f"{start:4d}/0x{start:04x}", fill=(120, 120, 120), font=label_font)
        char_x = 100
        for i in range(16):
            try:
                draw.text((char_x, y), chr(start + i), fill=0, font=target_font)
            except: pass
            char_x += max(font_size, 18)
        y += max(font_size + 12, 24)

    draw.text((10, y+20), "The quick brown fox jumps over the lazy dog.", fill=0, font=target_font)
    draw.text((10, y+45), "Báo cáo: Ầ ầ Ậ ậ Ắ ắ (Kiểm tra chân chữ & dấu)", fill=0, font=target_font)
    image.save(output_png)

def generate_oled_mockup(ttf_path, font_size, font_name, output_png):
    """Generates a 128x64 simulated OLED screen preview."""
    print("[*] Simulating SSD1306 OLED screen (128x64)...")
    canvas = Image.new('1', (128, 64), color=0)
    draw = ImageDraw.Draw(canvas)
    
    try:
        ui_font_small = ImageFont.load_default()
        target_font = ImageFont.truetype(ttf_path, font_size)
    except: return

    draw.line([(0, 11), (127, 11)], fill=1)
    draw.text((2, 0), "Deadline", font=ui_font_small, fill=1)
    draw.text((85, 0), "10:45", font=ui_font_small, fill=1)
    
    draw.text((2, 14), "#66", font=ui_font_small, fill=1)
    draw.text((32, 14), "Báo cáo: Ầ ầ Ắ ắ", font=target_font, fill=1)
    draw.text((2, 35), "Lúc: 15h30", font=target_font, fill=1)
    draw.text((2, 52), "Cần hoàn thành gấp!", font=ui_font_small, fill=1)

    scale = 6
    upscaled = canvas.resize((128 * scale, 64 * scale), Image.NEAREST).convert('RGB')
    oled_color = (180, 230, 255)
    pixels = upscaled.load()
    for py in range(upscaled.height):
        for px in range(upscaled.width):
            if px % scale == 0 or py % scale == 0:
                pixels[px, py] = (10, 10, 20)
            elif pixels[px, py] == (255, 255, 255):
                pixels[px, py] = oled_color
    upscaled.save(output_png)
    print(f"[+] OLED Mockup saved: {output_png}")

if __name__ == "__main__":
    if len(sys.argv) < 3:
        print("Usage: python scripts/u8g2_font_util.py <file.ttf> <size>")
        sys.exit(1)

    input_ttf = sys.argv[1]
    f_size = int(sys.argv[2])
    clean_name = os.path.splitext(os.path.basename(input_ttf))[0].lower().replace('-', '_')
    base_name = f"{clean_name}_{f_size}"
    
    download_bdfconv()
    bdf_file = f"{base_name}.bdf"
    generate_bdf_from_ttf(input_ttf, f_size, bdf_file)
    
    # 2. Call bdfconv.exe
    c_file = f"u8g2_font_{base_name}.c"
    print(f"[*] Compressing BDF for U8g2 using bdfconv.exe...")
    cmd = [
        BDFCONV_EXE, "-v", bdf_file, "-b", "0", "-f", "1", 
        "-m", VN_MAP, "-n", f"u8g2_font_{base_name}", "-o", c_file
    ]
    result = subprocess.run(cmd, capture_output=True, text=True)
    
    if result.returncode == 0:
        print(f"[+] Successfully generated: {c_file}")
    else:
        print(f"[!] BDFCONV Error: {result.stderr}")
        sys.exit(1)
    
    # 3. Create Previews
    generate_u8g2_preview(input_ttf, f_size, base_name, f"{base_name}_preview.png")
    generate_oled_mockup(input_ttf, f_size, base_name, f"{base_name}_oled_live.png")
    
    print(f"\n[SUCCESS] Font fixed and converted!")
    print(f"- Output C: {c_file}")
    print(f"- Live Mockup: {base_name}_oled_live.png")
    print(f"\nTo apply this font to your code, run:")
    print(f"python scripts/switch_font.py {c_file}")
