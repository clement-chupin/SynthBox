#!/usr/bin/env python3
"""
to_bvid.py — Convert any video to .bvid for GrvEP OLED playback.

Format .bvid :
  Header 16 octets : "BVID" + width(u16 LE) + height(u16 LE) + fps(u8) + pad(3) + frame_count(u32 LE)
  Chaque frame     : 2048 octets, 1bpp, MSB du byte = pixel gauche (U8g2 drawBitmap)

Usage :
  python3 to_bvid.py video.mp4
  python3 to_bvid.py video.mp4 -o anim.bvid --fps 15
  python3 to_bvid.py video.mp4 --no-dither --threshold 100 --invert

Dépendances :
  ffmpeg + ffprobe dans le PATH
  pip install tqdm   (optionnel : barre de progression)
"""

import sys
import os
import struct
import subprocess
import argparse

FRAME_W = 128
FRAME_H = 128
FRAME_SIZE_1BPP = FRAME_W * FRAME_H // 8   # 2 048 octets
HEADER_SIZE = 16


IMAGE_EXTS = {".png", ".jpg", ".jpeg", ".bmp", ".tiff", ".tif", ".webp", ".ppm", ".pgm"}


def is_image(path):
    return os.path.splitext(path)[1].lower() in IMAGE_EXTS


# ── Dimensions de la vidéo/image source ─────────────────────────────────────
def get_video_size(path):
    # Use -select_streams v:0 for videos; images have no stream selector, use -vframes 1 fallback
    for sel in ["-select_streams v:0", ""]:
        cmd = ["ffprobe", "-v", "error"]
        if sel:
            cmd += sel.split()
        cmd += ["-show_entries", "stream=width,height", "-of", "csv=s=x:p=0", path]
        r = subprocess.run(cmd, capture_output=True, text=True)
        out = r.stdout.strip()
        if r.returncode == 0 and out:
            try:
                w, h = map(int, out.split("x"))
                return w, h
            except ValueError:
                pass
    print("Impossible de lire les dimensions :")
    raise RuntimeError("ffprobe failed")


def compute_fit(orig_w, orig_h):
    """Calcule la taille et l'offset pour centrer la vidéo dans 128×128."""
    scale  = min(FRAME_W / orig_w, FRAME_H / orig_h)
    fit_w  = max(1, int(orig_w * scale))
    fit_h  = max(1, int(orig_h * scale))
    off_x  = (FRAME_W - fit_w) // 2
    off_y  = (FRAME_H - fit_h) // 2
    return fit_w, fit_h, off_x, off_y


# ── Dithering Floyd-Steinberg, contraint à la zone de contenu ───────────────
def _dither_fs(content_bytes, fit_w, fit_h, off_x, off_y, invert):
    """
    Place le contenu dans un canvas 128×128 puis applique FS dithering
    uniquement sur la zone de contenu — les bordures restent noir pur.
    """
    # Canvas noir
    p = [0.0] * (FRAME_W * FRAME_H)

    # Copier le contenu
    for row in range(fit_h):
        src = row * fit_w
        dst = (off_y + row) * FRAME_W + off_x
        for col in range(fit_w):
            p[dst + col] = content_bytes[src + col] / 255.0

    bits = [0] * (FRAME_W * FRAME_H)
    x_end = off_x + fit_w
    y_end = off_y + fit_h

    for y in range(off_y, y_end):
        for x in range(off_x, x_end):
            idx = y * FRAME_W + x
            old = p[idx]
            new = 1.0 if old >= 0.5 else 0.0
            bits[idx] = int(new)
            err = old - new
            # Propager uniquement dans la zone de contenu
            if x + 1 < x_end:
                p[idx + 1]            += err * 7 / 16
            if y + 1 < y_end:
                if x > off_x:
                    p[idx + FRAME_W - 1] += err * 3 / 16
                p[idx + FRAME_W]      += err * 5 / 16
                if x + 1 < x_end:
                    p[idx + FRAME_W + 1] += err * 1 / 16

    if invert:
        bits = [1 - b for b in bits]
    return bits


def _threshold(content_bytes, fit_w, fit_h, off_x, off_y, thr, invert):
    bits = [0] * (FRAME_W * FRAME_H)
    for row in range(fit_h):
        for col in range(fit_w):
            v = content_bytes[row * fit_w + col]
            b = 1 if v >= thr else 0
            bits[(off_y + row) * FRAME_W + (off_x + col)] = b
    if invert:
        bits = [1 - b for b in bits]
    return bits


def _pack_1bpp(bits):
    buf = bytearray(FRAME_SIZE_1BPP)
    for i, bit in enumerate(bits):
        if bit:
            buf[i >> 3] |= 0x80 >> (i & 7)
    return buf


