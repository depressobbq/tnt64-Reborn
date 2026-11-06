#!/usr/bin/env python3
"""
N64 / RT64 Dumped Texture Decoder

Batch-converts textures dumped by TNTRecomp (--dump-textures) into clean PNG images.
Accounts for N64Recomp little-endian word storage (byte index ^ 3) and RDP tile sampling format.
By default, extracts textures to 'dumped_textures/png/'.
"""

import sys
import os
import glob
import json
import struct
import argparse
from PIL import Image

# Format constants from N64 GBI
G_IM_FMT_RGBA = 0
G_IM_FMT_YUV  = 1
G_IM_FMT_CI   = 2
G_IM_FMT_IA   = 3
G_IM_FMT_I    = 4

G_IM_SIZ_4b   = 0
G_IM_SIZ_8b   = 1
G_IM_SIZ_16b  = 2
G_IM_SIZ_32b  = 3

def unswap_rdram(data):
    """
    N64Recomp stores 32-bit big-endian words in little-endian host RAM.
    Each byte at logical offset i is located at physical byte index (i ^ 3).
    """
    n = len(data)
    b = bytearray(n)
    for i in range(n):
        swapped_i = i ^ 3
        b[i] = data[swapped_i] if swapped_i < n else data[i]
    return bytes(b)

def rgba16_to_tuple(val):
    r = ((val >> 11) & 0x1F)
    g = ((val >> 6) & 0x1F)
    b = ((val >> 1) & 0x1F)
    a = 255 if (val & 1) else 0
    return ((r << 3) | (r >> 2), (g << 3) | (g >> 2), (b << 3) | (b >> 2), a)

