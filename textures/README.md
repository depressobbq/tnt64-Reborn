# HD Texture Replacements

Place custom high-resolution textures (PNG or DDS) into this directory along with `rt64.json` to replace native N64 textures.

## How Texture Replacement Works
- By default, the game renders the authentic native textures.
- When you provide replacement images in this folder alongside `rt64.json`, RT64 hot-loads them into the graphics pipeline with zero performance penalty.
- Textures are matched by their 64-bit hash (e.g. `<hash>.png`).

## Dumping Native Textures
1. Enable dumping by running:
   ```bash
   ./play.sh --dump-textures
   ```
   Or set `dump_textures = true` under `[display]` in `settings.ini`.
2. Play through matches, menus, and backgrounds. The game saves dumped textures into `dumped_textures/`.
3. To view or work with dumped textures as PNG images, run:
   ```bash
   python3 tools/process_dumped_textures.py
   ```
   This decodes all raw textures cleanly into `dumped_textures/png/`.
4. To replace a texture, copy its `<hash>.png` into `textures/`, edit or AI-upscale it, and configure `textures/rt64.json`.
