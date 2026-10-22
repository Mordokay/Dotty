# Companion avatar prompts

Prompts for an image model (ChatGPT, Gemini, Midjourney…) to draw the companion's ten avatars. One
image per avatar; save each as `tools/avatar_art_src/<id>.png` (e.g. `girl.png`), run
`python3 tools/avatar_art.py`, and the avatar replaces its placeholder. `avatar_art.py` crops the
picture to its ink, scales it into a 96×96 square, turns it black and white, and draws the face
(eyes, eyebrows, mouth: 12 expressions) at the position in its `FACE` table, tuned per picture.

Tips: generate 3–4 variants and pick the clearest one; if the model draws a face anyway, ask it to
"remove the eyes, eyebrows and mouth, keep everything else"; keep the outline thick. The pictures are
shrunk to 96 px on a 1-bit screen, so fine detail disappears: big shapes, few lines.

## Shared style (already included in every prompt below)

> Black-and-white line drawing of a cute character portrait for a tiny 1-bit e-paper screen, in the style of modern Disney animation (like Tangled): soft rounded shapes, a big round head, gentle friendly proportions, charming and warm. Pure black ink on a pure white background: no grey, no shading, no gradients, no hatching, no colour. Clean, thick, even black outlines (about 1/40 of the image width), white fills, only a few solid black areas. Head and shoulders, facing the viewer, centred, with a generous white margin. The silhouette must still read when shrunk to 96×96 pixels: no thin lines, no tiny details. IMPORTANT: leave the face area completely empty, a plain white space in the middle of the head: no eyes, no eyebrows, no mouth (expressions are added afterwards; a small nose or snout is fine). No text, no border, no shadow, no background. Square image.

## Robot → `tools/avatar_art_src/robot.png`

```
Black-and-white line drawing of a cute character portrait for a tiny 1-bit e-paper screen, in the style of modern Disney animation (like Tangled): soft rounded shapes, a big round head, gentle friendly proportions, charming and warm. Pure black ink on a pure white background: no grey, no shading, no gradients, no hatching, no colour. Clean, thick, even black outlines (about 1/40 of the image width), white fills, only a few solid black areas. Head and shoulders, facing the viewer, centred, with a generous white margin. The silhouette must still read when shrunk to 96×96 pixels: no thin lines, no tiny details. IMPORTANT: leave the face area completely empty, a plain white space in the middle of the head: no eyes, no eyebrows, no mouth (expressions are added afterwards; a small nose or snout is fine). No text, no border, no shadow, no background. Square image.

The character: a friendly little robot with a rounded, slightly squarish head like a vintage TV, a short antenna topped with a small round ball, two round bolt "ears" on the sides, a few rivets, and rounded shoulders. The face is a smooth empty panel.
```

## Dog → `tools/avatar_art_src/dog.png`

```
Black-and-white line drawing of a cute character portrait for a tiny 1-bit e-paper screen, in the style of modern Disney animation (like Tangled): soft rounded shapes, a big round head, gentle friendly proportions, charming and warm. Pure black ink on a pure white background: no grey, no shading, no gradients, no hatching, no colour. Clean, thick, even black outlines (about 1/40 of the image width), white fills, only a few solid black areas. Head and shoulders, facing the viewer, centred, with a generous white margin. The silhouette must still read when shrunk to 96×96 pixels: no thin lines, no tiny details. IMPORTANT: leave the face area completely empty, a plain white space in the middle of the head: no eyes, no eyebrows, no mouth (expressions are added afterwards; a small nose or snout is fine). No text, no border, no shadow, no background. Square image.

The character: a puppy with big floppy ears filled solid black, a round fluffy head, a small round black nose low in the middle of the face, and a little collar with a round tag.
```

## Cat → `tools/avatar_art_src/cat.png`

