# Pet art prompts

Prompts for an image model (ChatGPT, Midjourney, Gemini…) to design the pet's species. One image per
species; save each as `tools/pet_art_src/<file>.png`, run `python3 tools/pet_art.py`, and the
creature replaces its first-draft drawing. `pet_art.py` crops the picture to its ink, scales it to the
stage's height on a 48×48 grid, turns it black and white and adds the faces (all 8 poses) at the
position in its `FACE` table, which gets tuned per picture.

Tips: generate 3–4 variants and pick the clearest silhouette; if the model adds a face, ask it to
"remove the eyes and mouth, keep everything else"; keep the outline thick.

## Shared style (already included in every prompt below)

> 1-bit black-and-white drawing of a single cute creature for a tiny e-paper virtual pet, in the spirit of the 1996 handheld pets but an original design. Pure black ink on a pure white background: no grey, no shading, no gradients, no dithering, no colour, no texture. Thick, even black outline (about 1/24 of the image width), white body, only a few solid black details. Chunky, rounded, friendly shapes with a strong simple silhouette that still reads when shrunk to 48×48 pixels: no thin lines, no small details. Front view, whole body visible, centred, standing on an invisible ground line, generous white margin. IMPORTANT: leave the face completely empty: no eyes, no mouth, no nose, no cheeks (expressions are added afterwards). No text, no shadow, no border, no background scenery. Square image.

## Egg → `tools/pet_art_src/egg.png`

*The egg everything hatches from.*

```
1-bit black-and-white drawing of a single cute creature for a tiny e-paper virtual pet, in the spirit of the 1996 handheld pets but an original design. Pure black ink on a pure white background: no grey, no shading, no gradients, no dithering, no colour, no texture. Thick, even black outline (about 1/24 of the image width), white body, only a few solid black details. Chunky, rounded, friendly shapes with a strong simple silhouette that still reads when shrunk to 48×48 pixels: no thin lines, no small details. Front view, whole body visible, centred, standing on an invisible ground line, generous white margin. IMPORTANT: leave the face completely empty: no eyes, no mouth, no nose, no cheeks (expressions are added afterwards). No text, no shadow, no border, no background scenery. Square image.

The creature: A tall oval egg, slightly pointed at the top. A bold zigzag band around its middle and four small plus-shaped speckles, two above the band and two below. (An egg has no face, so ignore the empty-face rule.)
```

## Dotlet → `tools/pet_art_src/baby.png`

*Baby. Hatches from the egg, lives about an hour.*

```
1-bit black-and-white drawing of a single cute creature for a tiny e-paper virtual pet, in the spirit of the 1996 handheld pets but an original design. Pure black ink on a pure white background: no grey, no shading, no gradients, no dithering, no colour, no texture. Thick, even black outline (about 1/24 of the image width), white body, only a few solid black details. Chunky, rounded, friendly shapes with a strong simple silhouette that still reads when shrunk to 48×48 pixels: no thin lines, no small details. Front view, whole body visible, centred, standing on an invisible ground line, generous white margin. IMPORTANT: leave the face completely empty: no eyes, no mouth, no nose, no cheeks (expressions are added afterwards). No text, no shadow, no border, no background scenery. Square image.

The creature: A tiny round blob like a drop of ink, a little wider than tall, soft and squishy, no arms or legs, a single small curl of hair on top. Very simple: the smallest and plainest of all.
```

## Puffle → `tools/pet_art_src/child.png`

*Child. Every Dotlet becomes one.*

```
1-bit black-and-white drawing of a single cute creature for a tiny e-paper virtual pet, in the spirit of the 1996 handheld pets but an original design. Pure black ink on a pure white background: no grey, no shading, no gradients, no dithering, no colour, no texture. Thick, even black outline (about 1/24 of the image width), white body, only a few solid black details. Chunky, rounded, friendly shapes with a strong simple silhouette that still reads when shrunk to 48×48 pixels: no thin lines, no small details. Front view, whole body visible, centred, standing on an invisible ground line, generous white margin. IMPORTANT: leave the face completely empty: no eyes, no mouth, no nose, no cheeks (expressions are added afterwards). No text, no shadow, no border, no background scenery. Square image.

The creature: A round marshmallow-shaped body, slightly wider at the bottom, with two short stubby feet and a small tuft on top of its head. Simple and soft, a bit bigger and more confident than the baby.
```

## Sprig → `tools/pet_art_src/teenA.png`

*Teen, well cared for.*

