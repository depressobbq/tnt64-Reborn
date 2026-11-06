#!/usr/bin/env python3
"""
tools/texture_stitcher.py - Two-way Texture Reassembly and Slicing Tool for RT64.

This tool solves the N64 TMEM chopping problem:
1. 'assemble': Groups dumped texture slices into coherent composite sheets (e.g. 400x300 backgrounds,
   character spritesheets, UI panels) and generates an assembly manifest.
2. 'split': Reads upscaled versions of those sheets (e.g. 4x or 8x AI upscaled), automatically detects
   the scale factor, slices them back into replacement tiles with the exact coordinates and original
   RT64 hashes, and creates 'textures/rt64.json' ready for hot-loading in-game.
"""

import argparse
import glob
import hashlib
import json
import os
import shutil
import sys
from collections import defaultdict
from PIL import Image


def load_texture_dump_metadata(dumped_dir):
    """
    Scans dumped_dir for *.rice.json and corresponding *.tile.json files.
    Returns a list of tile dicts.
    """
    rice_files = sorted(glob.glob(os.path.join(dumped_dir, "*.rice.json")))
    if not rice_files:
        print(f"[-] No .rice.json files found in '{dumped_dir}'")
        return []

    entries = []
    png_dir = os.path.join(dumped_dir, "png")

    for rfile in rice_files:
        base_name = os.path.basename(rfile).replace(".rice.json", "")
        tile_file = rfile.replace(".rice.json", ".tile.json")
        if not os.path.exists(tile_file):
            continue

        try:
            with open(rfile, "r") as f:
                rice_info = json.load(f)
            with open(tile_file, "r") as f:
                tile_info = json.load(f)
        except Exception as e:
            print(f"[-] Error reading metadata for {base_name}: {e}")
            continue

        png_path = os.path.join(png_dir, f"{base_name}.png")
        if not os.path.exists(png_path):
            png_path = os.path.join(dumped_dir, f"{base_name}.png")
            if not os.path.exists(png_path):
                continue

        tex = rice_info.get("texture", {})
        tile = rice_info.get("tile", {})
        addr = tex.get("address", 0)
        fmt = tile.get("fmt", 0)
        siz = tile.get("siz", 0)
        uls = tile.get("uls", 0) >> 2
        ult = tile.get("ult", 0) >> 2
        w = tile_info.get("width", 32)
        h = tile_info.get("height", 32)

        pal_file = rfile.replace(".rice.json", ".rice.palette.rdram")
        pal_hash = None
        if os.path.exists(pal_file):
            with open(pal_file, "rb") as pf:
                pal_hash = hashlib.md5(pf.read()).hexdigest()

        entries.append({
            "hash": base_name,
            "address": addr,
            "width": w,
            "height": h,
            "uls": uls,
            "ult": ult,
            "fmt": fmt,
            "siz": siz,
            "pal_hash": pal_hash,
            "png_path": png_path
        })

    return entries


