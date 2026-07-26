#!/usr/bin/env node
// Generate LVGL font with Noto Sans SC (full CJK support)
const fs = require('fs');
const { execSync } = require('child_process');

// Read character list
let charStr = fs.readFileSync('temp_all_chars.txt', 'utf-8');
// Remove characters that Windows cmd can't handle in command line
charStr = charStr.replace(/[><|&^"]/g, '');
console.log(`Characters: ${charStr.length}`);

const sizes = [
  { size: 16, name: 'font_puhui_16_4' },
  { size: 20, name: 'font_puhui_20_4' },
  { size: 30, name: 'font_puhui_30_4' },
];

for (const { size, name } of sizes) {
  const output = `managed_components/78__xiaozhi-fonts/src/${name}.c`;
  const cmd = `lv_font_conv --font "C:\\Windows\\Fonts\\NotoSansSC-VF.ttf" --size ${size} --bpp 4 --format lvgl -r 0x20-0x7e -r 0x3000-0x303f -r 0x4e00-0x9fa5 -r 0xff01-0xff60 --no-compress -o "${output}" --lv-include lvgl.h --lv-font-name ${name}`;

  console.log(`Generating ${name} (${size}px)...`);
  try {
    execSync(cmd, { stdio: 'pipe', maxBuffer: 50 * 1024 * 1024 });
    const stats = fs.statSync(output);
    console.log(`  OK: ${(stats.size / 1024 / 1024).toFixed(1)} MB`);
  } catch (e) {
    console.log(`  ERROR: ${e.message}`);
  }
}
console.log('Done!');