```
1-bit black-and-white drawing of a single cute creature for a tiny e-paper virtual pet, in the spirit of the 1996 handheld pets but an original design. Pure black ink on a pure white background: no grey, no shading, no gradients, no dithering, no colour, no texture. Thick, even black outline (about 1/24 of the image width), white body, only a few solid black details. Chunky, rounded, friendly shapes with a strong simple silhouette that still reads when shrunk to 48×48 pixels: no thin lines, no small details. Front view, whole body visible, centred, standing on an invisible ground line, generous white margin. IMPORTANT: leave the face completely empty: no eyes, no mouth, no nose, no cheeks (expressions are added afterwards). No text, no shadow, no border, no background scenery. Square image.

The creature: A round, upright, cheerful creature with a small plant sprout growing from the top of its head: a short stem with two round leaves. Two little legs and two tiny nub arms. Neat and balanced.
```

## Lumpkin → `tools/pet_art_src/teenB.png`

*Teen, poorly cared for.*

```
1-bit black-and-white drawing of a single cute creature for a tiny e-paper virtual pet, in the spirit of the 1996 handheld pets but an original design. Pure black ink on a pure white background: no grey, no shading, no gradients, no dithering, no colour, no texture. Thick, even black outline (about 1/24 of the image width), white body, only a few solid black details. Chunky, rounded, friendly shapes with a strong simple silhouette that still reads when shrunk to 48×48 pixels: no thin lines, no small details. Front view, whole body visible, centred, standing on an invisible ground line, generous white margin. IMPORTANT: leave the face completely empty: no eyes, no mouth, no nose, no cheeks (expressions are added afterwards). No text, no shadow, no border, no background scenery. Square image.

The creature: A lumpy, slightly lopsided round creature leaning a little to one side, with an extra bump on one side of its body and one bent antenna ending in a small solid black ball. A bit droopy but still cute.
```

## Lumo → `tools/pet_art_src/adult1.png`

*The best adult (perfect care).*

```
1-bit black-and-white drawing of a single cute creature for a tiny e-paper virtual pet, in the spirit of the 1996 handheld pets but an original design. Pure black ink on a pure white background: no grey, no shading, no gradients, no dithering, no colour, no texture. Thick, even black outline (about 1/24 of the image width), white body, only a few solid black details. Chunky, rounded, friendly shapes with a strong simple silhouette that still reads when shrunk to 48×48 pixels: no thin lines, no small details. Front view, whole body visible, centred, standing on an invisible ground line, generous white margin. IMPORTANT: leave the face completely empty: no eyes, no mouth, no nose, no cheeks (expressions are added afterwards). No text, no shadow, no border, no background scenery. Square image.

The creature: An elegant fox-cat creature: tall pointed ears, a round head merged with a rounded body, a big curled tail on one side, small paws. Graceful upright posture. The most refined design of the set.
```

## Bramble → `tools/pet_art_src/adult2.png`

*Adult, good care and almost perfect discipline.*

```
1-bit black-and-white drawing of a single cute creature for a tiny e-paper virtual pet, in the spirit of the 1996 handheld pets but an original design. Pure black ink on a pure white background: no grey, no shading, no gradients, no dithering, no colour, no texture. Thick, even black outline (about 1/24 of the image width), white body, only a few solid black details. Chunky, rounded, friendly shapes with a strong simple silhouette that still reads when shrunk to 48×48 pixels: no thin lines, no small details. Front view, whole body visible, centred, standing on an invisible ground line, generous white margin. IMPORTANT: leave the face completely empty: no eyes, no mouth, no nose, no cheeks (expressions are added afterwards). No text, no shadow, no border, no background scenery. Square image.

The creature: A sturdy, friendly bear-like creature: two round ears, a big round body, a small oval belly patch outlined in black, short thick legs. Calm and huggable.
```

## Masko → `tools/pet_art_src/adult3.png`

*Adult, good care but little discipline.*

```
1-bit black-and-white drawing of a single cute creature for a tiny e-paper virtual pet, in the spirit of the 1996 handheld pets but an original design. Pure black ink on a pure white background: no grey, no shading, no gradients, no dithering, no colour, no texture. Thick, even black outline (about 1/24 of the image width), white body, only a few solid black details. Chunky, rounded, friendly shapes with a strong simple silhouette that still reads when shrunk to 48×48 pixels: no thin lines, no small details. Front view, whole body visible, centred, standing on an invisible ground line, generous white margin. IMPORTANT: leave the face completely empty: no eyes, no mouth, no nose, no cheeks (expressions are added afterwards). No text, no shadow, no border, no background scenery. Square image.

The creature: A round bird-like creature with a crest of three pointed feathers on top and a solid black bandit-style mask band across where the eyes would be (leave the mask fully black; we add white eyes inside it). Small wings at its sides, short legs. Mischievous.
```

## Chompy → `tools/pet_art_src/adult4.png`

*Adult, careless but well disciplined. Wins the game more often.*

