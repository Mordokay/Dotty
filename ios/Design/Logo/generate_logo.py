"""Generates every Dotty logo SVG from one set of shapes, so all versions stay identical.

Everything, glow included, stays inside the 1024 × 1024 frame, so the mark is never clipped.

Run: python3 generate_logo.py  (needs `pip install shapely`)
"""
import math
import re
from shapely.geometry import Polygon, LineString, Point
from shapely.ops import unary_union
from shapely import affinity

# ---- shared geometry (1024 x 1024) -------------------------------------------------------
WING = "M508 372 C 400 360, 236 410, 228 580 C 222 740, 350 840, 462 830 C 494 827, 508 806, 508 770 Z"
WING_L = 'transform="rotate(12 508 380)"'
WING_R = 'transform="translate(1024 0) scale(-1 1) rotate(12 508 380)"'
SHIELD = ("M372 432 C 336 432, 330 398, 346 372 C 386 286, 638 286, 678 372 "
          "C 694 398, 688 432, 652 432 C 590 450, 434 450, 372 432 Z")
ABDOMEN = ("M512 400 C 610 400, 640 560, 632 700 C 626 800, 580 880, 512 880 "
           "C 444 880, 398 800, 392 700 C 384 560, 414 400, 512 400 Z")
ANT_L = "M482 286 C 462 216, 420 178, 366 166"
ANT_R = "M542 286 C 562 216, 604 178, 658 166"
HEAD = (512, 298, 62, 46)

BARK_DARK = "#3A2413"
STAR = "#F4EBD0"


def svg(body, defs=""):
    return (f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 1024 1024" width="1024" height="1024">'
            f'<defs>{defs}</defs>{body}</svg>\n')


# ---- full colour mark -------------------------------------------------------------------
MARK_DEFS = """
<radialGradient id="halo" cx="512" cy="700" r="310" gradientUnits="userSpaceOnUse">
  <stop offset="0" stop-color="#F3F27A" stop-opacity="0.6"/><stop offset="0.4" stop-color="#B8E04E" stop-opacity="0.2"/>
  <stop offset="1" stop-color="#7BC043" stop-opacity="0"/></radialGradient>
<linearGradient id="abdomen" x1="0" y1="420" x2="0" y2="880" gradientUnits="userSpaceOnUse">
  <stop offset="0" stop-color="#4A3A1C"/><stop offset="0.38" stop-color="#8D9A33"/>
  <stop offset="0.6" stop-color="#E8EE62"/><stop offset="1" stop-color="#FFFBD2"/></linearGradient>
<linearGradient id="wing" x1="0" y1="380" x2="0" y2="840" gradientUnits="userSpaceOnUse">
  <stop offset="0" stop-color="#7D5732"/><stop offset="0.6" stop-color="#5A3B21"/><stop offset="1" stop-color="#3A2413"/></linearGradient>
<radialGradient id="wingShade" cx="420" cy="470" r="420" gradientUnits="userSpaceOnUse">
  <stop offset="0" stop-color="#000" stop-opacity="0"/><stop offset="0.75" stop-color="#000" stop-opacity="0.05"/>
  <stop offset="1" stop-color="#000" stop-opacity="0.35"/></radialGradient>
<radialGradient id="shield" cx="470" cy="330" r="200" gradientUnits="userSpaceOnUse">
  <stop offset="0" stop-color="#F0C576"/><stop offset="0.65" stop-color="#D0913F"/><stop offset="1" stop-color="#A06628"/></radialGradient>
<filter id="rim" x="-200%" y="-20%" width="500%" height="140%"><feGaussianBlur stdDeviation="6"/></filter>
<filter id="soft" x="-20%" y="-20%" width="140%" height="140%"><feGaussianBlur stdDeviation="14"/></filter>
<filter id="bloom" x="-50%" y="-50%" width="200%" height="200%"><feGaussianBlur stdDeviation="28"/></filter>
"""


