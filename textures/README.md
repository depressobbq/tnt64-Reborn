# HD Texture Replacements

Place your high-resolution texture replacements (PNG or DDS) in this directory along with `rt64.json`.

## How It Works
- The game automatically detects and loads textures from this folder on launch without any performance impact.
- To dump the native textures from the game, set `dump_textures = true` in `settings.ini` or launch with `./play.sh --dump-textures`.
- Convert the dumped raw textures to PNGs with:
  ```bash
  python3 tools/process_dumped_textures.py
  ```
- Edit, enhance, or AI-upscale any PNGs in this directory.
- The game will automatically replace native N64 textures with your custom textures on boot.
