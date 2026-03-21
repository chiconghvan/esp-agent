import os
import sys
import subprocess
import urllib.request
import math
import re
import platform
from datetime import datetime
from PIL import Image, ImageDraw, ImageFont

# --- Settings ---
# BDFCONV is the tool translated BDF font to U8g2-compatible .c file
# For Windows, we use the .exe from the official repo.
# For macOS, you should compile it from u8g2/tools/font/bdfconv source.
IS_WINDOWS = platform.system() == "Windows"
BDFCONV_EXE = os.path.join("scripts", "bdfconv.exe" if IS_WINDOWS else "bdfconv")
BDFCONV_URL = "https://github.com/olikraus/u8g2/raw/master/tools/font/bdfconv/bdfconv.exe"
# Dải mã Tiếng Việt tối ưu (ASCII, các nguyên âm có dấu Latin-1, Ext-A/B, và Ext-Additional)
# Rút gọn từ ~617 xuống ~251 ký tự để tiết kiệm bộ nhớ flash
VN_MAP = "32-126,160,192-218,224-250,258-259,272-273,296-297,360-361,416-417,431-432,7840-7929"

def get_codepoints_from_map(vn_map):
    """Parses a map string like '32-126,160' into a sorted list of unique integers."""
    points = []
    for part in vn_map.split(','):
        part = part.strip()
        if '-' in part:
            try:
                s, e = part.split('-')
                points.extend(range(int(s), int(e) + 1))
            except: pass
        else:
            try: points.append(int(part))
            except: pass
    return sorted(list(set(points)))

def download_bdfconv():
    """Downloads bdfconv.exe if missing (only for Windows)."""
    if not os.path.exists(BDFCONV_EXE):
        if IS_WINDOWS:
            print("[*] Downloading bdfconv.exe (Windows)...")
            os.makedirs("scripts", exist_ok=True)
            urllib.request.urlretrieve(BDFCONV_URL, BDFCONV_EXE)
        else:
            print(f"[!] Error: '{BDFCONV_EXE}' not found.")
            print("Please ensure 'scripts/bdfconv' is compiled for macOS.")
            print("Run: gcc -o scripts/bdfconv scripts/bdfconv_src/*.c")
            sys.exit(1)

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
            bbox = font.getbbox(char)
            
            if not bbox or (bbox[2] <= bbox[0]) or (bbox[3] <= bbox[1]):
                offset_x, y_offset = 0, 0
                width, height = 0, 0
                bitmap_rows = []
            else:
                # 1. Extract raw bitmap
                raw_rows = []
                for r in range(height):
                    row_data = [mask.getpixel((c, r)) for c in range(width)]
                    raw_rows.append(row_data)
                
                # 2. Trim empty rows from top and bottom to get "true" visual bounds
                top_trim = 0
                while top_trim < height and not any(raw_rows[top_trim]):
                    top_trim += 1
                
                bottom_trim = 0
                while bottom_trim < (height - top_trim) and not any(raw_rows[height - 1 - bottom_trim]):
                    bottom_trim += 1
                
                # Current metrics before trimming
                offset_x = bbox[0]
                y_offset = ascent - bbox[3]
                
                # Update metrics based on trim
                # Removing bottom rows increases y_offset (lifts the bottom)
                y_offset += bottom_trim
                height = height - top_trim - bottom_trim
                bitmap_rows = raw_rows[top_trim : (top_trim + height)] if height > 0 else []
                
                # --- Selective Baseline Alignment ---
                # List of characters that definitely should NOT have parts below the line (belly at 0)
                belly_on_line = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefhiklmnorstuvwxz0123456789"
                
                if height > 0 and y_offset != 0:
                    # Snap floating (1, 2) or tiny overshoot (-1)
                    if y_offset in [1, 2, -1]:
                        y_offset = 0
                    # For standard line-chars, we can even snap -2 if it's overshoot
                    elif char in belly_on_line and y_offset == -2:
                        y_offset = 0
            
            advance_width = int(font.getlength(char))
            
            f.write(f"STARTCHAR U{codepoint:04X}\n")
            f.write(f"ENCODING {codepoint}\n")
            f.write(f"SWIDTH 1000 0\n")
            f.write(f"DWIDTH {advance_width} 0\n")
            f.write(f"BBX {width} {height} {offset_x} {y_offset}\n")
            f.write("BITMAP\n")
            
            for row_data in bitmap_rows:
                row_bits = 0
                for col in range(width):
                    if row_data[col]:
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

    draw.text((10, y+20), "Tiếng Việt rất giàu và đẹp, đủ để biểu đạt mọi ý tưởng phức tạp nhất.", fill=0, font=target_font)
    #draw.text((10, y+45), "Báo cáo: Ầ ầ Ậ ậ Ắ ắ (Kiểm tra chân chữ & dấu)", fill=0, font=target_font)
    image.save(output_png)
