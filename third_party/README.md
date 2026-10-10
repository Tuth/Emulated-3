# Third-party components (vendored, unmodified)

| Component | Version | Licence | Source |
|---|---|---|---|
| clap/ (CLAP plugin API headers) | 1.2.10 | MIT (clap/LICENSE) | https://github.com/free-audio/clap |
| stb/stb_truetype.h | v1.26 | public domain / MIT (stb/LICENSE) | https://github.com/nothings/stb |
| stb/stb_image_write.h | master 2026-10-06 | public domain / MIT (stb/LICENSE) | https://github.com/nothings/stb |
| fonts/ArchivoNarrow-wght.ttf | Google Fonts main 2026-10-06 | SIL OFL 1.1 (fonts/OFL-ArchivoNarrow.txt) | https://github.com/google/fonts/tree/main/ofl/archivonarrow |
| fonts/NotoSans-Condensed*.ttf | system (Debian fonts-noto-core) | SIL OFL 1.1 (fonts/OFL-NotoSans.txt) | https://github.com/notofonts |

Not vendored (test tool only): clap-validator 0.4.1 (MIT) at ~/EIII_RE/tools/clap-validator.
No E-mu ROM, OS, sample or bank material is included here. The fonts are compiled into the GUI/plugin (ui/fonts.S);
their OFL licence files must accompany any binary distribution.

The LCD's character set (panel/eiii_lcd_font.inc, codes 0x20-0x7F in the HD44780 A00 layout) is an original drawing made
for this project by tools/gen_lcd_font.py — no character-ROM dump is included.  A real A00 dump can be supplied at run
time with EIII_LCD_CGROM=FILE (4096 bytes, MAME layout); the hd44780_a00.bin found next to the boot ROMs on this machine
is an all-zero placeholder and is rejected.
