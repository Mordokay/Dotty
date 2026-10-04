# Squishy soft bodies — notes from squishy-studio

Source: https://squishy-studio-chi.vercel.app (studied 2026-10-02). Custom WebGL2, no libraries.

## Physics (position-based soft body)
- A shape is one closed ring of 32–60 outline points (usually 40), spaced evenly along the rest outline. There is no interior mesh.
- Fixed step of 120 Hz with 4 substeps (h = 1/480 s) and at most 6 steps per frame. Rendering interpolates between the last two states.
- Integration: semi-implicit Euler (`px = x; v += g*h; x += v*h`), then `v = (x - px)/h`.
- Constraints, once each per substep:
  1. **Shape matching:** best-fit rotation `theta = atan2(Σcross, Σdot)` about the centroid, with `goal_i = c + S·R(theta)·rest_i`. `S = [[1+exx, exy],[exy, 1+eyy]]` is a stretch matrix.
  2. **Edge lengths** toward the goal edges: `edgeK` 26000, compliance `1/(k·h²)`.
  3. **Area (pressure):** pushes points along their neighbours' perpendicular. `areaK` 5200, stiffening up to 16× once area loss passes 12%.
  4. **Pull to goal:** `x += (goal - x)·1/(1 + 1/(shapeK·h²))`, with `shapeK` 950.
- **Damping:** velocity splits into whole-body motion and wobble. Wobble decays by `exp(-2.6·h)`, air drag is 0.12, spin damping 0.35.
- **"Boing":** `exx`, `eyy` and `exy` form a damped spring (k 330, damping 7.5: about 2.9 Hz, damping ratio about 0.2), limited to 0.5 stretch and 0.42 squash. Squeezes and pokes kick it:
  `evxx += -s*ax*ax + b*ay*ay; evyy += -s*ay*ay + b*ax*ax; evxy += -(s + b)*ax*ay`, where b = bulge × s.
- **Dents:** the rest outline is pushed inward along its normals by `0.2·scale·depth·(1 - d²/r²)²`. Dents recover with a smoothstep (press 0.9 s, poke 0.35 s hold then 2.6 s).

## Interaction
- **Press:** a dent. After 150 ms the squeeze builds over 0.75 s (cubic ease-out), compressing along the finger-to-centre axis and cutting the target area by up to 10%.
- **Poke** (release under 190 ms): an inward velocity kick of 260 on points in range.
- **Long-press release:** a rebound kick of `-squeeze·2.5` and a hop.
- **Grab:** drag past 6–10 px. Each point gets its own spring, `farK + (nearK - farK)·v²` (520 to 52000), falling off with distance from the finger. On release, 0.35 of the finger's velocity is added (fling).
- **Sleep** after 0.5 s of calm.

## Drawing
- **Outline:** subdivide n points to 2n with `(-p0 + 9p1 + 9p2 - p3)/16`.
- **Mesh:** a fan of 10 rings with a "puff" height of `0.62·r·sqrt(1-(1-t)²)`, growing when squashed (`sqrt(restArea/area)`, limited to 0.72–1.45).
- **Shading:** wrap-around diffuse, darker inside dents, two speculars (`pow(nh,140)·0.8 + pow(nh,16)·0.1`), a softbox reflection, a rim glow `pow(edge,3.2)`, and a soft shadow (7 px offset, 13 px blur).

## Porting to SwiftUI
- **Physics:** about 300 lines of SIMD2<Float> maths, cheap on the CPU (about 20k point updates per second per body).
- **Rendering:** Canvas + TimelineView for flat or glossy fills. The puffy shading needs Metal or a `.layerEffect` working from a blurred alpha height.
- **Running cost:** animate only bodies that are touched or still moving, keep each canvas the size of its control plus bleed room, and respect Reduce Motion.
