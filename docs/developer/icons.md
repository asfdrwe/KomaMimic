# Icons

The original SVG is `assets/icons/komamimic.svg`; the symbol variant is
`komamimic-symbol.svg`. PNG sizes, the Windows ICO, and the macOS ICNS are in
`assets/icons/`; the desktop entry is in `assets/`. These are original KomaMimic
assets licensed under GPL-3.0-only.

Regeneration requires CairoSVG and Pillow. `python3 tools/generate_icons.py`
updates images in the repository; `--check` compares them without changing files.
Run it in a working copy.
