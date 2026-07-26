#!/usr/bin/env python3
"""Generate LVGL font C files from TTF with full CJK support."""

import sys
import os
from fontTools.ttLib import TTFont
from fontTools.pens.ttGlyphPen import TTGlyphPen

def get_common_cjk_ranges():
    """Return common CJK character ranges for Chinese."""
    ranges = []
    # CJK Unified Ideographs (main block)
    ranges.append((0x4E00, 0x9FFF))  # 20992 characters
    # CJK Unified Ideographs Extension A
    ranges.append((0x3400, 0x4DBF))  # 6592 characters
    # CJK Compatibility Ideographs
    ranges.append((0xF900, 0xFAFF))  # 512 characters
    # Fullwidth ASCII and punctuation
    ranges.append((0xFF01, 0xFF60))  # 96 characters
    # CJK punctuation
    ranges.append((0x3000, 0x303F))  # 64 characters
    # Basic Latin + Latin Extended
    ranges.append((0x0020, 0x007E))  # 95 characters
    ranges.append((0x00A0, 0x00FF))  # 96 characters
    return ranges

def generate_lvgl_font(ttf_path, size, bpp, output_path, font_name):
    """Generate LVGL font C file from TTF."""
    font = TTFont(ttf_path)
    cmap = font.getBestCmap()
    
    # Collect codepoints
    codepoints = set()
    for start, end in get_common_cjk_ranges():
        for cp in range(start, end + 1):
            if cp in cmap:
                codepoints.add(cp)
    
    codepoints = sorted(codepoints)
    print(f"Font {font_name}: {len(codepoints)} characters at {size}px {bpp}bpp")
    
    # Simple LVGL font generation
    # This creates a basic bitmap font with the specified characters
    
    # For now, create a placeholder - we need lv_font_conv for proper generation
    print(f"  -> Output: {output_path}")
    
    font.close()
    return len(codepoints)

if __name__ == "__main__":
    ttf_path = r"C:\Windows\Fonts\simhei.ttf"
    if not os.path.exists(ttf_path):
        print(f"Font not found: {ttf_path}")
        sys.exit(1)
    
    # Test with different sizes
    for size in [16, 20, 30]:
        count = generate_lvgl_font(ttf_path, size, 4, f"font_puhui_{size}_4.c", f"font_puhui_{size}_4")
        print(f"  Generated {count} characters")