```
Black-and-white line drawing of a cute character portrait for a tiny 1-bit e-paper screen, in the style of modern Disney animation (like Tangled): soft rounded shapes, a big round head, gentle friendly proportions, charming and warm. Pure black ink on a pure white background: no grey, no shading, no gradients, no hatching, no colour. Clean, thick, even black outlines (about 1/40 of the image width), white fills, only a few solid black areas. Head and shoulders, facing the viewer, centred, with a generous white margin. The silhouette must still read when shrunk to 96×96 pixels: no thin lines, no tiny details. IMPORTANT: leave the face area completely empty, a plain white space in the middle of the head: no eyes, no eyebrows, no mouth (expressions are added afterwards; a small nose or snout is fine). No text, no border, no shadow, no background. Square image.

The character: a kitten with two big pointed ears (a solid black triangle inside each), a round head with a fluffy cheek ruff, a tiny triangular nose, and two short whiskers on each side near the edges of the face.
```

## Fox → `tools/avatar_art_src/fox.png`

```
Black-and-white line drawing of a cute character portrait for a tiny 1-bit e-paper screen, in the style of modern Disney animation (like Tangled): soft rounded shapes, a big round head, gentle friendly proportions, charming and warm. Pure black ink on a pure white background: no grey, no shading, no gradients, no hatching, no colour. Clean, thick, even black outlines (about 1/40 of the image width), white fills, only a few solid black areas. Head and shoulders, facing the viewer, centred, with a generous white margin. The silhouette must still read when shrunk to 96×96 pixels: no thin lines, no tiny details. IMPORTANT: leave the face area completely empty, a plain white space in the middle of the head: no eyes, no eyebrows, no mouth (expressions are added afterwards; a small nose or snout is fine). No text, no border, no shadow, no background. Square image.

The character: a young fox with tall pointed ears with solid black tips, fluffy pointed cheek tufts on both sides, a small black nose, and a fluffy white chest ruff.
```

## Owl → `tools/avatar_art_src/owl.png`

```
Black-and-white line drawing of a cute character portrait for a tiny 1-bit e-paper screen, in the style of modern Disney animation (like Tangled): soft rounded shapes, a big round head, gentle friendly proportions, charming and warm. Pure black ink on a pure white background: no grey, no shading, no gradients, no hatching, no colour. Clean, thick, even black outlines (about 1/40 of the image width), white fills, only a few solid black areas. Head and shoulders, facing the viewer, centred, with a generous white margin. The silhouette must still read when shrunk to 96×96 pixels: no thin lines, no tiny details. IMPORTANT: leave the face area completely empty, a plain white space in the middle of the head: no eyes, no eyebrows, no mouth (expressions are added afterwards; a small nose or snout is fine). No text, no border, no shadow, no background. Square image.

The character: a round baby owl with two small feather tufts on top of its head, a heart-shaped face outline, a tiny pointed beak low in the middle of the face, folded wings at the sides and a few simple scalloped feather marks on its chest.
```

## Bear → `tools/avatar_art_src/bear.png`

```
Black-and-white line drawing of a cute character portrait for a tiny 1-bit e-paper screen, in the style of modern Disney animation (like Tangled): soft rounded shapes, a big round head, gentle friendly proportions, charming and warm. Pure black ink on a pure white background: no grey, no shading, no gradients, no hatching, no colour. Clean, thick, even black outlines (about 1/40 of the image width), white fills, only a few solid black areas. Head and shoulders, facing the viewer, centred, with a generous white margin. The silhouette must still read when shrunk to 96×96 pixels: no thin lines, no tiny details. IMPORTANT: leave the face area completely empty, a plain white space in the middle of the head: no eyes, no eyebrows, no mouth (expressions are added afterwards; a small nose or snout is fine). No text, no border, no shadow, no background. Square image.

The character: a teddy bear cub with two round ears (solid black inside), a big round head, a round muzzle low on the face with a small black nose, and a little scarf around its neck.
```

## Girl → `tools/avatar_art_src/girl.png`

