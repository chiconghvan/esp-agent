import sys
import os
import re
import urllib.request

# Configuration
PROJ_ROOT = os.getcwd()
DISPLAY_DIR = os.path.join(PROJ_ROOT, 'main', 'display')
CONFIG_H = os.path.join(DISPLAY_DIR, 'display_config.h')
CMAKE_LIST = os.path.join(PROJ_ROOT, 'main', 'CMakeLists.txt')

def switch_font(input_source):
    # 1. Determine if URL or Local Path
    is_url = input_source.startswith("http")
    
    if is_url:
        raw_url = input_source.replace("github.com", "raw.githubusercontent.com").replace("/blob/", "/")
        filename = os.path.basename(raw_url)
        print(f"[*] Downloading font from URL: {filename}...")
        try:
            req = urllib.request.Request(raw_url, headers={'User-Agent': 'Mozilla/5.0'})
            with urllib.request.urlopen(req) as response:
                content = response.read().decode('utf-8')
        except Exception as e:
            print(f"[!] Download error: {e}")
            return
    else:
        # Local file
        if not os.path.exists(input_source):
            print(f"[!] File not found: {input_source}")
            return
        filename = os.path.basename(input_source)
        print(f"[*] Processing local file: {filename}...")
        with open(input_source, 'r', encoding='utf-8', errors='ignore') as f:
            content = f.read()

    target_path = os.path.join(DISPLAY_DIR, filename)

    # 2. Patch file C (add #include <u8g2.h>)
    if '#include <u8g2.h>' not in content:
        content = '#include <u8g2.h>\n\n' + content
    
    # 3. Extract and Sanitize font variable name
    match = re.search(r'const uint8_t\s+([a-zA-Z0-9_\-]+)', content)
    if not match:
        print("[!] Could not find font variable name (u8g2_font_...)")
        return
    
    raw_font_var = match.group(1)
    font_var = raw_font_var.replace('-', '_')
    
    if '-' in raw_font_var:
        print(f"[*] Fixing invalid variable name: {raw_font_var} -> {font_var}")
        content = content.replace(raw_font_var, font_var)
    
    # Save to display directory
    with open(target_path, 'w', encoding='utf-8') as f:
        f.write(content)
    print(f"[+] Saved and patched: {filename}")

    # 4. Update display_config.h
    if os.path.exists(CONFIG_H):
        with open(CONFIG_H, 'r', encoding='utf-8') as f:
            config_lines = f.readlines()
        
        new_config = []
        for line in config_lines:
            if 'extern const uint8_t u8g2_font_' in line and '[]' in line:
                line = f"extern const uint8_t {font_var}[];\n"
            elif '#define DISP_FONT_TASK_TITLE' in line:
                line = f"#define DISP_FONT_TASK_TITLE    {font_var}     // Auto-switched\n"
            elif '#define DISP_FONT_TASK_DUE' in line:
                line = f"#define DISP_FONT_TASK_DUE      {font_var}     // Auto-switched\n"
            new_config.append(line)
            
        with open(CONFIG_H, 'w', encoding='utf-8') as f:
            f.writelines(new_config)
        print("[+] Updated display_config.h")

    # 5. Update CMakeLists.txt
    if os.path.exists(CMAKE_LIST):
        with open(CMAKE_LIST, 'r', encoding='utf-8') as f:
            cmake_content = f.read()
        
        # Replace old font filename with new one
        cmake_content = re.sub(r'"display/u8g2_font_.*?\.c"', f'"display/{filename}"', cmake_content)
        
        with open(CMAKE_LIST, 'w', encoding='utf-8') as f:
            f.write(cmake_content)
        print("[+] Updated CMakeLists.txt")

    # 6. Cleanup
    for f in os.listdir(DISPLAY_DIR):
        if (f.startswith("u8g2_font_") or f.startswith("cascadia")) and f.endswith(".c") and f != filename:
            try:
                os.remove(os.path.join(DISPLAY_DIR, f))
                print(f"[-] Removed old font: {f}")
            except:
                pass

    print(f"\n[OK] Switch to font {font_var} completed!")
    print("Now run: idf.py build flash")

if __name__ == "__main__":
    if len(sys.argv) < 2:
        print("Usage: python scripts/switch_font.py <source>")
    else:
        switch_font(sys.argv[1])

if __name__ == "__main__":
    if len(sys.argv) < 2:
        print("Sử dụng: python scripts/switch_font.py <link_github_font_c>")
    else:
        switch_font(sys.argv[1])