# ── Conversion principale ────────────────────────────────────────────────────
def convert(input_path, output_path, fps, threshold, dither, invert, show_progress):
    orig_w, orig_h = get_video_size(input_path)
    fit_w, fit_h, off_x, off_y = compute_fit(orig_w, orig_h)
    frame_size_gray = fit_w * fit_h

    fills = "plein écran" if (fit_w == FRAME_W and fit_h == FRAME_H) else \
            f"{fit_w}×{fit_h} centré ({off_x}px, {off_y}px)"
    is_img = is_image(input_path)
    src_type = "image" if is_img else "vidéo"
    print(f"→ Source ({src_type}) : {orig_w}×{orig_h}  →  {fills}")

    # FFmpeg : scale exact, pas de pad (le centrage se fait en Python)
    vf = f"scale={fit_w}:{fit_h},format=gray"
    cmd = ["ffmpeg", "-hide_banner", "-loglevel", "error", "-i", input_path, "-vf", vf]
    if not is_img:
        cmd += ["-r", str(fps)]
    cmd += ["-pix_fmt", "gray"]
    if is_img:
        cmd += ["-vframes", "1"]
    cmd += ["-f", "rawvideo", "pipe:1"]

    proc = subprocess.run(cmd, capture_output=True)
    if proc.returncode != 0:
        print("ERREUR FFmpeg :")
        print(proc.stderr.decode(errors="replace"))
        raise RuntimeError("ffmpeg failed")

    raw = proc.stdout
    if len(raw) < frame_size_gray:
        print("Aucune frame extraite — vérifiez le fichier d'entrée.")
        raise RuntimeError("no frames")

    n_frames = len(raw) // frame_size_gray
    print(f"  {n_frames} frames × {fps} fps → {n_frames / fps:.1f}s")

    # Barre de progression
    try:
        from tqdm import tqdm
        iterator = tqdm(range(n_frames), unit="frame") if show_progress else range(n_frames)
    except ImportError:
        iterator = range(n_frames)
        if show_progress:
            print("  (pip install tqdm pour une barre de progression)")

    with open(output_path, "wb") as f:
        f.write(b"BVID")
        f.write(struct.pack("<HHB3BI", FRAME_W, FRAME_H, fps, 0, 0, 0, n_frames))

        for i in iterator:
            content = raw[i * frame_size_gray : (i + 1) * frame_size_gray]
            if dither:
                bits = _dither_fs(content, fit_w, fit_h, off_x, off_y, invert)
            else:
                bits = _threshold(content, fit_w, fit_h, off_x, off_y, threshold, invert)
            f.write(_pack_1bpp(bits))

    size_kb = os.path.getsize(output_path) / 1024
    print(f"✓  {output_path}  ({size_kb:.0f} KB)")


def main():
    parser = argparse.ArgumentParser(
        description="Convertit une vidéo au format .bvid pour GrvEP (128×128 1bpp).",
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument("input", nargs="+", help="Fichier(s) source (mp4, avi, gif, png, jpg…) — supporte les globs : *.mp4 *.png")
    parser.add_argument("-o", "--output",  help="Fichier de sortie .bvid (défaut : même nom)")
    parser.add_argument("--fps",           type=int, default=10,  help="Images/s (défaut : 10)")
    parser.add_argument("--threshold",     type=int, default=128, help="Seuil 0–255 sans dithering (défaut : 128)")
    parser.add_argument("--no-dither",     action="store_true",   help="Seuillage simple (pas de Floyd-Steinberg)")
    parser.add_argument("--invert",        action="store_true",   help="Inverser les pixels")
    parser.add_argument("--no-progress",   action="store_true",   help="Pas de barre de progression")
    args = parser.parse_args()

    # Filtrer : uniquement les fichiers (pas les dossiers ni les .bvid déjà convertis)
    VIDEO_EXTS = {".mp4", ".avi", ".mov", ".mkv", ".webm", ".gif", ".flv", ".wmv", ".m4v", ".ts"}
    ALL_EXTS = VIDEO_EXTS | IMAGE_EXTS
    inputs = [p for p in args.input
              if os.path.isfile(p) and os.path.splitext(p)[1].lower() in ALL_EXTS]

    if not inputs:
        print("Aucun fichier vidéo/image trouvé parmi les arguments.")
        print(f"Extensions supportées : {', '.join(sorted(ALL_EXTS))}")
        sys.exit(1)

    if args.output and len(inputs) > 1:
        print("--output ne peut pas être utilisé avec plusieurs fichiers.")
        sys.exit(1)

    errors = []
    for path in inputs:
        output = args.output or os.path.splitext(path)[0] + ".bvid"
        print(f"\n[{inputs.index(path)+1}/{len(inputs)}] {os.path.basename(path)}")
        try:
            convert(path, output, args.fps, args.threshold, not args.no_dither, args.invert, not args.no_progress)
        except (RuntimeError, Exception) as e:
            print(f"  ✗ Erreur : {e}")
            errors.append(path)

    if errors:
        print(f"\n⚠  {len(errors)} fichier(s) échoué(s) :")
        for e in errors:
            print(f"   {e}")


if __name__ == "__main__":
    main()
