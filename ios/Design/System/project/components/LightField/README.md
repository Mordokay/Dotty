# LightField

The base of every screen: large soft light sources drifting in the void, blending where they meet, with small motes drifting nearer. The light sources sit at `depth-far` and the motes at `depth-back`, so they sway against each other when the phone tilts (in this preview, when the pointer moves). Put the screen's content inside as children; it sits at `depth-ui` and stays put.

- `lights`: two to four light colours. Use the bracelet's colour first; a screen about a partner adds their colour.
- `motes` (default 14): fewer on busy screens. Each mote is a small firefly with its own size and pace, wandering on an irregular path, blinking, drifting slowly through the bracelet colours and (in the app) leaving a fading trail.
- `seed` fixes the layout so a screen doesn't jump between renders.
- With Reduce Motion the lights hold still.
- In SwiftUI: `LightField(lights:)`, drawn with `Canvas` in plus-lighter blending, offset per layer by the shared parallax camera.