def wing_group(tf, shine_opacity):
    return f"""<g {tf}>
  <path d="{WING}" fill="#1E140A" opacity="0.45" filter="url(#soft)" transform="translate(10 8)"/>
  <path d="{WING}" fill="url(#wing)"/>
  <path d="{WING}" fill="url(#wingShade)"/>
  <path d="M318 462 C 350 424, 410 404, 462 402" fill="none" stroke="#FFF4DC" stroke-opacity="{shine_opacity}" stroke-width="24" stroke-linecap="round"/>
  <path d="M504 480 L 504 770" fill="none" stroke="#F6EE6A" stroke-opacity="0.55" stroke-width="22" stroke-linecap="round" filter="url(#rim)"/>
  <path d="M507 500 L 507 768" fill="none" stroke="#FBF5A8" stroke-opacity="0.7" stroke-width="4" stroke-linecap="round"/>
</g>"""


MARK_BODY = f"""
<circle cx="512" cy="700" r="310" fill="url(#halo)"/>
<path d="{ANT_L}" fill="none" stroke="{BARK_DARK}" stroke-width="22" stroke-linecap="round"/>
<path d="{ANT_R}" fill="none" stroke="{BARK_DARK}" stroke-width="22" stroke-linecap="round"/>
<ellipse cx="{HEAD[0]}" cy="{HEAD[1]}" rx="{HEAD[2]}" ry="{HEAD[3]}" fill="{BARK_DARK}"/>
<ellipse cx="512" cy="790" rx="120" ry="90" fill="#F6EE6A" opacity="0.8" filter="url(#bloom)"/>
<path d="{ABDOMEN}" fill="url(#abdomen)"/>
<g fill="none" stroke="#5E6A1E" stroke-opacity="0.45" stroke-width="7" stroke-linecap="round">
  <path d="M404 640 Q 512 668 620 640"/><path d="M410 718 Q 512 746 614 718"/><path d="M428 792 Q 512 816 596 792"/>
</g>
<ellipse cx="512" cy="830" rx="56" ry="28" fill="#FFFFF0" opacity="0.7" filter="url(#soft)"/>
{wing_group(WING_L, 0.45)}
{wing_group(WING_R, 0.2)}
<path d="{SHIELD}" fill="#1E140A" opacity="0.5" filter="url(#soft)" transform="translate(0 18)"/>
<path d="{SHIELD}" fill="url(#shield)"/>
<path d="M408 364 C 426 334, 460 318, 494 314" fill="none" stroke="#FFF4DC" stroke-opacity="0.6" stroke-width="16" stroke-linecap="round"/>
"""


# ---- the mark in three layers, so an app can recolour the firefly's light ------------------
# Stack them in this order: glow (tinted), body, rim (tinted). The glow and rim are drawn in
# greyscale, so multiplying them by a colour gives the firefly that colour of light while its
# wood-brown body stays as it is.
GLOW_DEFS = """
<radialGradient id="haloG" cx="512" cy="700" r="310" gradientUnits="userSpaceOnUse">
  <stop offset="0" stop-color="#FFFFFF" stop-opacity="0.6"/><stop offset="0.4" stop-color="#FFFFFF" stop-opacity="0.2"/>
  <stop offset="1" stop-color="#FFFFFF" stop-opacity="0"/></radialGradient>
<linearGradient id="abdomenG" x1="0" y1="420" x2="0" y2="880" gradientUnits="userSpaceOnUse">
  <stop offset="0" stop-color="#2E2E2E"/><stop offset="0.38" stop-color="#8A8A8A"/>
  <stop offset="0.6" stop-color="#E6E6E6"/><stop offset="1" stop-color="#FFFFFF"/></linearGradient>
<filter id="bloom" x="-50%" y="-50%" width="200%" height="200%"><feGaussianBlur stdDeviation="28"/></filter>
"""
GLOW_BODY = f"""
<circle cx="512" cy="700" r="310" fill="url(#haloG)"/>
<ellipse cx="512" cy="790" rx="120" ry="90" fill="#FFFFFF" opacity="0.8" filter="url(#bloom)"/>
<path d="{ABDOMEN}" fill="url(#abdomenG)"/>
<g fill="none" stroke="#6E6E6E" stroke-opacity="0.45" stroke-width="7" stroke-linecap="round">
  <path d="M404 640 Q 512 668 620 640"/><path d="M410 718 Q 512 746 614 718"/><path d="M428 792 Q 512 816 596 792"/>
</g>
"""


