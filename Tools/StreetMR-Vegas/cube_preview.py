# Checks the app's cube strips: rebuilds an equirectangular view from <key>.png (+ light groups
# added in linear light, as the headset's compositor does) with the GL cube convention.
#   blender -b --python cube_preview.py -- <dir> <key> <out.png> [groups e.g. 1,4,5]
import os
import sys

import bpy
import numpy as np

args = sys.argv[sys.argv.index("--") + 1:]
D, KEY, OUT = args[0], args[1], args[2]
GROUPS = [int(g) for g in args[3].split(",")] if len(args) > 3 and args[3] else []


def load(path):
    img = bpy.data.images.load(path, check_existing=False)
    img.colorspace_settings.name = "Non-Color"
    w, h = img.size
    px = np.empty(w * h * 4, np.float32)
    img.pixels.foreach_get(px)
    bpy.data.images.remove(img)
    return np.flipud(px.reshape(h, w, 4)[..., :3]).copy()


def lin(c):
    return np.where(c <= 0.04045, c / 12.92, np.power((c + 0.055) / 1.055, 2.4))


def enc(c):
    c = np.clip(c, 0, 1)
    return np.where(c <= 0.0031308, 12.92 * c, 1.055 * np.power(c, 1 / 2.4) - 0.055)


W, H = 2048, 1024
v, u = np.mgrid[0:H, 0:W]
az = ((u + 0.5) / W - 0.5) * 2 * np.pi
el = (0.5 - (v + 0.5) / H) * np.pi
x = np.cos(el) * np.sin(az)
y = np.sin(el)
z = -np.cos(el) * np.cos(az)
ax, ay, az_ = np.abs(x), np.abs(y), np.abs(z)
face = np.where((ax >= ay) & (ax >= az_), np.where(x > 0, 0, 1),
                np.where(ay >= az_, np.where(y > 0, 2, 3), np.where(z > 0, 4, 5)))
s = np.zeros_like(x)
t = np.zeros_like(x)
m = face == 0; s[m] = -z[m] / ax[m]; t[m] = -y[m] / ax[m]
m = face == 1; s[m] = z[m] / ax[m]; t[m] = -y[m] / ax[m]
m = face == 2; s[m] = x[m] / ay[m]; t[m] = z[m] / ay[m]
m = face == 3; s[m] = x[m] / ay[m]; t[m] = -z[m] / ay[m]
m = face == 4; s[m] = x[m] / az_[m]; t[m] = -y[m] / az_[m]
m = face == 5; s[m] = -x[m] / az_[m]; t[m] = -y[m] / az_[m]


def sample(strip):
    n = strip.shape[0]
    col = np.clip(((s + 1) / 2 * n).astype(int), 0, n - 1) + face * n
    row = np.clip(((t + 1) / 2 * n).astype(int), 0, n - 1)
    return strip[row, col]


acc = lin(sample(load(os.path.join(D, KEY + ".png"))))
for g in GROUPS:
    acc += lin(sample(load(os.path.join(D, f"{KEY}_lights{g}.png"))))

exec(open("C:/StreetMR/tools/vegas/vegas_post.py").read().split("def load_exr")[0].split("args = ")[0])
import struct, zlib
data = (enc(acc) * 255 + 0.5).astype(np.uint8)
raw = b"".join(b"\x00" + data[r].tobytes() for r in range(H))
def chunk(tag, body):
    c = struct.pack(">I", len(body)) + tag + body
    return c + struct.pack(">I", zlib.crc32(tag + body) & 0xFFFFFFFF)
with open(OUT, "wb") as f:
    f.write(b"\x89PNG\r\n\x1a\n")
    f.write(chunk(b"IHDR", struct.pack(">IIBBBBB", W, H, 8, 2, 0, 0, 0)))
    f.write(chunk(b"IDAT", zlib.compress(raw, 6)))
    f.write(chunk(b"IEND", b""))
print("WROTE", OUT)