```
Black-and-white line drawing of a cute character portrait for a tiny 1-bit e-paper screen, in the style of modern Disney animation (like Tangled): soft rounded shapes, a big round head, gentle friendly proportions, charming and warm. Pure black ink on a pure white background: no grey, no shading, no gradients, no hatching, no colour. Clean, thick, even black outlines (about 1/40 of the image width), white fills, only a few solid black areas. Head and shoulders, facing the viewer, centred, with a generous white margin. The silhouette must still read when shrunk to 96×96 pixels: no thin lines, no tiny details. IMPORTANT: leave the face area completely empty, a plain white space in the middle of the head: no eyes, no eyebrows, no mouth (expressions are added afterwards; a small nose or snout is fine). No text, no border, no shadow, no background. Square image.

The character: a young woman with long, flowing blonde hair that falls past her shoulders, with a soft side-swept fringe and a small flower tucked behind one ear. The hair is blonde, so draw it WHITE with outlines only and a few long flowing strand lines inside it (never filled black). A simple rounded neckline. A sweet, warm look.
```

## Guy → `tools/avatar_art_src/guy.png`

```
Black-and-white line drawing of a cute character portrait for a tiny 1-bit e-paper screen, in the style of modern Disney animation (like Tangled): soft rounded shapes, a big round head, gentle friendly proportions, charming and warm. Pure black ink on a pure white background: no grey, no shading, no gradients, no hatching, no colour. Clean, thick, even black outlines (about 1/40 of the image width), white fills, only a few solid black areas. Head and shoulders, facing the viewer, centred, with a generous white margin. The silhouette must still read when shrunk to 96×96 pixels: no thin lines, no tiny details. IMPORTANT: leave the face area completely empty, a plain white space in the middle of the head: no eyes, no eyebrows, no mouth (expressions are added afterwards; a small nose or snout is fine). No text, no border, no shadow, no background. Square image.

The character: a young man with short, tousled dark hair drawn solid black with one or two white highlight streaks, a confident friendly look, a light stubble-free chin, and the collar of a casual jacket over a simple shirt.
```

## Ghost → `tools/avatar_art_src/ghost.png`

```
Black-and-white line drawing of a cute character portrait for a tiny 1-bit e-paper screen, in the style of modern Disney animation (like Tangled): soft rounded shapes, a big round head, gentle friendly proportions, charming and warm. Pure black ink on a pure white background: no grey, no shading, no gradients, no hatching, no colour. Clean, thick, even black outlines (about 1/40 of the image width), white fills, only a few solid black areas. Head and shoulders, facing the viewer, centred, with a generous white margin. The silhouette must still read when shrunk to 96×96 pixels: no thin lines, no tiny details. IMPORTANT: leave the face area completely empty, a plain white space in the middle of the head: no eyes, no eyebrows, no mouth (expressions are added afterwards; a small nose or snout is fine). No text, no border, no shadow, no background. Square image.

The character: a friendly little ghost shaped like a soft rounded sheet, a dome-shaped head, two tiny stubby arms raised in a wave, and a wavy bottom edge. Not spooky at all: sweet and huggable.
```

## Dragon → `tools/avatar_art_src/dragon.png`

```
Black-and-white line drawing of a cute character portrait for a tiny 1-bit e-paper screen, in the style of modern Disney animation (like Tangled): soft rounded shapes, a big round head, gentle friendly proportions, charming and warm. Pure black ink on a pure white background: no grey, no shading, no gradients, no hatching, no colour. Clean, thick, even black outlines (about 1/40 of the image width), white fills, only a few solid black areas. Head and shoulders, facing the viewer, centred, with a generous white margin. The silhouette must still read when shrunk to 96×96 pixels: no thin lines, no tiny details. IMPORTANT: leave the face area completely empty, a plain white space in the middle of the head: no eyes, no eyebrows, no mouth (expressions are added afterwards; a small nose or snout is fine). No text, no border, no shadow, no background. Square image.

The character: a baby dragon with two short rounded horns, small frills on the sides of its head, a short rounded snout with two little nostril dots, and tiny wings peeking out behind its shoulders.
```
