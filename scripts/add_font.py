import sys
import os
import re

def add_font(c_file):
    """
    Adds a font .c file to the project, updating CMakeLists.txt and display_config.h.
    Ensures #include <stdint.h> and #include <u8g2.h> are present.
    """
    if not os.path.exists(c_file):
        print(f"Error: Font file '{c_file}' not found.")
        return

    # 1. Prepare Content
    with open(c_file, 'r', encoding='utf-8') as f:
        content = f.read()

    # Prepend includes if missing
    new_includes = ""
    if "#include <stdint.h>" not in content:
        new_includes += "#include <stdint.h>\n"
    if "#include <u8g2.h>" not in content:
        new_includes += "#include <u8g2.h>\n"
    
    if new_includes:
        # Insert at the top, but after any existing comments if possible
        if content.lstrip().startswith("/*"):
            # Find end of first comment block
            end_comment = content.find("*/")
            if end_comment != -1:
                content = content[:end_comment+2] + "\n" + new_includes + content[end_comment+2:]
            else:
                content = new_includes + "\n" + content
        else:
            content = new_includes + "\n" + content

    # 2. Extract Font Name (e.g., u8g2_font_iosevkacharonmono_light_11)
    match = re.search(r'const uint8_t (u8g2_font_\w+)\[', content)
    if not match:
        print("[-] Could not detect font name in .c file.")
        return
    font_name = match.group(1)
    base_name = os.path.basename(c_file)
    print(f"[*] Detected font: {font_name}")

    # 3. Save to Project
    target_path = f"main/display/{base_name}"
    # Ensure directory exists (though it should)
    os.makedirs("main/display", exist_ok=True)
    
    with open(target_path, 'w', encoding='utf-8') as f:
        f.write(content)
    print(f"[+] Saved to {target_path}")

    # 4. Update main/CMakeLists.txt
    cmake_path = "main/CMakeLists.txt"
    if os.path.exists(cmake_path):
        with open(cmake_path, 'r', encoding='utf-8') as f:
            cmake_content = f.read()
        
        rel_path = f"display/{base_name}"
        if rel_path not in cmake_content:
            # Add after display_manager.c or similar
            # Look for SRCS... )
            srcs_pattern = re.compile(r'(SRCS\s+.*?)\)', re.DOTALL)
            match_srcs = srcs_pattern.search(cmake_content)
            if match_srcs:
                srcs_block = match_srcs.group(1).strip()
                new_srcs_block = srcs_block + f'\n    "{rel_path}"'
                cmake_content = cmake_content.replace(match_srcs.group(1), new_srcs_block)
                with open(cmake_path, 'w', encoding='utf-8') as f:
                    f.write(cmake_content)
                print(f"[+] Added to {cmake_path}")
        else:
            print(f"[*] {rel_path} already in {cmake_path}")

    # 5. Update display_config.h
    config_path = "main/display/display_config.h"
    if os.path.exists(config_path):
        with open(config_path, 'r', encoding='utf-8') as f:
            config_lines = f.readlines()
        
        extern_line = f"extern const uint8_t {font_name}[];\n"
        if extern_line not in config_lines:
            # Find the "Extern Custom Fonts" section
            for i, line in enumerate(config_lines):
                if "Extern Custom Fonts" in line:
                    config_lines.insert(i + 1, extern_line)
                    with open(config_path, 'w', encoding='utf-8') as f:
                        f.writelines(config_lines)
                    print(f"[+] Added extern declaration to {config_path}")
                    break
        else:
            print(f"[*] Extern declaration already in {config_path}")

    print(f"\n[SUCCESS] Font '{font_name}' integrated!")
    print(f"You can now use it in display_config.h by setting a font macro to: {font_name}")

if __name__ == "__main__":
    if len(sys.argv) < 2:
        print("Usage: python scripts/add_font.py <path_to_c_file>")
    else:
        add_font(sys.argv[1])