def decode_texture(base_path, tile_info):
    """Decodes texture linearly using RDP tile parameters and unswapped RDRAM/palette."""
    rice_rdram_path = base_path + ".rice.rdram"
    if not os.path.exists(rice_rdram_path):
        return None

    try:
        with open(rice_rdram_path, "rb") as f:
            raw_rdram = f.read()
    except Exception:
        return None

    rdram = unswap_rdram(raw_rdram)
    width = tile_info.get("width", 32)
    height = tile_info.get("height", 32)
    tile = tile_info.get("tile", {})
    fmt = tile.get("fmt", G_IM_FMT_RGBA)
    siz = tile.get("siz", G_IM_SIZ_16b)

    palette = []
    pal_rdram_path = base_path + ".rice.palette.rdram"
    if os.path.exists(pal_rdram_path):
        try:
            with open(pal_rdram_path, "rb") as f:
                raw_pal = f.read()
            pal_data = unswap_rdram(raw_pal)
            for i in range(len(pal_data) // 2):
                val = struct.unpack(">H", pal_data[i*2:(i+1)*2])[0]
                palette.append(rgba16_to_tuple(val))
        except Exception:
            pass

    img = Image.new("RGBA", (width, height))
    pix = img.load()

    if fmt == G_IM_FMT_RGBA:
        if siz == G_IM_SIZ_16b:
            for y in range(height):
                for x in range(width):
                    idx = (y * width + x) * 2
                    if idx + 1 < len(rdram):
                        val = struct.unpack(">H", rdram[idx:idx+2])[0]
                        pix[x, y] = rgba16_to_tuple(val)
        elif siz == G_IM_SIZ_32b:
            for y in range(height):
                for x in range(width):
                    idx = (y * width + x) * 4
                    if idx + 3 < len(rdram):
                        pix[x, y] = (rdram[idx], rdram[idx+1], rdram[idx+2], rdram[idx+3])
    elif fmt == G_IM_FMT_CI:
        if siz == G_IM_SIZ_8b:
            for y in range(height):
                for x in range(width):
                    idx = y * width + x
                    if idx < len(rdram):
                        p = rdram[idx]
                        if p < len(palette):
                            pix[x, y] = palette[p]
                        else:
                            pix[x, y] = (p, p, p, 255)
        elif siz == G_IM_SIZ_4b:
            for y in range(height):
                for x in range(width):
                    byte_idx = (y * width + x) >> 1
                    if byte_idx < len(rdram):
                        b = rdram[byte_idx]
                        p = (b & 0xF) if (x & 1) else ((b >> 4) & 0xF)
                        if p < len(palette):
                            pix[x, y] = palette[p]
                        else:
                            pix[x, y] = (p * 17, p * 17, p * 17, 255)
    elif fmt == G_IM_FMT_IA:
        if siz == G_IM_SIZ_8b:
            for y in range(height):
                for x in range(width):
                    idx = y * width + x
                    if idx < len(rdram):
                        b = rdram[idx]
                        i = ((b >> 4) & 0xF) * 17
                        a = (b & 0xF) * 17
                        pix[x, y] = (i, i, i, a)
        elif siz == G_IM_SIZ_16b:
            for y in range(height):
                for x in range(width):
                    idx = (y * width + x) * 2
                    if idx + 1 < len(rdram):
                        val = struct.unpack(">H", rdram[idx:idx+2])[0]
                        i = (val >> 8) & 0xFF
                        a = val & 0xFF
                        pix[x, y] = (i, i, i, a)
        elif siz == G_IM_SIZ_4b:
            for y in range(height):
                for x in range(width):
                    byte_idx = (y * width + x) >> 1
                    if byte_idx < len(rdram):
                        b = rdram[byte_idx]
                        nibble = (b & 0xF) if (x & 1) else ((b >> 4) & 0xF)
                        i = (nibble >> 1) * 36
                        a = 255 if (nibble & 1) else 0
                        pix[x, y] = (i, i, i, a)
    elif fmt == G_IM_FMT_I:
        if siz == G_IM_SIZ_8b:
            for y in range(height):
                for x in range(width):
                    idx = y * width + x
                    if idx < len(rdram):
                        b = rdram[idx]
                        pix[x, y] = (b, b, b, 255)
        elif siz == G_IM_SIZ_4b:
            for y in range(height):
                for x in range(width):
                    byte_idx = (y * width + x) >> 1
                    if byte_idx < len(rdram):
                        b = rdram[byte_idx]
                        nibble = (b & 0xF) if (x & 1) else ((b >> 4) & 0xF)
                        i = nibble * 17
                        pix[x, y] = (i, i, i, 255)

    return img

def main():
    parser = argparse.ArgumentParser(description="Decode RT64 dumped N64 textures to clean PNGs.")
    parser.add_argument("dumped_dir", nargs="?", default="dumped_textures", help="Directory with dumped textures")
    parser.add_argument("output_dir", nargs="?", default="dumped_textures/png", help="Output directory for PNG textures (default: dumped_textures/png)")
    parser.add_argument("--create-manifest", action="store_true", help="Generate an rt64.json database in the output directory")
    args = parser.parse_args()

    if not os.path.isdir(args.dumped_dir):
        print(f"[!] Directory '{args.dumped_dir}' does not exist.")
        print("    Enable 'dump_textures = true' in settings.ini and play the game to dump textures first.")
        return 1

    os.makedirs(args.output_dir, exist_ok=True)

    tile_files = glob.glob(os.path.join(args.dumped_dir, "*.tile.json"))
    if not tile_files:
        print(f"[!] No *.tile.json files found in '{args.dumped_dir}'.")
        return 1

    print(f"[*] Found {len(tile_files)} texture descriptions in '{args.dumped_dir}'")

    decoded_count = 0
    rt64_hashes = []

    for tile_json_path in sorted(tile_files):
        base_name = os.path.basename(tile_json_path).replace(".tile.json", "")
        base_path = os.path.join(args.dumped_dir, base_name)

        try:
            with open(tile_json_path, "r") as f:
                tile_info = json.load(f)
        except Exception as e:
            print(f"[-] Error reading {base_name}: {e}")
            continue

        width = tile_info.get("width", 32)
        height = tile_info.get("height", 32)

        if width <= 0 or height <= 0 or width > 2048 or height > 2048:
            continue

        try:
            img = decode_texture(base_path, tile_info)
            if img is not None:
                out_png = os.path.join(args.output_dir, f"{base_name}.png")
                img.save(out_png)
                decoded_count += 1

                clean_hash = base_name.split(".")[0]
                rt64_hashes.append(clean_hash)
        except Exception as e:
            print(f"[-] Failed to decode {base_name}: {e}")

    if args.create_manifest and rt64_hashes:
        db = {
            "config": {
                "configurationVersion": 3,
                "autoPath": "rt64",
                "defaultOperation": "stall",
                "defaultShift": "none",
                "hashVersion": 1
            },
            "textures": [{"hashes": {"rt64": h}} for h in sorted(set(rt64_hashes))]
        }
        db_path = os.path.join(args.output_dir, "rt64.json")
        with open(db_path, "w") as f:
            json.dump(db, f, indent=4)
        print(f"[+] Created replacement manifest: '{db_path}'")

    print(f"[+] Successfully decoded {decoded_count} textures into '{args.output_dir}/'")
    return 0

if __name__ == "__main__":
    sys.exit(main())
