"""Cut the FathersFallout logo out of its JPG (the "transparent" checkerboard is baked into the
pixels) and make the launcher's art: logo.png (transparent) and icon.ico (the Vault Boy circle).

    python -I make_art.py <FathersFallout.jpg> <out dir>
"""
import sys
import numpy as np
from PIL import Image
from scipy import ndimage

src, out = sys.argv[1], sys.argv[2]
img = np.asarray(Image.open(src).convert('RGB')).astype(np.float32)
h, w, _ = img.shape
lo, hi = img.min(axis=2), img.max(axis=2)
lum = img.mean(axis=2)

# Checkerboard squares are neutral light greys/whites. Logo colours (yellow, skin, blue) are not
# neutral; its whites (teeth, eye) are enclosed by the black outline.
light = (lo > 175) & (hi - lo < 28)
labels, n = ndimage.label(light)
background = np.zeros_like(light)
border = set(np.unique(np.concatenate([labels[0], labels[-1], labels[:, 0], labels[:, -1]]))) - {0}
for i in range(1, n + 1):
    comp = labels == i
    if i in border:
        background |= comp
        continue
    vals = lum[comp]
    if vals.size < 30:
        continue
    # Enclosed checkerboard (inside letters like 'a', 'e'): both shades of the pattern present.
    white, grey = (vals > 246).mean(), ((vals > 200) & (vals < 240)).mean()
    if white > 0.15 and grey > 0.15:
        background |= comp

alpha = np.where(background, 0.0, 1.0)
# Soften the fringe: outline pixels next to the background are part black, part checker-white.
near = ndimage.binary_dilation(background, iterations=2) & ~background
fringe = near & (hi - lo < 40) & (lum > 50)
a = np.clip((245.0 - lum) / (245.0 - 25.0), 0.0, 1.0)
alpha[fringe] = a[fringe]
rgb = img.copy()
rgb[fringe] = 20.0   # the outline's own colour, now carried by alpha

# Drop stray specks (the faded checker area under the text).
solid = alpha > 0.5
lab2, n2 = ndimage.label(solid)
sizes = ndimage.sum(solid, lab2, range(1, n2 + 1))
for i, s in enumerate(sizes, 1):
    if s < 400:
        alpha[lab2 == i] = 0.0

rgba = np.dstack([rgb, alpha * 255.0]).clip(0, 255).astype(np.uint8)
ys, xs = np.nonzero(rgba[:, :, 3] > 8)
pad = 6
box = (max(xs.min() - pad, 0), max(ys.min() - pad, 0), min(xs.max() + pad, w), min(ys.max() + pad, h))
logo = Image.fromarray(rgba, 'RGBA').crop(box)
logo.save(f'{out}/logo.png')
print('logo', logo.size, 'box', box)

# Icon: the round badge (above the wordmark). The wordmark is the lowest dense yellow band.
la = np.asarray(logo)
rows = (la[:, :, 3] > 128).sum(axis=1)
text_top = int(la.shape[0] * 0.72)   # wordmark sits in the bottom ~quarter
badge = logo.crop((0, 0, logo.size[0], text_top))
bx, by = badge.size
side = max(bx, by)
sq = Image.new('RGBA', (side, side), (0, 0, 0, 0))
sq.paste(badge, ((side - bx) // 2, (side - by) // 2))
sq.save(f'{out}/icon.ico', sizes=[(16, 16), (24, 24), (32, 32), (48, 48), (64, 64), (128, 128), (256, 256)])
sq.resize((256, 256), Image.LANCZOS).save(f'{out}/icon256.png')
print('icon from', badge.size)
