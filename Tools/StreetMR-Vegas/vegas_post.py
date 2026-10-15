# Quest 3 Street: turns vegas_scene.py's light-group passes (scene-linear EXR) into what the app
# loads: the plaza with every switchable light off (<key>.png), and one picture per light group that
# the headset ADDS on top when that group is on (<key>_lights<n>.png: chase1..3, flicker, boardsA/B).
# Haze, glow (a linear bloom, so the parts still add up) and the tone curve are done here so that
# base + group == tonemap(base + group) exactly in the compositor's linear blending.
#
#   blender -b --factory-startup --python vegas_post.py -- <variant> <mode> <in_dir> <out_dir>
#   mode preview: equirect look-dev pictures; mode faces: the cube strips for the app.

import math
import os
import struct
import sys
import zlib

import bpy
import numpy as np

args = sys.argv[sys.argv.index("--") + 1:]
VARIANT, MODE, IN, OUT = args[0], args[1], args[2], args[3]
FACE = int(args[4]) if len(args) > 4 else 1536
LIGHT_FACE = int(args[5]) if len(args) > 5 else 1024
OVERSCAN = 1.15
GROUPS = ["chase1", "chase2", "chase3", "flicker", "boardsA", "boardsB"]
KEY = {"night": "streetmr_loading", "dusk": "streetmr_menu"}[VARIANT]
os.makedirs(OUT, exist_ok=True)

LOOK = {
    "night": dict(exposure=1.25, haze=(0.20, 0.07, 0.22), haze_amount=0.55, bloom=0.10),
    "dusk": dict(exposure=1.0, haze=(0.75, 0.42, 0.42), haze_amount=0.6, bloom=0.08),
}[VARIANT]


def load_exr(path):
    img = bpy.data.images.load(path, check_existing=False)
    w, h = img.size
    px = np.empty(w * h * 4, np.float32)
    img.pixels.foreach_get(px)
    bpy.data.images.remove(img)
    return np.flipud(px.reshape(h, w, 4)[..., :3]).copy()


def pass_path(prefix, name):
    return os.path.join(IN, f"{prefix}_{name}_0001.exr")


def resize(img, h, w):
    """Bilinear resize (numpy only)."""
    ih, iw = img.shape[:2]
    ys = (np.arange(h) + 0.5) * ih / h - 0.5
    xs = (np.arange(w) + 0.5) * iw / w - 0.5
    y0 = np.clip(np.floor(ys).astype(int), 0, ih - 1)
    x0 = np.clip(np.floor(xs).astype(int), 0, iw - 1)
    y1 = np.clip(y0 + 1, 0, ih - 1)
    x1 = np.clip(x0 + 1, 0, iw - 1)
    fy = np.clip(ys - y0, 0, 1)[:, None, None]
    fx = np.clip(xs - x0, 0, 1)[None, :, None]
    a = img[y0][:, x0]
    b = img[y0][:, x1]
    c = img[y1][:, x0]
    d = img[y1][:, x1]
    return (a * (1 - fx) + b * fx) * (1 - fy) + (c * (1 - fx) + d * fx) * fy


def blur3(img):
    out = img.copy()
    for axis in (0, 1):
        out = (np.roll(out, 1, axis) + out * 2 + np.roll(out, -1, axis)) * 0.25
    return out


def bloom(img, strength):
    """Linear glow pyramid: halve, soften, bring back up, add. Bright neon (values over 1) carries
    it; being linear, bloom(a + b) == bloom(a) + bloom(b)."""
    h, w = img.shape[:2]
    acc = np.zeros_like(img)
    level = img
    weights = [0.0, 0.35, 0.3, 0.25, 0.2, 0.15, 0.12]
    for k in range(1, len(weights)):
        lh, lw = level.shape[:2]
        if lh < 4 or lw < 4:
            break
        level = level[: lh // 2 * 2, : lw // 2 * 2]
        level = level.reshape(lh // 2, 2, lw // 2, 2, 3).mean(axis=(1, 3))
        level = blur3(blur3(level))
        acc += weights[k] * resize(level, h, w)
    return img + strength * acc


def haze(img, mist, is_base):
    # the sky itself (mist 1) keeps its own colours: only buildings and the ground get hazier
    k = (np.clip(mist, 0, 1) * (mist < 0.999))[..., None] * LOOK["haze_amount"]
    if is_base:
        return img * (1 - k) + np.array(LOOK["haze"], np.float32) * k
    return img * (1 - k)


def tonemap(x):
    """ACES (Narkowicz fit) after exposure: display-linear 0..1."""
    x = np.maximum(x * LOOK["exposure"], 0)
    return np.clip((x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14), 0, 1)


def srgb(x):
    x = np.clip(x, 0, 1)
    return np.where(x <= 0.0031308, 12.92 * x, 1.055 * np.power(x, 1 / 2.4) - 0.055)


def write_png(path, rgb01):
    data = (np.clip(rgb01, 0, 1) * 255 + 0.5).astype(np.uint8)
    h, w = data.shape[:2]
    raw = b"".join(b"\x00" + data[y].tobytes() for y in range(h))

    def chunk(tag, body):
        c = struct.pack(">I", len(body)) + tag + body
        return c + struct.pack(">I", zlib.crc32(tag + body) & 0xFFFFFFFF)

    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n")
        f.write(chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)))
        f.write(chunk(b"IDAT", zlib.compress(raw, 6)))
        f.write(chunk(b"IEND", b""))
    print("WROTE", path, w, h, flush=True)


def develop(prefix):
    """Base (lit by the always-on lights) and each group, hazed and glowing, still linear."""
    mist = load_exr(pass_path(prefix, "mist"))[..., 0]
    base = bloom(haze(load_exr(pass_path(prefix, "base")), mist, True), LOOK["bloom"])
    groups = {}
    for g in GROUPS:
        p = pass_path(prefix, g)
        groups[g] = bloom(haze(load_exr(p), mist, False), LOOK["bloom"]) if os.path.exists(p) \
            else np.zeros_like(base)
    return base, groups


if MODE == "preview":
    base, groups = develop(f"{VARIANT}_pano")
    write_png(os.path.join(OUT, f"{VARIANT}_preview_off.png"), srgb(tonemap(base)))
    on = base + groups["chase1"] + groups["flicker"] + groups["boardsA"]
    write_png(os.path.join(OUT, f"{VARIANT}_preview_on.png"), srgb(tonemap(on)))
else:
    n = int(round(FACE * OVERSCAN))
    off = (n - FACE) // 2
    base_faces = []
    light_faces = {g: [] for g in GROUPS}
    for fi in range(6):
        base, groups = develop(f"{VARIANT}_face{fi}")
        crop = lambda a: np.ascontiguousarray(a[off:off + FACE, off:off + FACE][:, ::-1])
        tb = tonemap(base)
        base_faces.append(srgb(crop(tb)))
        for g in GROUPS:
            # what this group adds once tone-mapped together with the base (display-linear)
            add = np.clip(tonemap(base + groups[g]) - tb, 0, 1)
            add = crop(add)
            if LIGHT_FACE != FACE:
                add = resize(add, LIGHT_FACE, LIGHT_FACE)
            light_faces[g].append(srgb(add))
        print("POST_FACE", fi, flush=True)
    write_png(os.path.join(OUT, f"{KEY}.png"), np.concatenate(base_faces, axis=1))
    for i, g in enumerate(GROUPS):
        write_png(os.path.join(OUT, f"{KEY}_lights{i + 1}.png"), np.concatenate(light_faces[g], axis=1))
print("POST_DONE", VARIANT, MODE, flush=True)
