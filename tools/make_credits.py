"""Draws the home screen's credits line, "Ported by: AlvaroEPena" with GitHub's mark beside it, as the two images the
program draws over the home menu (games/bo2/src/bo2zm_credits.cpp): games/bo2/src/ui/credits/credits_720.png for screens up to
900 lines tall (handheld) and credits_1080.png for taller ones (docked), each drawn at one pixel per pixel.

usage: python -I tools/make_credits.py [--font PATH] [--out DIR] [--preview PATH]

- The mark is GitHub's ("Invertocat"), from the path of Octicons' mark-github icon (primer/octicons, MIT License),
  filled in white and not altered otherwise, as GitHub's logo guidelines ask (https://github.com/logos: the mark may
  be used to point to a GitHub profile; white on dark backgrounds).
- The text is set in Roboto Medium (Apache License 2.0), the copy Dear ImGui carries in misc/fonts (sdk/thirdparty/
  imgui, fetched by tools/fetch_thirdparty.py).
- Both have a soft dark shadow, so the line reads on the menu's dark backdrop and on its brighter parts alike.
Nothing here comes from the game. Needs Pillow.
"""
import argparse
import math
import os
import re
import sys

from PIL import Image, ImageChops, ImageDraw, ImageFilter, ImageFont

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TEXT = "Ported by: AlvaroEPena"
# Octicons mark-github, 16 x 16 units.
MARK = ("M8 0C3.58 0 0 3.58 0 8c0 3.54 2.29 6.53 5.47 7.59.4.07.55-.17.55-.38 0-.19-.01-.82-.01-1.49-2.01.37-2.53-.49"
        "-2.69-.94-.09-.23-.48-.94-.82-1.13-.28-.15-.68-.52-.01-.53.63-.01 1.08.58 1.23.82.72 1.21 1.87.87 2.33.66.07"
        "-.52.28-.87.51-1.07-1.78-.2-3.64-.89-3.64-3.95 0-.87.31-1.59.82-2.15-.08-.2-.36-1.02.08-2.12 0 0 .67-.21 2.2"
        ".82.64-.18 1.32-.27 2-.27.68 0 1.36.09 2 .27 1.53-1.04 2.2-.82 2.2-.82.44 1.1.16 1.92.08 2.12.51.56.82 1.27.82"
        " 2.15 0 3.07-1.87 3.75-3.65 3.95.29.25.54.73.54 1.48 0 1.07-.01 1.93-.01 2.2 0 .21.15.46.55.38A8.013 8.013 0"
        " 0016 8c0-4.42-3.58-8-8-8z")
# Sizes at 720 lines (the 1080 image is 1.5 times each): the mark's height, the text's size, the gap between them.
MARK_PX, TEXT_PX, GAP_PX = 22, 17, 8
SUPER = 16  # the mark is drawn this many times larger, then reduced (smooth edges)


def tokens(path):
    """The path's commands and numbers. Arc flags are single digits that may run into the next number ("0016")."""
    out = []
    for m in re.finditer(r"[A-Za-z]|-?(?:\d+\.?\d*|\.\d+)", path):
        out.append(m.group(0))
    return out


def flatten(path, steps=24):
    """The path as polygons (lists of points), curves and arcs cut into short straight pieces."""
    toks = tokens(path)
    polys, poly = [], []
    i = 0
    cmd = None
    x = y = 0.0
    start = (0.0, 0.0)

    def num():
        nonlocal i
        v = float(toks[i])
        i += 1
        return v

    def cubic(p0, p1, p2, p3):
        for k in range(1, steps + 1):
            t = k / steps
            a, b, c, d = (1 - t) ** 3, 3 * (1 - t) ** 2 * t, 3 * (1 - t) * t * t, t ** 3
            poly.append((a * p0[0] + b * p1[0] + c * p2[0] + d * p3[0], a * p0[1] + b * p1[1] + c * p2[1] + d * p3[1]))

    def arc(x1, y1, rx, ry, phi, large, sweep, x2, y2):
        # SVG's endpoint form to a centre and angles (SVG 1.1, appendix F.6.5).
        cp, sp = math.cos(math.radians(phi)), math.sin(math.radians(phi))
        dx, dy = (x1 - x2) / 2, (y1 - y2) / 2
        x1p, y1p = cp * dx + sp * dy, -sp * dx + cp * dy
        lam = (x1p / rx) ** 2 + (y1p / ry) ** 2
        if lam > 1:
            rx, ry = rx * math.sqrt(lam), ry * math.sqrt(lam)
        num_ = rx * rx * ry * ry - rx * rx * y1p * y1p - ry * ry * x1p * x1p
        coef = math.sqrt(max(0.0, num_ / (rx * rx * y1p * y1p + ry * ry * x1p * x1p)))
        if large == sweep:
            coef = -coef
        cxp, cyp = coef * rx * y1p / ry, -coef * ry * x1p / rx
        cx = cp * cxp - sp * cyp + (x1 + x2) / 2
        cy = sp * cxp + cp * cyp + (y1 + y2) / 2

        def angle(ux, uy, vx, vy):
            a = math.atan2(ux * vy - uy * vx, ux * vx + uy * vy)
            return a

        t1 = angle(1, 0, (x1p - cxp) / rx, (y1p - cyp) / ry)
        dt = angle((x1p - cxp) / rx, (y1p - cyp) / ry, (-x1p - cxp) / rx, (-y1p - cyp) / ry)
        if not sweep and dt > 0:
            dt -= 2 * math.pi
        elif sweep and dt < 0:
            dt += 2 * math.pi
        n = max(8, int(abs(dt) / (2 * math.pi) * 128))
        for k in range(1, n + 1):
            t = t1 + dt * k / n
            px, py = rx * math.cos(t), ry * math.sin(t)
            poly.append((cp * px - sp * py + cx, sp * px + cp * py + cy))

    while i < len(toks):
        if re.fullmatch(r"[A-Za-z]", toks[i]):
            cmd = toks[i]
            i += 1
            if cmd in "zZ":
                if poly:
                    polys.append(poly)
                poly = []
                x, y = start
                continue
        if cmd in "Mm":
            nx, ny = num(), num()
            if cmd == "m":
                nx, ny = x + nx, y + ny
            if poly:
                polys.append(poly)
            poly = [(nx, ny)]
            x, y = start = (nx, ny)
            cmd = "L" if cmd == "M" else "l"  # further pairs are lines
        elif cmd in "Ll":
            nx, ny = num(), num()
            if cmd == "l":
                nx, ny = x + nx, y + ny
            poly.append((nx, ny))
            x, y = nx, ny
        elif cmd in "Cc":
            pts = [num() for _ in range(6)]
            if cmd == "c":
                pts = [pts[k] + (x if k % 2 == 0 else y) for k in range(6)]
            cubic((x, y), (pts[0], pts[1]), (pts[2], pts[3]), (pts[4], pts[5]))
            x, y = pts[4], pts[5]
        elif cmd in "Aa":
            rx, ry, phi = num(), num(), num()
            # The two flags: one digit each, possibly glued to what follows.
            flags = []
            while len(flags) < 2:
                t = toks[i]
                flags.append(int(t[0]))
                if len(t) > 1:
                    toks[i] = t[1:]
                else:
                    i += 1
            nx, ny = num(), num()
            if cmd == "a":
                nx, ny = x + nx, y + ny
            arc(x, y, rx, ry, phi, flags[0], flags[1], nx, ny)
            x, y = nx, ny
        else:
            raise ValueError(f"path command {cmd} not handled")
    if poly:
        polys.append(poly)
    return polys


