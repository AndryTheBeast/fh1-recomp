#!/usr/bin/env python3
"""Draws the port's icons (original artwork: no logo or art of the game is used) and writes them as .ico files.

    python tools/fh1_make_icon.py [--variant a|b|c] [--sheet PREVIEW.png]
    python tools/fh1_make_icon.py --image PICTURE.png [--sheet PREVIEW.png]

Writes fh1/res/fh1.ico (the game: FH1.exe) and installer/fh1_installer.ico (the same picture with a download
arrow in a corner), each with the sizes Windows asks for (16 to 256 pixels). --sheet also saves a picture of the
three variants side by side, large and small, to choose from. Needs Pillow.

--image: the icons are made from your own picture instead (cut to a square), and written as
fh1/res/fh1_local.ico and installer/fh1_installer_local.ico. Those two names are ignored by git and, when they
exist, the builds use them instead of the drawn icons: a picture that is not ours to publish (the game's cover,
for example) stays on this PC's builds and out of the repository.

Variants: a = a sun on the horizon above a road, sunset colors; b = the letters FH1 over a horizon line, dark;
c = the sun and the road in a round badge, night colors.
"""
import os
import sys

from PIL import Image, ImageDraw, ImageFilter, ImageFont

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
S = 1024  # drawn large, then reduced: smooth edges at every size
SIZES = [16, 20, 24, 32, 40, 48, 64, 128, 256]


def lerp(a, b, t):
    return tuple(int(round(a[i] + (b[i] - a[i]) * t)) for i in range(3))


def gradient(top, middle, bottom):
    im = Image.new('RGB', (S, S))
    px = im.load()
    for y in range(S):
        t = y / (S - 1)
        c = lerp(top, middle, t * 2) if t < 0.5 else lerp(middle, bottom, (t - 0.5) * 2)
        for x in range(S):
            px[x, y] = c
    return im


def mask_rounded(radius):
    m = Image.new('L', (S, S), 0)
    ImageDraw.Draw(m).rounded_rectangle([0, 0, S - 1, S - 1], radius=radius, fill=255)
    return m


def mask_circle():
    m = Image.new('L', (S, S), 0)
    ImageDraw.Draw(m).ellipse([8, 8, S - 9, S - 9], fill=255)
    return m


def road(draw, horizon, ground, line, half_top=26, half_bottom=330, dashes=5):
    """The ground below the horizon and a road running to its middle, with a broken center line."""
    draw.rectangle([0, horizon, S, S], fill=ground)
    cx = S // 2
    draw.polygon([(cx - half_top, horizon), (cx + half_top, horizon), (cx + half_bottom, S), (cx - half_bottom, S)],
                 fill=(34, 30, 44))
    # Dashes grow towards the viewer.
    y = horizon + 22
    for i in range(dashes):
        t0 = (y - horizon) / (S - horizon)
        length = 26 + 150 * t0
        t1 = min(1.0, (y + length - horizon) / (S - horizon))
        w0, w1 = 4 + 30 * t0, 4 + 30 * t1
        draw.polygon([(cx - w0, y), (cx + w0, y), (cx + w1, y + length), (cx - w1, y + length)], fill=line)
        y += length * 1.75


def sun(im, center, radius, color, glow):
    halo = Image.new('RGBA', (S, S), (0, 0, 0, 0))
    ImageDraw.Draw(halo).ellipse([center[0] - radius * 1.5, center[1] - radius * 1.5, center[0] + radius * 1.5,
                                  center[1] + radius * 1.5], fill=glow + (150,))
    halo = halo.filter(ImageFilter.GaussianBlur(radius * 0.35))
    im.alpha_composite(halo)
    ImageDraw.Draw(im).ellipse([center[0] - radius, center[1] - radius, center[0] + radius, center[1] + radius],
                               fill=color + (255,))


