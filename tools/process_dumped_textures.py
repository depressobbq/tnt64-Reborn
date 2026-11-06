#!/usr/bin/env python3
"""
tools/process_dumped_textures.py - Cleanly decode dumped textures from RT64/N64ModernRuntime.

Decodes textures according to RDP sampler tile formats (RGBA16, RGBA32, CI8/CI4 with palette,
IA8, IA16, IA4, I8, I4) and accounts for little-endian host RAM byte swapping (i ^ 3) relative
to the global RDRAM address.
"""

import argparse
import glob
import json
import os
import struct
import sys
from PIL import Image

# N64 RDP Formats
G_IM_FMT_RGBA = 0
G_IM_FMT_YUV  = 1
G_IM_FMT_CI   = 2
G_IM_FMT_IA   = 3
G_IM_FMT_I    = 4

# N64 RDP Sizes
G_IM_SIZ_4b   = 0
G_IM_SIZ_8b   = 1
G_IM_SIZ_16b  = 2
G_IM_SIZ_32b  = 3

def unswap_rdram(data, rdram_start=0):
    """
    N64Recomp stores 32-bit big-endian words in little-endian host RAM.
    Each byte at logical global address G is located at physical byte index (G ^ 3).
    When reading a slice dumped from rdram_start, local byte index i maps to:
    (rdram_start + i) ^ 3 - rdram_start.
    """
    n = len(data)
    b = bytearray(n)
    for i in range(n):
        global_addr = rdram_start + i
        swapped_global = global_addr ^ 3
        local_swapped = swapped_global - rdram_start
        if 0 <= local_swapped < n:
            b[i] = data[local_swapped]
        else:
            b[i] = data[i]
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

    width = tile_info.get("width", 32)
    height = tile_info.get("height", 32)
    tile = tile_info.get("tile", {})
    fmt = tile.get("fmt", G_IM_FMT_RGBA)
    siz = tile.get("siz", G_IM_SIZ_16b)

    rdram_start = 0
    rice_json_path = base_path + ".rice.json"
    if os.path.exists(rice_json_path):
        try:
            with open(rice_json_path, "r") as f:
                rinfo = json.load(f)
            tex = rinfo.get("texture", {})
            rtile = rinfo.get("tile", {})
            addr = tex.get("address", 0)
            uls = rtile.get("uls", 0) >> 2
            ult = rtile.get("ult", 0) >> 2
            tex_siz = tex.get("siz", siz)
            bpr = tex.get("width", width) << tex_siz >> 1
            rdram_start = addr + (uls << tex_siz >> 1) + bpr * ult
        except Exception:
            rdram_start = 0

    rdram = unswap_rdram(raw_rdram, rdram_start)

    palette = []
    pal_rdram_path = base_path + ".rice.palette.rdram"
    if os.path.exists(pal_rdram_path):
        try:
            with open(pal_rdram_path, "rb") as f:
                raw_pal = f.read()
            pal_start = 0
            pal_json_path = base_path + ".rice.palette.json"
            if os.path.exists(pal_json_path):
                with open(pal_json_path, "r") as pf:
                    pinfo = json.load(pf)
                ptex = pinfo.get("texture", {})
                ptile = pinfo.get("tile", {})
                p_uls = ptile.get("uls", 0) >> 2
                p_ult = ptile.get("ult", 0) >> 2
                p_siz = ptex.get("siz", 2)
                p_bpr = ptex.get("width", 16) << p_siz >> 1
                pal_start = ptex.get("address", 0) + (p_uls << p_siz >> 1) + p_bpr * p_ult
            pal_data = unswap_rdram(raw_pal, pal_start)
            for i in range(len(pal_data) // 2):
                val = struct.unpack(">H", pal_data[i*2:(i+1)*2])[0]
                palette.append(rgba16_to_tuple(val))
        except Exception:
            pass

    img = Image.new("RGBA", (width, height), (0, 0, 0, 0))
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
                        i = ((nibble >> 1) & 0x7) * 36
                        a = 255 if (nibble & 1) else 0
                        pix[x, y] = (i, i, i, a)
    elif fmt == G_IM_FMT_I:
        if siz == G_IM_SIZ_8b:
            for y in range(height):
                for x in range(width):
                    idx = y * width + x
                    if idx < len(rdram):
                        val = rdram[idx]
                        pix[x, y] = (val, val, val, val)
        elif siz == G_IM_SIZ_4b:
            for y in range(height):
                for x in range(width):
                    byte_idx = (y * width + x) >> 1
                    if byte_idx < len(rdram):
                        b = rdram[byte_idx]
                        val = ((b & 0xF) if (x & 1) else ((b >> 4) & 0xF)) * 17
                        pix[x, y] = (val, val, val, val)

    return img

def main():
    parser = argparse.ArgumentParser(description="Process and decode dumped RT64 textures to PNG")
    parser.add_argument("dump_dir", nargs="?", default="dumped_textures", help="Directory with dumped .rice/.tile files")
    parser.add_argument("output_dir", nargs="?", default="dumped_textures/png", help="Directory to save decoded PNGs")
    parser.add_argument("--create-manifest", action="store_true", help="Generate an rt64.json database in the output directory")
    args = parser.parse_args()

    if not os.path.exists(args.dump_dir):
        print(f"[-] Dump directory '{args.dump_dir}' does not exist.")
        return 1

    os.makedirs(args.output_dir, exist_ok=True)
    tile_files = sorted(glob.glob(os.path.join(args.dump_dir, "*.tile.json")))

    if not tile_files:
        print(f"[-] No .tile.json files found in '{args.dump_dir}'. Run with --dump-textures first.")
        return 1

    print(f"[*] Found {len(tile_files)} dumped texture tiles. Processing...")

    decoded_count = 0
    rt64_hashes = []

    for tile_file in tile_files:
        base_name = os.path.basename(tile_file).replace(".tile.json", "")
        base_path = os.path.join(args.dump_dir, base_name)

        try:
            with open(tile_file, "r") as f:
                tile_info = json.load(f)
        except Exception as e:
            print(f"[-] Error reading {tile_file}: {e}")
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
            "configuration": {
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