def mark_alpha(size):
    """The mark's coverage, size x size pixels."""
    big = Image.new("L", (size * SUPER, size * SUPER), 0)
    draw = ImageDraw.Draw(big)
    scale = size * SUPER / 16.0
    for poly in flatten(MARK):
        draw.polygon([(px * scale, py * scale) for px, py in poly], fill=255)
    return big.resize((size, size), Image.LANCZOS)


def credits_image(scale, font_path):
    mark_px = round(MARK_PX * scale)
    text_px = round(TEXT_PX * scale)
    gap = round(GAP_PX * scale)
    pad = max(2, round(3 * scale))  # room for the shadow
    font = ImageFont.truetype(font_path, text_px)
    left, top, right, bottom = font.getbbox(TEXT)
    ascent, descent = font.getmetrics()
    text_w = right - left
    height = max(mark_px, ascent + descent) + 2 * pad
    width = pad + mark_px + gap + text_w + 2 * pad
    alpha = Image.new("L", (width, height), 0)
    mark_y = (height - mark_px) // 2
    alpha.paste(mark_alpha(mark_px), (pad, mark_y))
    text = Image.new("L", (width, height), 0)
    # The text's capitals centred on the mark.
    cap_top, cap_bottom = font.getbbox("P")[1], font.getbbox("P")[3]
    text_y = round(height / 2 - (cap_top + cap_bottom) / 2)
    ImageDraw.Draw(text).text((pad + mark_px + gap - left, text_y), TEXT, font=font, fill=235)
    alpha = ImageChops.lighter(alpha, text)
    # A soft shadow one pixel down and right (1.5 at 1080).
    offset = max(1, round(scale))
    shadow = Image.new("L", (width, height), 0)
    shadow.paste(alpha, (offset, offset))
    shadow = shadow.filter(ImageFilter.GaussianBlur(1.2 * scale)).point(lambda v: min(255, int(v * 0.9)))
    # Straight alpha: the coverage is the content's or the shadow's, whichever is more; each pixel's color is the
    # content's share of it (white), the rest black.
    out = Image.new("RGBA", (width, height), (0, 0, 0, 0))
    a = ImageChops.lighter(alpha, shadow)
    px_a, px_c = a.load(), alpha.load()
    out_px = out.load()
    for yy in range(height):
        for xx in range(width):
            av = px_a[xx, yy]
            if av:
                c = min(255, round(255 * px_c[xx, yy] / av))
                out_px[xx, yy] = (c, c, c, av)
    return out


def main():
    p = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    p.add_argument("--font", default=os.path.join(ROOT, "sdk", "thirdparty", "imgui", "misc", "fonts",
                                                  "Roboto-Medium.ttf"))
    p.add_argument("--out", default=os.path.join(ROOT, "games", "bo2", "src", "ui", "credits"))
    p.add_argument("--preview", help="also write the two images over a grey backdrop to this PNG")
    args = p.parse_args()
    os.makedirs(args.out, exist_ok=True)
    images = []
    for lines, scale in ((720, 1.0), (1080, 1.5)):
        img = credits_image(scale, args.font)
        path = os.path.join(args.out, f"credits_{lines}.png")
        img.save(path, optimize=True)
        images.append(img)
        print(f"{path}: {img.size[0]} x {img.size[1]}")
    if args.preview:
        w = max(i.size[0] for i in images) + 40
        h = sum(i.size[1] for i in images) + 60
        back = Image.new("RGBA", (w, h), (70, 60, 55, 255))
        y = 20
        for img in images:
            back.alpha_composite(img, (20, y))
            y += img.size[1] + 20
        back.save(args.preview)
    return 0


if __name__ == "__main__":
    sys.exit(main())