def variant_a():
    im = gradient((92, 28, 140), (236, 44, 122), (255, 168, 46)).convert('RGBA')
    horizon = 610
    sun(im, (S // 2, horizon - 10), 230, (255, 236, 150), (255, 200, 90))
    road(ImageDraw.Draw(im), horizon, (58, 24, 84), (255, 226, 120))
    im.putalpha(mask_rounded(190))
    return im


def variant_b():
    im = gradient((20, 22, 46), (30, 26, 60), (58, 22, 74)).convert('RGBA')
    d = ImageDraw.Draw(im)
    font = ImageFont.truetype(r'C:\Windows\Fonts\seguibl.ttf', 430)
    box = d.textbbox((0, 0), 'FH1', font=font)
    x = (S - (box[2] - box[0])) // 2 - box[0]
    y = 220 - box[1]
    # The letters take the sunset colors: a gradient cut out by the text.
    letters = Image.new('L', (S, S), 0)
    ImageDraw.Draw(letters).text((x, y), 'FH1', font=font, fill=255)
    im.paste(gradient((255, 196, 70), (255, 96, 96), (228, 40, 140)).convert('RGBA'), (0, 0), letters)
    d.rounded_rectangle([110, 720, S - 110, 752], radius=16, fill=(255, 190, 80, 255))
    d.rounded_rectangle([230, 800, S - 230, 822], radius=11, fill=(236, 60, 130, 255))
    im.putalpha(mask_rounded(190))
    return im


def variant_c():
    im = gradient((16, 28, 70), (40, 60, 130), (240, 96, 150)).convert('RGBA')
    horizon = 640
    sun(im, (S // 2, horizon), 200, (255, 214, 120), (255, 150, 120))
    road(ImageDraw.Draw(im), horizon, (14, 20, 46), (120, 230, 240), half_bottom=300)
    im.putalpha(mask_circle())
    ring = ImageDraw.Draw(im)
    ring.ellipse([8, 8, S - 9, S - 9], outline=(120, 230, 240, 255), width=34)
    return im


def with_arrow(im):
    """The installer's icon: a white disc with a download arrow in the bottom-right corner."""
    out = im.copy()
    d = ImageDraw.Draw(out)
    cx, cy, r = 770, 770, 230
    d.ellipse([cx - r, cy - r, cx + r, cy + r], fill=(255, 255, 255, 255), outline=(40, 30, 60, 255), width=22)
    color = (226, 44, 122, 255)
    d.rounded_rectangle([cx - 38, cy - 130, cx + 38, cy + 20], radius=14, fill=color)
    d.polygon([(cx - 118, cy - 6), (cx + 118, cy - 6), (cx, cy + 112)], fill=color)
    d.rounded_rectangle([cx - 120, cy + 126, cx + 120, cy + 162], radius=14, fill=color)
    return out


VARIANTS = {'a': variant_a, 'b': variant_b, 'c': variant_c}


def save_ico(im, path):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    # Each size is reduced from the large drawing by itself (Pillow would otherwise scale the 256 one down).
    frames = [im.resize((n, n), Image.LANCZOS) for n in SIZES]
    frames[-1].save(path, format='ICO', sizes=[(n, n) for n in SIZES], append_images=frames[:-1])
    print(path, os.path.getsize(path), 'bytes')


def sheet(path):
    cell = 300
    out = Image.new('RGBA', (cell * 3 + 40, cell * 2 + 250), (238, 238, 242, 255))
    d = ImageDraw.Draw(out)
    font = ImageFont.truetype(r'C:\Windows\Fonts\segoeui.ttf', 26)
    for i, key in enumerate('abc'):
        im = VARIANTS[key]()
        x = 20 + i * cell
        out.alpha_composite(im.resize((256, 256), Image.LANCZOS), (x + 22, 20))
        d.text((x + 22, 284), 'Variant ' + key.upper(), font=font, fill=(30, 30, 40))
        px = x + 22
        for n in (48, 32, 24, 16):  # the sizes of the desktop, the taskbar and file lists
            out.alpha_composite(im.resize((n, n), Image.LANCZOS), (px, 330))
            px += n + 14
        out.alpha_composite(with_arrow(im).resize((128, 128), Image.LANCZOS), (x + 22, 410))
        d.text((x + 160, 450), 'installer', font=font, fill=(90, 90, 100))
        px = x + 22
        for n in (48, 32, 16):
            out.alpha_composite(with_arrow(im).resize((n, n), Image.LANCZOS), (px, 560))
            px += n + 14
    # The same small sizes on a dark strip, as on a dark taskbar.
    d.rectangle([0, cell * 2 + 60, out.width, out.height], fill=(28, 28, 34, 255))
    for i, key in enumerate('abc'):
        im = VARIANTS[key]()
        px = 42 + i * cell
        for n in (64, 48, 32, 24, 16):
            out.alpha_composite(im.resize((n, n), Image.LANCZOS), (px, cell * 2 + 100))
            px += n + 14
    out.convert('RGB').save(path)
    print(path)


def from_picture(path):
    """The picture cut to its middle square, at the drawing size."""
    im = Image.open(path).convert('RGBA')
    side = min(im.size)
    left, top = (im.width - side) // 2, (im.height - side) // 2
    return im.crop((left, top, left + side, top + side)).resize((S, S), Image.LANCZOS)


def sheet_of(im, path):
    """One icon and its installer form, large and at the small sizes, on light and dark."""
    out = Image.new('RGBA', (760, 420), (238, 238, 242, 255))
    d = ImageDraw.Draw(out)
    d.rectangle([0, 300, out.width, out.height], fill=(28, 28, 34, 255))
    font = ImageFont.truetype(r'C:\Windows\Fonts\segoeui.ttf', 22)
    for column, (picture, label) in enumerate(((im, 'FH1.exe'), (with_arrow(im), 'FH1Installer.exe'))):
        x = 20 + column * 380
        out.alpha_composite(picture.resize((200, 200), Image.LANCZOS), (x, 20))
        d.text((x, 230), label, font=font, fill=(30, 30, 40))
        px = x + 214
        for n in (64, 48):
            out.alpha_composite(picture.resize((n, n), Image.LANCZOS), (px, 20 if n == 64 else 100))
        px = x
        for n in (64, 48, 32, 24, 16):
            out.alpha_composite(picture.resize((n, n), Image.LANCZOS), (px, 330))
            px += n + 14
    out.convert('RGB').save(path)
    print(path)


def main(argv):
    if '--image' in argv:
        im = from_picture(argv[argv.index('--image') + 1])
        if '--sheet' in argv:
            sheet_of(im, argv[argv.index('--sheet') + 1])
        save_ico(im, os.path.join(REPO, 'fh1', 'res', 'fh1_local.ico'))
        save_ico(with_arrow(im), os.path.join(REPO, 'installer', 'fh1_installer_local.ico'))
        return 0
    variant = 'a'
    if '--variant' in argv:
        variant = argv[argv.index('--variant') + 1].lower()
    if '--sheet' in argv:
        sheet(argv[argv.index('--sheet') + 1])
    im = VARIANTS[variant]()
    save_ico(im, os.path.join(REPO, 'fh1', 'res', 'fh1.ico'))
    save_ico(with_arrow(im), os.path.join(REPO, 'installer', 'fh1_installer.ico'))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