def wing_body(tf, shine_opacity):
    return f"""<g {tf}>
  <path d="{WING}" fill="#1E140A" opacity="0.45" filter="url(#soft)" transform="translate(10 8)"/>
  <path d="{WING}" fill="url(#wing)"/>
  <path d="{WING}" fill="url(#wingShade)"/>
  <path d="M318 462 C 350 424, 410 404, 462 402" fill="none" stroke="#FFF4DC" stroke-opacity="{shine_opacity}" stroke-width="24" stroke-linecap="round"/>
</g>"""


BODY_LAYER = f"""
<path d="{ANT_L}" fill="none" stroke="{BARK_DARK}" stroke-width="22" stroke-linecap="round"/>
<path d="{ANT_R}" fill="none" stroke="{BARK_DARK}" stroke-width="22" stroke-linecap="round"/>
<ellipse cx="{HEAD[0]}" cy="{HEAD[1]}" rx="{HEAD[2]}" ry="{HEAD[3]}" fill="{BARK_DARK}"/>
<ellipse cx="512" cy="830" rx="56" ry="28" fill="#FFFFFF" opacity="0.55" filter="url(#soft)"/>
{wing_body(WING_L, 0.45)}
{wing_body(WING_R, 0.2)}
<path d="{SHIELD}" fill="#1E140A" opacity="0.5" filter="url(#soft)" transform="translate(0 18)"/>
<path d="{SHIELD}" fill="url(#shield)"/>
<path d="M408 364 C 426 334, 460 318, 494 314" fill="none" stroke="#FFF4DC" stroke-opacity="0.6" stroke-width="16" stroke-linecap="round"/>
"""

RIM_LAYER = "".join(f"""<g {tf}>
  <path d="M504 480 L 504 770" fill="none" stroke="#FFFFFF" stroke-opacity="0.55" stroke-width="22" stroke-linecap="round" filter="url(#rim)"/>
  <path d="M507 500 L 507 768" fill="none" stroke="#FFFFFF" stroke-opacity="0.75" stroke-width="4" stroke-linecap="round"/>
</g>""" for tf in (WING_L, WING_R))

# ---- app icon: night sky, stars, round treeline -----------------------------------------
STARS = [(170, 190, 5, .9), (300, 110, 3, .6), (820, 150, 4, .8), (900, 300, 3, .5), (120, 420, 3, .5),
         (760, 90, 2.5, .5), (905, 520, 4, .7), (95, 640, 2.5, .4), (250, 300, 2, .4), (700, 240, 2, .35)]
TREES = [(-10, 930, 70), (70, 905, 60), (140, 940, 55), (200, 965, 50), (830, 960, 52),
         (890, 925, 62), (960, 900, 70), (1030, 935, 60)]
ICON_DEFS = MARK_DEFS + """
<linearGradient id="sky" x1="0" y1="0" x2="0" y2="1024" gradientUnits="userSpaceOnUse">
  <stop offset="0" stop-color="#0B1426"/><stop offset="0.6" stop-color="#0E1F22"/><stop offset="1" stop-color="#12291B"/></linearGradient>
"""
ICON_BODY = (
    '<rect width="1024" height="1024" fill="url(#sky)"/>'
    + "".join(f'<circle cx="{x}" cy="{y}" r="{r}" fill="{STAR}" opacity="{o}"/>' for x, y, r, o in STARS)
    + '<g fill="#081510" opacity="0.9">'
    + "".join(f'<circle cx="{x}" cy="{y}" r="{r}"/>' for x, y, r in TREES)
    + '<rect x="0" y="960" width="1024" height="64"/></g>'
    + f'<g transform="translate(512 548) scale(0.84) translate(-512 -540)">{MARK_BODY}</g>'
)

# ---- flat (solid colours, no effects) ---------------------------------------------------
FLAT_BODY = f"""
<path d="{ANT_L}" fill="none" stroke="{BARK_DARK}" stroke-width="22" stroke-linecap="round"/>
<path d="{ANT_R}" fill="none" stroke="{BARK_DARK}" stroke-width="22" stroke-linecap="round"/>
<ellipse cx="{HEAD[0]}" cy="{HEAD[1]}" rx="{HEAD[2]}" ry="{HEAD[3]}" fill="{BARK_DARK}"/>
<path d="{ABDOMEN}" fill="#EEF06A"/>
<path d="{WING}" fill="#5A3B21" {WING_L}/><path d="{WING}" fill="#5A3B21" {WING_R}/>
<path d="{SHIELD}" fill="#D0913F"/>
"""