def draw_wrapped_text(draw, text, font, x, y, max_width, line_pitch, fill=1):
    """Helper to draw text with simple word wrapping."""
    words = text.split(' ')
    lines = []
    current_line = ""
    for word in words:
        test_line = current_line + (" " if current_line else "") + word
        if draw.textlength(test_line, font=font) > max_width - x:
            if current_line: lines.append(current_line)
            current_line = word
        else:
            current_line = test_line
    if current_line: lines.append(current_line)
    
    for line in lines:
        draw.text((x, y), line, font=font, fill=fill)
        y += line_pitch
    return y


def draw_dense_character_grid(draw, codepoints, font, x, y, max_width, line_pitch, fill=1):
    """Helper to draw all characters in a continuous wrapping grid without decimal/hex prefixes."""
    curr_x = x
    # Estimate char width based on 'W' or size
    char_w_unit = draw.textlength("W", font=font)
    spacing = max(char_w_unit + 2, 12)
    
    # Calculate how many chars per row
    chars_per_row = int((max_width - x - 4) // spacing)
    if chars_per_row < 1: chars_per_row = 1
    
    count = 0
    for cp in codepoints:
        char = chr(cp)
        # Check if font actually has this glyph (non-zero width)
        if font.getlength(char) > 0:
            draw.text((curr_x, y), char, font=font, fill=fill)
            curr_x += spacing
            count += 1
            if count % chars_per_row == 0:
                curr_x = x
                y += line_pitch
    
    if curr_x != x:
        y += line_pitch
    return y

def generate_oled_mockup(ttf_path, font_size, font_name, output_png, data_size, bbx_w, bbx_h, cap_a):
    """Generates a simulated OLED preview with 256px width and structured layout."""
    print(f"[*] Generating 256px preview for {font_name}...")
    
    try:
        target_font = ImageFont.truetype(ttf_path, font_size)
    except Exception as e:
        print(f"[!] Error loading font for preview: {e}")
        return

    ascent, descent = target_font.getmetrics()
    line_pitch = ascent + descent + 1
    
    preview_width = 256
    header_bar_h = 10
    
    # Get ALL codepoints from the compact map
    all_codepoints = get_codepoints_from_map(VN_MAP)
    
    samples = [
        "Tiếng Việt rất giàu và đẹp, đủ để biểu đạt mọi ý tưởng phức tạp nhất.",
        "The quick brown fox jumps over the lazy dog. 1234567890 !@#$%^&*()_+ /\\|+= []{}"
    ]

    header_info = [
        f"u8g2_font_{font_name}",
        f"BBX Width {bbx_w}, Height {bbx_h}, Capital A {cap_a}",
        f"Font Data Size: {data_size} Bytes"
    ]

    # Calculate height estimation
    estimated_height = 1500 
    
    temp_canvas = Image.new('1', (preview_width, estimated_height), color=0)
    temp_draw = ImageDraw.Draw(temp_canvas)
    
    # --- Start Drawing ---
    # 1. Status Bar
    temp_draw.rectangle([(0, 0), (preview_width - 1, header_bar_h - 1)], fill=1)
    current_time = datetime.now().strftime("%H:%M")
    temp_draw.text((2, -1), "U8G2 PREVIEW", font=target_font, fill=0)
    temp_draw.text((preview_width - 2 - temp_draw.textlength(current_time, font=target_font), -1), current_time, font=target_font, fill=0)
    
    y = header_bar_h + 4
    for line in header_info:
        temp_draw.text((2, y), line, font=target_font, fill=1)
        y += line_pitch
    
    y += 6
    # Render DENSE grid of ALL characters
    y = draw_dense_character_grid(temp_draw, all_codepoints, target_font, 4, y, preview_width, line_pitch)
    
    y += 8
    # Render wrapped samples
    for samp in samples:
        y = draw_wrapped_text(temp_draw, samp, target_font, 4, y, preview_width, line_pitch)
        y += 4

    total_height = y + 10
    canvas = temp_canvas.crop((0, 0, preview_width, total_height))
    
    # Upscale logic (Simulated OLED look)
    scale = 3 
    upscaled = canvas.resize((preview_width * scale, total_height * scale), Image.NEAREST).convert('RGB')
    oled_color = (180, 230, 255) # Light blue
    bg_color = (10, 10, 20)      # Dark space blue
    
    pixels = upscaled.load()
    for py in range(upscaled.height):
        for px in range(upscaled.width):
            if px % scale == 0 or py % scale == 0:
                pixels[px, py] = (5, 5, 10)
            elif pixels[px, py] == (255, 255, 255):
                pixels[px, py] = oled_color
            else:
                pixels[px, py] = bg_color
                
    upscaled.save(output_png)
    print(f"[+] 256px Preview saved: {output_png} ({preview_width}x{total_height})")

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
    
    # 2. Call bdfconv
    c_file = f"u8g2_font_{base_name}.c"
    print(f"[*] Compressing BDF for U8g2 using {os.path.basename(BDFCONV_EXE)}...")
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
    out_dir = os.path.join("scripts", "font")
    os.makedirs(out_dir, exist_ok=True)
    
    # Move paths to scripts/font
    target_c_file = os.path.join(out_dir, c_file)
    target_oled_png = os.path.join(out_dir, f"{base_name}_oled_live.png")
    
    # Move the generated .c file
    if os.path.exists(c_file):
        if os.path.exists(target_c_file): os.remove(target_c_file)
        os.rename(c_file, target_c_file)
    
    # Also cleanup .bdf
    if os.path.exists(bdf_file):
        os.remove(bdf_file)

    # 4. Extract metrics and size for preview
    font_for_metrics = ImageFont.truetype(input_ttf, f_size)
    ascent, descent = font_for_metrics.getmetrics()
    bbx_h = ascent + descent
    
    # Get Capital A height
    bbox_a = font_for_metrics.getbbox('A')
    cap_a = bbox_a[3] - bbox_a[1] if bbox_a else ascent
    
    # Estimate max width for BBX
    bbx_w = 0
    for char in "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789":
        bbx_w = max(bbx_w, font_for_metrics.getlength(char))
    bbx_w = int(math.ceil(bbx_w))

    # Read .c file to find data size
    data_size = "0"
    if os.path.exists(target_c_file):
        with open(target_c_file, 'r', encoding='utf-8') as f:
            c_content = f.read()
            size_match = re.search(r'\[(\d+)\]', c_content)
            if size_match:
                data_size = size_match.group(1)

    generate_oled_mockup(input_ttf, f_size, base_name, target_oled_png, data_size, bbx_w, bbx_h, cap_a)
    
    print(f"\n[SUCCESS] Font fixed and converted!")
    print(f"- Output C: {target_c_file}")
    print(f"- Preview (OLED): {target_oled_png}")
    print(f"\nTo apply this font to your code, run:")
    print(f"python scripts/add_font.py {target_c_file}")
