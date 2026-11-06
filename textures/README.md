# HD Texture Replacements

Place custom high-resolution textures (PNG or DDS) into this directory along with `rt64.json` to replace native N64 textures.

## How Texture Replacement Works
- By default, the game renders the authentic native textures.
- When you provide replacement images in this folder alongside `rt64.json`, RT64 hot-loads them into the graphics pipeline with zero performance penalty.
- Textures are matched by their 64-bit hash (e.g. `<hash>.png`).

---

## The AI Upscaling & Reassembly Workflow

N64 hardware restricted TMEM to 4 KB, causing backgrounds (e.g. 400×300) to be sliced into dozens of tiny 400×5 strips. Upscaling tiny strips with AI causes severe horizontal seam lines and loss of vertical context.

To solve this, we provide a two-way texture stitcher (`tools/texture_stitcher.py`):

### Step 1: Dump & Decode Textures
1. Dump textures while playing:
   ```bash
   ./play.sh --dump-textures
   ```
2. Decode raw dumps into clean PNGs:
   ```bash
   python3 tools/process_dumped_textures.py
   ```

### Step 2: Assemble into Full Artwork Sheets
Stitch the hundreds of tiny slices into coherent full-resolution sheets:
```bash
python3 tools/texture_stitcher.py assemble
```
This generates:
- `dumped_textures/assembled/sheets/`: 20 full-resolution background & scene sheets (e.g. 400×300, 397×251).
- `dumped_textures/assembled/spritesheets/`: 31 unified animation filmstrips.
- `dumped_textures/assembled/standalone/`: 91 individual icons and UI elements.
- `dumped_textures/assembled/assembly_manifest.json`: Exact spatial coordinates for every slice.

### Step 3: Upscale the Assembled Sheets
Feed the full sheets in `dumped_textures/assembled/sheets/` into your AI upscaler of choice (Topaz Gigapixel, Real-ESRGAN, ChaiNNer, etc.) at any scale (2×, 4×, 8×).
Save your upscaled sheets into a folder, e.g. `dumped_textures/assembled_hd/`.

### Step 4: Split & Deploy to Game
Slice the upscaled artwork back into individual replacement tiles and generate `rt64.json`:
```bash
python3 tools/texture_stitcher.py split --assembled-dir dumped_textures/assembled_hd --output-dir textures
```
The tool automatically:
- Detects your upscale multiplier (e.g. 4.0× or 8.0×).
- Crops each tile with sub-pixel precision across internal seams.
- Generates `textures/rt64.json` with all replaced hashes.

Launch the game to enjoy your HD textures seamlessly:
```bash
./play.sh
```