# ---- one-colour glyph, computed so every corner is rounded ---------------------------------
def parse(d):
    """Absolute M/C/L/Z path -> list of points (cubics flattened)."""
    toks = re.findall(r"[MCLZ]|-?[\d.]+", d)
    pts, i, cmd = [], 0, None
    while i < len(toks):
        if toks[i].isalpha():
            cmd = toks[i]; i += 1
            if cmd == "Z":
                continue
        if cmd == "M" or cmd == "L":
            pts.append((float(toks[i]), float(toks[i + 1]))); i += 2
        elif cmd == "C":
            p0 = pts[-1]
            c = [float(t) for t in toks[i:i + 6]]; i += 6
            p1, p2, p3 = (c[0], c[1]), (c[2], c[3]), (c[4], c[5])
            for k in range(1, 25):
                t = k / 24
                mt = 1 - t
                pts.append(tuple(mt**3 * a + 3 * mt * mt * t * b + 3 * mt * t * t * cc + t**3 * e
                                 for a, b, cc, e in zip(p0, p1, p2, p3)))
    return pts


def wing_poly(mirror):
    p = affinity.rotate(Polygon(parse(WING)), 12, origin=(508, 380))
    return affinity.scale(p, -1, 1, origin=(512, 0)) if mirror else p


def stroke(d, w):
    return LineString(parse(d)).buffer(w / 2, quad_segs=16)


def rounded(g, r):
    """Round convex corners (opening) then concave ones (closing)."""
    return g.buffer(-r, quad_segs=16).buffer(r, quad_segs=16).buffer(r, quad_segs=16).buffer(-r, quad_segs=16)


GAP, R = 30, 12
head = affinity.scale(Point(HEAD[0], HEAD[1]).buffer(1, quad_segs=32), HEAD[2], HEAD[3])
abdomen = Polygon(parse(ABDOMEN))
wings = [wing_poly(False), wing_poly(True)]
shield = Polygon(parse(SHIELD))
antennae = unary_union([stroke(ANT_L, 22), stroke(ANT_R, 22)])

# Glow: only the tail below the wings' hinge, so it reads as a rounded drop, not a spike.
tail = abdomen.intersection(Polygon([(0, 600), (1024, 600), (1024, 1024), (0, 1024)]))
# Head and antennae are one piece; the shield's gap trims it.
crown = unary_union([head, antennae])

def cut(piece, above):
    return piece.difference(unary_union([a.buffer(GAP, quad_segs=16) for a in above]))

pieces = [
    rounded(cut(tail, wings), R),
    rounded(cut(crown, [shield]), 6),
    rounded(cut(wings[0], [shield]), R),
    rounded(cut(wings[1], [shield]), R),
    shield,
]
glyph = unary_union(pieces)


def to_path(geom):
    polys = getattr(geom, "geoms", [geom])
    out = []
    for p in polys:
        for ring in [p.exterior, *p.interiors]:
            c = list(ring.coords)
            out.append("M" + " L".join(f"{x:.1f} {y:.1f}" for x, y in c) + " Z")
    return " ".join(out)


GLYPH_BODY = f'<path d="{to_path(glyph)}" fill="{STAR}" fill-rule="evenodd"/>'

open("dotty-mark.svg", "w").write(svg(MARK_BODY, MARK_DEFS))
open("dotty-app-icon.svg", "w").write(svg(ICON_BODY, ICON_DEFS))
open("dotty-mark-flat.svg", "w").write(svg(FLAT_BODY))
open("dotty-mark-one-color-starlight.svg", "w").write(svg(GLYPH_BODY))
open("dotty-mark-one-color-heartwood.svg", "w").write(svg(GLYPH_BODY.replace(STAR, BARK_DARK)))
open("dotty-mark-layer-glow.svg", "w").write(svg(GLOW_BODY, GLOW_DEFS))
open("dotty-mark-layer-body.svg", "w").write(svg(BODY_LAYER, MARK_DEFS))
open("dotty-mark-layer-rim.svg", "w").write(svg(RIM_LAYER, MARK_DEFS))
print("ok")
