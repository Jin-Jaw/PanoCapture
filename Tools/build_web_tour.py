"""Builds a web-ready copy of a PanoCapture tour: smaller files, same tour.

    python build_web_tour.py <tour folder> <output folder> [--format avif|webp] [--quality 70]

- Panoramas: AVIF (or WebP). Measured on an 8K capture against a lossless master: AVIF q70 matches JPEG q92 in
  PSNR (47.8 dB) at 0.26 MB instead of 2.04 MB; WebP bands in smooth skies at comparable sizes. The 4K and 2K copies
  are resized from the full image, so each tier is compressed once. Capture with Image Format PNG for the best result
  (a JPEG master gets compressed twice).
- Depth: lossless WebP. The bytes are millimetres, not colours, so they must survive exactly.
- scene.glb: meshopt compression through gltfpack (npm), 16-bit positions (about 1 mm over 60 m).
- Files are renamed with a content hash under assets/, so the host can cache them forever; tour.json, index.html and
  the viewer revalidate every time (see the _headers file, which Cloudflare Pages and Netlify read).

Needs Pillow with WebP/AVIF (pip install pillow) and Node (for npx gltfpack).
"""

import argparse
import hashlib
import json
import os
import shutil
import subprocess
import sys
from concurrent.futures import ProcessPoolExecutor
from pathlib import Path

from PIL import Image

Image.MAX_IMAGE_PIXELS = None  # 8K+ panoramas are expected
VIEWER_FILES = ("index.html", "viewer.js", "viewer.css")
HEADERS = """# Content-hashed files never change: cache them for a year.
/assets/*
  Cache-Control: public, max-age=31536000, immutable

# The tour and the viewer are rewritten in place: always revalidate.
/*
  Cache-Control: no-cache
"""


def hashed_name(path: Path, stem: str) -> str:
    digest = hashlib.sha256(path.read_bytes()).hexdigest()[:10]
    return f"{stem}.{digest}{path.suffix}"


def encode_color(src: Path, out: Path, width: int, fmt: str, quality: int) -> None:
    image = Image.open(src).convert("RGB")
    if image.width != width:
        image = image.resize((width, width // 2), Image.LANCZOS)
    if fmt == "avif":
        image.save(out, "AVIF", quality=quality, speed=4, subsampling="4:4:4")
    else:
        image.save(out, "WEBP", quality=quality, method=6)


def encode_lossless(src: Path, out: Path) -> None:
    # exact: keep every byte, including under (absent) alpha; method 6: smallest file.
    Image.open(src).convert("RGB").save(out, "WEBP", lossless=True, quality=100, method=6, exact=True)


def run_job(job):
    kind, src, out, width, fmt, quality = job
    if kind == "color":
        encode_color(Path(src), Path(out), width, fmt, quality)
    else:
        encode_lossless(Path(src), Path(out))
    return out


def compress_mesh(src: Path, out: Path) -> bool:
    npx = shutil.which("npx.cmd") or shutil.which("npx")
    if not npx:
        return False
    result = subprocess.run([npx, "--yes", "gltfpack@1.2.0", "-i", str(src), "-o", str(out), "-cc", "-vp", "16"],
                            capture_output=True, text=True)
    if result.returncode != 0 or not out.exists():
        print("gltfpack failed, keeping the uncompressed mesh:", result.stderr.strip()[-500:], file=sys.stderr)
        return False
    return True


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("tour", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--format", choices=("avif", "webp"), default="avif")
    parser.add_argument("--quality", type=int, default=None, help="default: 88 for WebP, 70 for AVIF")
    parser.add_argument("--name", help="title shown in the viewer (default: the tour's own name)")
    parser.add_argument("--viewer", type=Path, default=Path(__file__).resolve().parent.parent / "Resources" / "Viewer")
    args = parser.parse_args()
    quality = args.quality or (70 if args.format == "avif" else 88)

    src_dir: Path = args.tour
    tour = json.loads((src_dir / "tour.json").read_text(encoding="utf-8"))
    out_dir: Path = args.output
    staging = out_dir / "_staging"
    if out_dir.exists():
        shutil.rmtree(out_dir)
    (out_dir / "assets").mkdir(parents=True)
    staging.mkdir()

    ext = "." + args.format
    width = int(tour.get("width") or Image.open(src_dir / tour["points"][0]["image"]).width)
    jobs, renames = [], []  # renames: (staged file, stem, setter)

    def stage(kind, src, stem, suffix, tier_width, setter):
        out = staging / (stem + suffix)
        jobs.append((kind, str(src), str(out), tier_width, args.format, quality))
        renames.append((out, stem, setter))

    for point in tour["points"]:
        pid = point["id"]
        master = src_dir / point["image"]
        stage("color", master, pid, ext, width, lambda name, p=point: p.__setitem__("image", name))
        if point.get("medium"):
            stage("color", master, pid + "_4k", ext, 4096, lambda name, p=point: p.__setitem__("medium", name))
        if point.get("preview"):
            preview_width = Image.open(src_dir / point["preview"]).width
            stage("color", master, pid + "_preview", ext, preview_width, lambda name, p=point: p.__setitem__("preview", name))
        if point.get("depth"):
            stage("lossless", src_dir / point["depth"], pid + "_depth", ".webp", 0, lambda name, p=point: p.__setitem__("depth", name))
    for floor in tour.get("floors", []):
        plan = floor.get("plan")
        if plan:
            stage("lossless", src_dir / plan["image"], Path(plan["image"]).stem, ".webp", 0, lambda name, p=plan: p.__setitem__("image", name))

    print(f"Encoding {len(jobs)} images ({args.format}, quality {quality})...")
    with ProcessPoolExecutor() as pool:
        for done in pool.map(run_job, jobs):
            print("  ", Path(done).name)

    if tour.get("mesh"):
        mesh_src = src_dir / tour["mesh"]["file"]
        mesh_out = staging / "scene.glb"
        if not compress_mesh(mesh_src, mesh_out):
            shutil.copyfile(mesh_src, mesh_out)
        renames.append((mesh_out, "scene", lambda name: tour["mesh"].__setitem__("file", name)))

    for staged, stem, setter in renames:
        name = hashed_name(staged, stem)
        staged.replace(out_dir / "assets" / name)
        setter("assets/" + name)
    staging.rmdir()

    if args.name:
        tour["name"] = args.name
    tour["web"] = {"format": args.format, "quality": quality, "mesh": "meshopt" if tour.get("mesh") else None}
    (out_dir / "tour.json").write_text(json.dumps(tour, indent=2), encoding="utf-8")
    for name in VIEWER_FILES:
        shutil.copyfile(args.viewer / name, out_dir / name)
    (out_dir / "_headers").write_text(HEADERS, encoding="utf-8")

    # Compare with only the files the source tour uses (a tour folder can hold leftovers from earlier captures).
    source = json.loads((src_dir / "tour.json").read_text(encoding="utf-8"))
    used = {src_dir / "tour.json", *(src_dir / n for n in VIEWER_FILES)}
    for p in source["points"]:
        used.update(src_dir / p[k] for k in ("image", "medium", "preview", "depth") if p.get(k))
    if source.get("mesh"):
        used.add(src_dir / source["mesh"]["file"])
    used.update(src_dir / f["plan"]["image"] for f in source.get("floors", []) if f.get("plan"))
    before = sum(f.stat().st_size for f in used if f.exists())
    after = sum(f.stat().st_size for f in out_dir.rglob("*") if f.is_file())
    print(f"Done: {before / 2**20:.1f} MB -> {after / 2**20:.1f} MB in {out_dir}")


if __name__ == "__main__":
    main()
