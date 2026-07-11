"""Generate original PNG graphics for the repo (no Blizzard assets).
Produces: assets/banner.png, assets/stats.png, assets/architecture.png
Run:  python scripts/make_assets.py
"""
import os
from PIL import Image, ImageDraw, ImageFont

OUT = os.path.join(os.path.dirname(__file__), "..", "assets")
os.makedirs(OUT, exist_ok=True)
F = "C:/Windows/Fonts/segoeui.ttf"
FB = "C:/Windows/Fonts/segoeuib.ttf"
FL = "C:/Windows/Fonts/segoeuil.ttf"


def font(path, size):
    return ImageFont.truetype(path, size)


# Midnight palette
BG0 = (14, 12, 34)      # deep indigo
BG1 = (46, 22, 74)      # violet
BG2 = (86, 30, 96)      # magenta-ish
GOLD = (245, 197, 92)
CYAN = (108, 224, 224)
TEXT = (238, 236, 250)
MUTE = (168, 162, 200)
CARD = (26, 22, 52)
CARD_BD = (74, 62, 120)


def vgrad(w, h, top, bot):
    img = Image.new("RGB", (w, h), top)
    px = img.load()
    for y in range(h):
        t = y / max(1, h - 1)
        r = int(top[0] + (bot[0] - top[0]) * t)
        g = int(top[1] + (bot[1] - top[1]) * t)
        b = int(top[2] + (bot[2] - top[2]) * t)
        for x in range(w):
            px[x, y] = (r, g, b)
    return img


def diag_glow(img, cx, cy, radius, color, strength=60):
    """Soft radial accent using a temp layer."""
    w, h = img.size
    glow = Image.new("RGB", (w, h), (0, 0, 0))
    d = ImageDraw.Draw(glow)
    steps = 24
    for i in range(steps, 0, -1):
        rr = int(radius * i / steps)
        a = int(strength * (i / steps) ** 2)
        d.ellipse([cx - rr, cy - rr, cx + rr, cy + rr], fill=(a, a, a))
    return Image.blend(img, Image.composite(Image.new("RGB", (w, h), color), img, glow.convert("L")), 0.55)


def chip(d, x, y, label, value, fv, fl):
    pad = 18
    vw = d.textlength(value, font=fv)
    lw = d.textlength(label, font=fl)
    w = int(max(vw, lw)) + pad * 2
    h = 66
    d.rounded_rectangle([x, y, x + w, y + h], radius=12, fill=CARD, outline=CARD_BD, width=1)
    d.text((x + pad, y + 8), value, font=fv, fill=GOLD)
    d.text((x + pad, y + 40), label, font=fl, fill=MUTE)
    return w


# ---------------- banner ----------------
W, H = 1280, 380
img = vgrad(W, H, BG0, BG1)
img = diag_glow(img, 1050, 80, 520, BG2, 70)
img = diag_glow(img, 120, 360, 420, (40, 30, 90), 60)
d = ImageDraw.Draw(img)
# top tag
d.text((64, 56), "TRINITYCORE  ·  RETAIL BUILD 12.0.7.68275", font=font(FB, 22), fill=CYAN)
# title
d.text((60, 92), "WoW Midnight Server", font=font(FB, 84), fill=TEXT)
d.text((64, 196), "Current-retail World of Warcraft private server — documented,",
       font=font(FL, 30), fill=MUTE)
d.text((64, 232), "honest, and community-fixed.", font=font(FL, 30), fill=MUTE)
# gold rule
d.rounded_rectangle([64, 286, 210, 292], radius=3, fill=GOLD)
# chips row
x = 64
for val, lab in [("48,257", "quests"), ("733,928", "spawns"),
                 ("517", "maps"), ("26", "races · allied unlocked")]:
    x += chip(d, x, 306, lab, val, font(FB, 30), font(F, 17)) + 16
img.save(os.path.join(OUT, "banner.png"))

# ---------------- stats ----------------
W, H = 1200, 520
img = vgrad(W, H, BG0, (24, 16, 48))
d = ImageDraw.Draw(img)
d.text((48, 40), "Content at a glance", font=font(FB, 44), fill=TEXT)
d.text((50, 100), "Verified counts from the live world database — no fabrication.",
       font=font(FL, 24), fill=MUTE)
cards = [
    ("48,257", "Quests", GOLD),
    ("61,101", "Quest objectives", CYAN),
    ("733,928", "Creature spawns", GOLD),
    ("227,684", "Creature templates", CYAN),
    ("197,724", "GameObject spawns", GOLD),
    ("517", "Maps populated", CYAN),
    ("3,084,867", "Loot table rows", GOLD),
    ("172,414", "Vendor rows", CYAN),
]
cw, ch, gap = 270, 150, 24
x0, y0 = 48, 170
for i, (val, lab, col) in enumerate(cards):
    cx = x0 + (i % 4) * (cw + gap)
    cy = y0 + (i // 4) * (ch + gap)
    d.rounded_rectangle([cx, cy, cx + cw, cy + ch], radius=16, fill=CARD, outline=CARD_BD, width=1)
    d.rounded_rectangle([cx, cy, cx + 6, cy + ch], radius=3, fill=col)
    d.text((cx + 24, cy + 30), val, font=font(FB, 46), fill=TEXT)
    d.text((cx + 26, cy + 96), lab, font=font(F, 22), fill=MUTE)
img.save(os.path.join(OUT, "stats.png"))

# ---------------- architecture ----------------
W, H = 1200, 460
img = vgrad(W, H, BG0, (20, 14, 44))
d = ImageDraw.Draw(img)
d.text((48, 36), "How it fits together", font=font(FB, 40), fill=TEXT)


def box(x, y, w, h, title, sub, col):
    d.rounded_rectangle([x, y, x + w, y + h], radius=16, fill=CARD, outline=col, width=2)
    d.text((x + 20, y + 20), title, font=font(FB, 26), fill=TEXT)
    yy = y + 58
    for line in sub:
        d.text((x + 20, yy), line, font=font(F, 18), fill=MUTE)
        yy += 26


def arrow(x1, y, x2):
    d.line([x1, y, x2, y], fill=GOLD, width=3)
    d.polygon([(x2, y), (x2 - 14, y - 8), (x2 - 14, y + 8)], fill=GOLD)


box(48, 150, 250, 170, "Your WoW client", ["build 12.0.7.68275", "patched via Arctium", "portal -> your server"], CYAN)
arrow(298, 235, 360)
box(360, 120, 250, 110, "bnetserver", ["Battle.net auth", "REST dev-cert :8081", "listen :1119"], GOLD)
box(360, 250, 250, 110, "worldserver", ["game world :8085", "GM console"], GOLD)
arrow(610, 235, 672)
box(672, 150, 250, 170, "MySQL", ["auth", "characters", "world  (+ our fixes)", "hotfixes"], CYAN)
box(972, 150, 180, 170, "Fix pack", ["quest chains", "GO spawns", "raid bindings", "graveyards"], (245, 197, 92))
d.line([922, 235, 972, 235], fill=CARD_BD, width=2)
img.save(os.path.join(OUT, "architecture.png"))

print("wrote:", os.listdir(OUT))