```
1-bit black-and-white drawing of a single cute creature for a tiny e-paper virtual pet, in the spirit of the 1996 handheld pets but an original design. Pure black ink on a pure white background: no grey, no shading, no gradients, no dithering, no colour, no texture. Thick, even black outline (about 1/24 of the image width), white body, only a few solid black details. Chunky, rounded, friendly shapes with a strong simple silhouette that still reads when shrunk to 48×48 pixels: no thin lines, no small details. Front view, whole body visible, centred, standing on an invisible ground line, generous white margin. IMPORTANT: leave the face completely empty: no eyes, no mouth, no nose, no cheeks (expressions are added afterwards). No text, no shadow, no border, no background scenery. Square image.

The creature: A wide, squat, round creature, much wider than tall, with very short feet. Its lower half is meant for an enormous grin (leave that area empty white; we draw the mouth). Jolly and greedy-looking.
```

## Wiggle → `tools/pet_art_src/adult5.png`

*Adult, careless and half disciplined.*

```
1-bit black-and-white drawing of a single cute creature for a tiny e-paper virtual pet, in the spirit of the 1996 handheld pets but an original design. Pure black ink on a pure white background: no grey, no shading, no gradients, no dithering, no colour, no texture. Thick, even black outline (about 1/24 of the image width), white body, only a few solid black details. Chunky, rounded, friendly shapes with a strong simple silhouette that still reads when shrunk to 48×48 pixels: no thin lines, no small details. Front view, whole body visible, centred, standing on an invisible ground line, generous white margin. IMPORTANT: leave the face completely empty: no eyes, no mouth, no nose, no cheeks (expressions are added afterwards). No text, no shadow, no border, no background scenery. Square image.

The creature: A worm or caterpillar standing up in a gentle S-curve made of three rounded segments, the head being the top segment, a small tail curl at the bottom. Wobbly and silly.
```

## Spike → `tools/pet_art_src/adult6.png`

*Adult, the worst care.*

```
1-bit black-and-white drawing of a single cute creature for a tiny e-paper virtual pet, in the spirit of the 1996 handheld pets but an original design. Pure black ink on a pure white background: no grey, no shading, no gradients, no dithering, no colour, no texture. Thick, even black outline (about 1/24 of the image width), white body, only a few solid black details. Chunky, rounded, friendly shapes with a strong simple silhouette that still reads when shrunk to 48×48 pixels: no thin lines, no small details. Front view, whole body visible, centred, standing on an invisible ground line, generous white margin. IMPORTANT: leave the face completely empty: no eyes, no mouth, no nose, no cheeks (expressions are added afterwards). No text, no shadow, no border, no background scenery. Square image.

The creature: A round creature covered all around with short triangular spikes, like a chestnut burr or a sea urchin, with two little feet underneath. Grumpy but cute.
```

## Sir Moss → `tools/pet_art_src/secret.png`

*Secret adult: a Masko raised without any discipline turns into him after about 4 days.*

```
1-bit black-and-white drawing of a single cute creature for a tiny e-paper virtual pet, in the spirit of the 1996 handheld pets but an original design. Pure black ink on a pure white background: no grey, no shading, no gradients, no dithering, no colour, no texture. Thick, even black outline (about 1/24 of the image width), white body, only a few solid black details. Chunky, rounded, friendly shapes with a strong simple silhouette that still reads when shrunk to 48×48 pixels: no thin lines, no small details. Front view, whole body visible, centred, standing on an invisible ground line, generous white margin. IMPORTANT: leave the face completely empty: no eyes, no mouth, no nose, no cheeks (expressions are added afterwards). No text, no shadow, no border, no background scenery. Square image.

The creature: An old gentleman creature: a round body, a tall black top hat, a big bushy black moustache in the lower middle of the face area (the moustache stays; leave eyes and mouth empty), and a small walking cane. Dignified and funny.
```

## Angel → `tools/pet_art_src/angel.png`

*Shown when a pet passes away.*

```
1-bit black-and-white drawing of a single cute creature for a tiny e-paper virtual pet, in the spirit of the 1996 handheld pets but an original design. Pure black ink on a pure white background: no grey, no shading, no gradients, no dithering, no colour, no texture. Thick, even black outline (about 1/24 of the image width), white body, only a few solid black details. Chunky, rounded, friendly shapes with a strong simple silhouette that still reads when shrunk to 48×48 pixels: no thin lines, no small details. Front view, whole body visible, centred, standing on an invisible ground line, generous white margin. IMPORTANT: leave the face completely empty: no eyes, no mouth, no nose, no cheeks (expressions are added afterwards). No text, no shadow, no border, no background scenery. Square image.

The creature: A small friendly ghost with a wavy bottom edge, two tiny wings at its sides and a thin halo floating above its head. Peaceful. Leave the face empty.
```
