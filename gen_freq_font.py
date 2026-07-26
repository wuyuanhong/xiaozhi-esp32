#!/usr/bin/env python3
"""Generate LVGL font with frequency-ordered Chinese characters."""
import subprocess
import sys
import os

# Read characters from file
with open('temp_all_chars.txt', 'r', encoding='utf-8') as f:
    char_str = f.read()

print(f"Total characters: {len(char_str)}")

# Write to temp file
with open('temp_symbols_freq.txt', 'w', encoding='utf-8') as f:
    f.write(char_str)

# Generate fonts with lv_font_conv
sizes = [(16, 'font_puhui_16_4'), (20, 'font_puhui_20_4'), (30, 'font_puhui_30_4')]

for size, name in sizes:
    output = f"managed_components/78__xiaozhi-fonts/src/{name}.c"
    cmd = [
        r"C:\Users\95382\AppData\Roaming\npm\lv_font_conv.cmd",
        "--font", r"C:\Windows\Fonts\NotoSansSC-VF.ttf",
        "--size", str(size),
        "--bpp", "4",
        "--format", "lvgl",
        "-r", "0x20-0x7e",
        "--symbols", char_str,
        "--no-compress",
        "-o", output,
        "--lv-include", "lvgl.h",
        "--lv-font-name", name
    ]
    print(f"Generating {name} ({size}px)...")
    result = subprocess.run(cmd, capture_output=True, text=True, shell=False)
    if result.returncode != 0:
        print(f"  ERROR: {result.stderr}")
    else:
        size_mb = os.path.getsize(output) / (1024*1024)
        print(f"  OK: {size_mb:.1f} MB")

print("Done!")