def assemble_sheets(dumped_dir, output_dir):
    """
    Assembles dumped texture slices into full composite sheets, spritesheets, and standalone files.
    """
    entries = load_texture_dump_metadata(dumped_dir)
    if not entries:
        print("[-] No valid dumped textures found to assemble.")
        return 1

    sheets_dir = os.path.join(output_dir, "sheets")
    spritesheets_dir = os.path.join(output_dir, "spritesheets")
    standalone_dir = os.path.join(output_dir, "standalone")

    # Clean existing destination directories to ensure no stale files remain
    for d in [sheets_dir, spritesheets_dir, standalone_dir]:
        if os.path.exists(d):
            shutil.rmtree(d)
        os.makedirs(d, exist_ok=True)

    # Group textures by: (address, width, fmt, siz, pal_hash)
    groups = defaultdict(list)
    for entry in entries:
        key = (
            entry["address"],
            entry["width"],
            entry["fmt"],
            entry["siz"],
            entry["pal_hash"]
        )
        groups[key].append(entry)

    manifest = {
        "version": 1,
        "sheets": {},
        "spritesheets": {},
        "standalone": []
    }

    sheet_count = 0
    sprite_count = 0
    standalone_count = 0
    total_slices_assembled = 0

    for key, slices in sorted(groups.items(), key=lambda x: -len(x[1])):
        addr, w, fmt, siz, pal = key
        unique_ys = set(s["ult"] for s in slices)

        # 1. Spatial composite sheet (sliced background, playfield, or large panel)
        # Check vertical coverage to differentiate continuous artwork from isolated font glyphs
        is_composite = False
        if addr != 0 and len(slices) > 1 and len(unique_ys) > 1:
            max_y = max(s["ult"] + s["height"] for s in slices)
            total_slice_h = sum(s["height"] for s in slices)
            coverage = total_slice_h / float(max_y) if max_y > 0 else 0
            # Slices must cover at least 65% of the canvas height to be considered a continuous image
            if coverage >= 0.65:
                is_composite = True

        if is_composite:
            sheet_count += 1
            max_x = max(s["uls"] + s["width"] for s in slices)
            max_y = max(s["ult"] + s["height"] for s in slices)

            pal_tag = f"_pal_{pal[:6]}" if pal else ""
            sheet_filename = f"sheet_{sheet_count:02d}_0x{addr:08X}_{max_x}x{max_y}{pal_tag}.png"
            sheet_path = os.path.join(sheets_dir, sheet_filename)

            canvas = Image.new("RGBA", (max_x, max_y), (0, 0, 0, 0))
            slice_records = []

            for s in sorted(slices, key=lambda item: (item["ult"], item["uls"])):
                strip = Image.open(s["png_path"]).convert("RGBA")
                canvas.paste(strip, (s["uls"], s["ult"]))
                slice_records.append({
                    "hash": s["hash"],
                    "box": [s["uls"], s["ult"], s["width"], s["height"]]
                })
                total_slices_assembled += 1

            canvas.save(sheet_path)
            manifest["sheets"][sheet_filename] = {
                "width": max_x,
                "height": max_y,
                "relative_path": os.path.join("sheets", sheet_filename),
                "slices": slice_records
            }

        # 2. Dynamic animation sequence / repeated buffer sharing same coordinates
        elif len(slices) > 1 and len(unique_ys) == 1:
            sprite_count += 1
            frame_w = slices[0]["width"]
            frame_h = slices[0]["height"]
            total_w = frame_w * len(slices)

            pal_tag = f"_pal_{pal[:6]}" if pal else ""
            sprite_filename = f"anim_{sprite_count:02d}_0x{addr:08X}_{len(slices)}frames_{total_w}x{frame_h}{pal_tag}.png"
            sprite_path = os.path.join(spritesheets_dir, sprite_filename)

            canvas = Image.new("RGBA", (total_w, frame_h), (0, 0, 0, 0))
            slice_records = []

            for idx, s in enumerate(slices):
                frame_img = Image.open(s["png_path"]).convert("RGBA")
                x_pos = idx * frame_w
                canvas.paste(frame_img, (x_pos, 0))
                slice_records.append({
                    "hash": s["hash"],
                    "box": [x_pos, 0, s["width"], s["height"]]
                })
                total_slices_assembled += 1

            canvas.save(sprite_path)
            manifest["spritesheets"][sprite_filename] = {
                "width": total_w,
                "height": frame_h,
                "frame_width": frame_w,
                "frame_height": frame_h,
                "relative_path": os.path.join("spritesheets", sprite_filename),
                "slices": slice_records
            }

        # 3. Standalone textures (including isolated glyphs from sparse tables)
        else:
            for s in slices:
                standalone_count += 1
                clean_hash = s["hash"].split(".")[0]
                out_path = os.path.join(standalone_dir, f"{clean_hash}.png")
                shutil.copy2(s["png_path"], out_path)
                manifest["standalone"].append({
                    "hash": s["hash"],
                    "width": s["width"],
                    "height": s["height"],
                    "relative_path": os.path.join("standalone", f"{clean_hash}.png")
                })

    manifest_path = os.path.join(output_dir, "assembly_manifest.json")
    with open(manifest_path, "w") as f:
        json.dump(manifest, f, indent=4)

    print("\n" + "=" * 60)
    print(" [✓] Texture Assembly Complete!")
    print(f" - Continuous Artwork Sheets:     {sheet_count} (in 'sheets/')")
    print(f" - Animation Strips Assembled:     {sprite_count} (in 'spritesheets/')")
    print(f" - Standalone Textures Copied:    {standalone_count} (in 'standalone/')")
    print(f" - Slices Unified into Sheets:    {total_slices_assembled} of {len(entries)}")
    print(f" - Manifest saved to:             '{manifest_path}'")
    print("=" * 60 + "\n")
    return 0


