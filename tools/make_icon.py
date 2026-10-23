"""Draw the placeholder app icon (original art: helicopter over a desert sunset).

Writes res/icon.png (1024 px master) and res/desertstrikehd.ico (16-256 px).
Run: python tools/make_icon.py
"""
import math, os
from PIL import Image, ImageDraw, ImageFilter

ROOT = os.path.join(os.path.dirname(__file__), "..")
S = 1024


def lerp(a, b, t):
    return tuple(int(a[i] + (b[i] - a[i]) * t) for i in range(3))


def draw():
    img = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    # rounded-square tile with a sunset gradient
    bg = Image.new("RGBA", (S, S))
    top, mid, low = (40, 22, 70), (232, 96, 44), (255, 196, 92)
    px = bg.load()
    for y in range(S):
        t = y / S
        c = lerp(top, mid, t / 0.62) if t < 0.62 else lerp(mid, low, (t - 0.62) / 0.38)
        for x in range(S):
            px[x, y] = c + (255,)
    mask = Image.new("L", (S, S), 0)
    ImageDraw.Draw(mask).rounded_rectangle([24, 24, S - 24, S - 24], radius=200, fill=255)
    img.paste(bg, (0, 0), mask)
    d = ImageDraw.Draw(img)
    # sun
    d.ellipse([S * 0.30, S * 0.40, S * 0.70, S * 0.80], fill=(255, 226, 140, 255))
    # dunes
    dune = (92, 46, 30, 255)
    pts = [(24, S * 0.80)]
    for i in range(0, 41):
        x = 24 + (S - 48) * i / 40
        pts.append((x, S * 0.78 + math.sin(i / 40 * math.pi * 2.2 + 0.6) * S * 0.035))
    pts += [(S - 24, S - 24), (24, S - 24)]
    dl = Image.new("L", (S, S), 0)
    ImageDraw.Draw(dl).polygon(pts, fill=255)
    dl = Image.composite(dl, Image.new("L", (S, S), 0), mask)
    img.paste(Image.new("RGBA", (S, S), dune), (0, 0), dl)
    # helicopter silhouette (side view, nose right, slight pitch)
    sil = Image.new("L", (S, S), 0)
    h = ImageDraw.Draw(sil)
    cx, cy = S * 0.50, S * 0.47
    body = [(-200, 10), (-150, -40), (-20, -70), (120, -62), (200, -20), (225, 20),
            (190, 55), (60, 70), (-100, 60), (-160, 40)]
    tail = [(-170, -5), (-420, -40), (-430, -20), (-180, 35)]
    fin = [(-415, -40), (-460, -120), (-430, -122), (-395, -38)]
    canopy = [(110, -60), (195, -22), (150, -18), (95, -48)]
    rot = math.radians(-6)

    def tf(p):
        x, y = p
        return (cx + x * math.cos(rot) - y * math.sin(rot), cy + x * math.sin(rot) + y * math.cos(rot))

    for poly in (body, tail, fin):
        h.polygon([tf(p) for p in poly], fill=255)
    # rotor mast + blades
    h.polygon([tf(p) for p in [(-10, -70), (10, -70), (8, -110), (-8, -110)]], fill=255)
    h.polygon([tf(p) for p in [(-380, -118), (380, -108), (380, -96), (-380, -104)]], fill=255)
    # skids
    for x0, x1 in ((-120, 150),):
        h.polygon([tf(p) for p in [(x0, 98), (x1, 98), (x1 + 30, 86), (x1 + 34, 92), (x1 + 6, 110), (x0, 110)]], fill=255)
        for sx in (-60, 90):
            h.polygon([tf(p) for p in [(sx, 60), (sx + 12, 60), (sx + 12, 100), (sx, 100)]], fill=255)
    # stub wings with rocket pods
    h.polygon([tf(p) for p in [(-40, 20), (70, 20), (70, 40), (-40, 40)]], fill=255)
    h.ellipse([*tf((-30, 38)), *tf((60, 62))], fill=255)
    shadow = sil.filter(ImageFilter.GaussianBlur(10))
    img.paste(Image.new("RGBA", (S, S), (20, 8, 20, 140)), (14, 22), shadow)
    img.paste(Image.new("RGBA", (S, S), (24, 18, 30, 255)), (0, 0), sil)
    # canopy glint
    can = Image.new("L", (S, S), 0)
    ImageDraw.Draw(can).polygon([tf(p) for p in canopy], fill=255)
    img.paste(Image.new("RGBA", (S, S), (255, 170, 80, 255)), (0, 0), can)
    return img


def main():
    img = draw()
    os.makedirs(os.path.join(ROOT, "res"), exist_ok=True)
    img.save(os.path.join(ROOT, "res", "icon.png"))
    sizes = [(16, 16), (24, 24), (32, 32), (48, 48), (64, 64), (128, 128), (256, 256)]
    img.save(os.path.join(ROOT, "res", "desertstrikehd.ico"), sizes=sizes)
    img.resize((256, 256), Image.LANCZOS).save(os.path.join(ROOT, "res", "icon_256.png"))


if __name__ == "__main__":
    main()