def split_sheets(assembled_dir, output_dir, manifest_path=None):
    """
    Reads upscaled sheets from assembled_dir, slices them back into replacement tiles
    according to manifest, copies standalone textures, and creates textures/rt64.json.
    """
    if manifest_path is None:
        manifest_path = os.path.join(assembled_dir, "assembly_manifest.json")
        if not os.path.exists(manifest_path):
            alt_path = os.path.join("dumped_textures", "assembled", "assembly_manifest.json")
            if os.path.exists(alt_path):
                manifest_path = alt_path

    if not os.path.exists(manifest_path):
        print(f"[-] Assembly manifest not found at '{manifest_path}'")
        return 1

    with open(manifest_path, "r") as f:
        manifest = json.load(f)

    os.makedirs(output_dir, exist_ok=True)
    rt64_hashes = []
    slices_extracted = 0
    standalone_copied = 0

    all_sheets = {}
    all_sheets.update(manifest.get("sheets", {}))
    all_sheets.update(manifest.get("spritesheets", {}))

    print(f"[*] Processing {len(all_sheets)} composite/spritesheet entries...")

    for sheet_name, sheet_info in all_sheets.items():
        rel_path = sheet_info.get("relative_path", sheet_name)
        candidate_paths = [
            os.path.join(assembled_dir, rel_path),
            os.path.join(assembled_dir, sheet_name),
            os.path.join(assembled_dir, "sheets", sheet_name),
            os.path.join(assembled_dir, "spritesheets", sheet_name)
        ]

        found_path = None
        for p in candidate_paths:
            if os.path.exists(p):
                found_path = p
                break

        if not found_path:
            continue

        orig_w = sheet_info["width"]
        orig_h = sheet_info["height"]

        with Image.open(found_path) as sheet_img:
            upscaled_img = sheet_img.convert("RGBA")
            curr_w, curr_h = upscaled_img.size

        scale_x = curr_w / float(orig_w)
        scale_y = curr_h / float(orig_h)

        print(f" -> Splitting '{os.path.basename(found_path)}' (scale: {scale_x:.2f}x, {curr_w}x{curr_h})")

        for slice_rec in sheet_info.get("slices", []):
            h_name = slice_rec["hash"]
            clean_hash = h_name.split(".")[0]
            box = slice_rec["box"]

            crop_x1 = int(round(box[0] * scale_x))
            crop_y1 = int(round(box[1] * scale_y))
            crop_x2 = int(round((box[0] + box[2]) * scale_x))
            crop_y2 = int(round((box[1] + box[3]) * scale_y))

            crop_x1 = max(0, min(curr_w, crop_x1))
            crop_y1 = max(0, min(curr_h, crop_y1))
            crop_x2 = max(crop_x1 + 1, min(curr_w, crop_x2))
            crop_y2 = max(crop_y1 + 1, min(curr_h, crop_y2))

            slice_img = upscaled_img.crop((crop_x1, crop_y1, crop_x2, crop_y2))

            # Save clean replacement image
            out_png = os.path.join(output_dir, f"{clean_hash}.png")
            slice_img.save(out_png)

            rt64_hashes.append(clean_hash)
            slices_extracted += 1

    # Process standalone textures
    for s_info in manifest.get("standalone", []):
        h_name = s_info["hash"]
        clean_hash = h_name.split(".")[0]
        rel_path = s_info.get("relative_path", f"{clean_hash}.png")
        candidate_paths = [
            os.path.join(assembled_dir, rel_path),
            os.path.join(assembled_dir, "standalone", f"{clean_hash}.png"),
            os.path.join(assembled_dir, f"{clean_hash}.png"),
            os.path.join(assembled_dir, f"{h_name}.png")
        ]

        found_path = None
        for p in candidate_paths:
            if os.path.exists(p):
                found_path = p
                break

        if found_path:
            out_png = os.path.join(output_dir, f"{clean_hash}.png")
            shutil.copy2(found_path, out_png)

            rt64_hashes.append(clean_hash)
            standalone_copied += 1

    # Generate rt64.json
    unique_hashes = sorted(set(rt64_hashes))
    if unique_hashes:
        db = {
            "configuration": {
                "configurationVersion": 3,
                "autoPath": "rt64",
                "defaultOperation": "stall",
                "defaultShift": "none",
                "hashVersion": 5
            },
            "textures": [
                {
                    "hashes": {"rt64": h},
                    "path": f"{h}.png"
                }
                for h in unique_hashes
            ]
        }
        db_path = os.path.join(output_dir, "rt64.json")
        with open(db_path, "w") as f:
            json.dump(db, f, indent=4)
        print(f"[+] Created RT64 replacement database: '{db_path}'")

    print("\n" + "=" * 60)
    print(" [✓] Texture Disassembly and Splitting Complete!")
    print(f" - Slices Extracted into '{output_dir}/':  {slices_extracted}")
    print(f" - Standalone Textures Copied:           {standalone_copied}")
    print(f" - Total Replaced Textures:             {len(unique_hashes)}")
    print("=" * 60 + "\n")
    return 0


def main():
    parser = argparse.ArgumentParser(
        description="Two-way Texture Reassembly and Slicing Tool for RT64 Replacement Packs"
    )
    subparsers = parser.add_subparsers(dest="command", required=True)

    # assemble subcommand
    assemble_cmd = subparsers.add_parser(
        "assemble",
        help="Stitch dumped raw texture slices into full composite sheets and spritesheets"
    )
    assemble_cmd.add_argument(
        "--dumped-dir",
        default="dumped_textures",
        help="Path to dumped textures folder containing *.rice.json and png/ (default: dumped_textures)"
    )
    assemble_cmd.add_argument(
        "--output-dir",
        default="dumped_textures/assembled",
        help="Path to save assembled sheets and manifest (default: dumped_textures/assembled)"
    )

    # split subcommand
    split_cmd = subparsers.add_parser(
        "split",
        help="Slice upscaled sheets back into replacement tiles and generate textures/rt64.json"
    )
    split_cmd.add_argument(
        "--assembled-dir",
        default="dumped_textures/assembled",
        help="Path to directory containing upscaled sheets (default: dumped_textures/assembled)"
    )
    split_cmd.add_argument(
        "--output-dir",
        default="textures",
        help="Path to save replacement slices and rt64.json (default: textures)"
    )
    split_cmd.add_argument(
        "--manifest",
        default=None,
        help="Path to assembly_manifest.json (default: <assembled-dir>/assembly_manifest.json)"
    )

    args = parser.parse_args()

    if args.command == "assemble":
        return assemble_sheets(args.dumped_dir, args.output_dir)
    elif args.command == "split":
        return split_sheets(args.assembled_dir, args.output_dir, args.manifest)

    return 0


if __name__ == "__main__":
    sys.exit(main())
