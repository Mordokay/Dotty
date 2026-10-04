# Dotty

Dotty is the iPhone companion app for Dotty, the e-paper desk companion. Its look comes from a design language first sketched for a light-and-touch wearable, which these docs still use as their example device ("the bracelet"): it talks in two ways, lighting up in colour and vibrating, and the app talks the same way. The firefly mark keeps that origin. Every screen is made of light, colour, glass and touch, layered in depth, against a deep dark. There is no scenery: no moon, no trees, no lanterns. The interface itself is what glows.

## Principles

1. **Light is the material.** Interfaces are built from light sources (soft coloured glows that blend where they meet) and glass that lets that light through. Light means something is alive: the bracelet, a touch, the one thing to do next.
2. **Colour is the bracelet's.** A bracelet can show any RGB colour, and when a person picks one, the app's light takes that colour too. The seven light colours are the interface's own palette and the quick presets; they never limit what a bracelet can show.
3. **Every light has a feel.** The bracelet vibrates when it glows, so the phone does too. Each light moment pairs with a haptic from the haptic tokens, and a received touch plays its own rhythm on the phone.
4. **Layers, not scenes.** Screens are stacks of layers at different depths. Scrolling and tilting the phone move each layer by its depth (parallax), so the interface feels deep without drawing a world.
5. **Rounded everything.** No corners and no tips. Anything you tap is a pill or a circle; glass uses `radius-card`; nothing goes below `radius-soft`.

## Colour

| Role | Tokens |
| --- | --- |
| The dark | `void`, and `void-raised` for solid dark surfaces |
| Light colours | `light-firefly` (default), `light-leaf`, `light-lagoon`, `light-dusk`, `light-bloom`, `light-ember`, `light-amber`: the palette and presets; any RGB colour can stand in for them |
| Glass | `glass`, `glass-strong`, `glass-edge` (the lit rim), `glass-veil` (keeps text readable) |
| Text | `ink`, `ink-muted`, `ink-faint`; `on-light` on lit fills |
| Signals | success is `light-leaf`, information `light-lagoon`, waiting `light-amber`, error `light-ember` |

Text on glass stays readable over any colour because every glass surface carries `glass-veil`: `ink` is 15:1 and `ink-muted` 7.6:1. On a lit fill, `on-light` is at least 6:1 on every light colour.

## Light and blending

- A **light source** has a bright core, a halo (`opacity-halo`) and an aura (`opacity-aura`) fading into the void. Lights add up where they overlap (screen or plus-lighter blending), so two lights make a brighter, mixed colour, like real light.
- **Glass** blurs what is behind it (`blur-glass`), takes a little of its colour, and catches light on its top rim (`glass-edge`). Glass never has a hard border. It is the static form of jelly glass (see Coming next).
- A **lit element** (a primary button, an on toggle, the selected colour) is filled with its light colour and casts a glow (`glow-sm`, `glow-md`, `glow-lg`) in that same colour.
- Shadows are rare and soft (`depth`): in the dark, things are separated by light, not by shadow.

## Depth and parallax

Every element sits at a depth: `depth-far` for the big light sources, `depth-back` for drifting motes, `depth-ui` for the interface, `depth-near` for things floating above it. Scrolling moves a layer by its depth; tilting the phone moves it by `(1 - depth) × tilt-reach`, so far layers sway the most and the interface stays put. The gyroscope's neutral pose follows how the person holds the phone, so only tilting moves things.

## Haptics

| Moment | Haptic |
| --- | --- |
| Press a button or row | `haptic-tap` |
| Choose a colour, flip a toggle, step a slider | `haptic-select` |
| Paired, saved, sent | `haptic-confirm` |
| Something failed | `haptic-error` |
| A touch arrives | `haptic-touch`: the touch's own rhythm |
| A pulse the person is waiting on | `haptic-pulse` at each peak |

## Type

- **Quicksand** (display family) sets titles: `display-xl`, `display-l`, `title`. It matches the round firefly and the `dotty` wordmark, and the app bundles it (OFL licence).
- **SF Pro Rounded** (text family) sets everything else. In SwiftUI this is the system font with `.fontDesign(.rounded)`, so Dynamic Type and SF Symbols come for free.

## Motion

| Motion | Duration | Easing | Used for |
| --- | --- | --- | --- |
| Pulse | `duration-pulse` (2.4 s) | `ease-breathe` | Lights breathing: loader, status dots, the bracelet orb |
| Float | `duration-float` (4.8 s) | `ease-breathe` | The firefly drifting |
| Drift | `duration-drift` (14 s) | `ease-breathe` | A LightField's light sources wandering |
| Settle | `duration-settle` (0.42 s) | `ease-settle` | Toggles, selection, sheets |
| Tap | `duration-tap` (0.16 s) | linear | Press feedback |

Loading is always the `FireflyLoader`. Respect Reduce Motion: lights hold still and pulses become a steady glow.

### Coming next

- **Jelly glass.** The material Dotty's controls are heading toward: Apple's Liquid Glass, reshaped by a soft-body simulation and lit from inside like a salt lamp. Pressed, it dents and light pours in where the finger is; poked, it wobbles; dragged, it stretches; it always settles back. Like Himalayan salt or optical fibre, the body stays see-through while it glows: the light pools near its source, escapes where the body is thin (its edges), and sheds only a small, soft halo. Each release plays a haptic as strong as the squash. A first version lives in the app's jelly lab.
- **Fluids.** Light that flows and mixes like liquid colour.
- **Light fibres.** Strands that carry light along their length and let it out at their tips and bends, for decorative and progress elements.
- **The flying firefly.** The logo firefly lifting off and flying with flapping wings.

## Iconography

Line icons on a 24px grid, 2px stroke, round caps and joins, drawn in `ink` (or `on-light` on lit fills). The `Icon` component holds the set; in the app, prefer SF Symbols in their rounded weights where one fits.

## Logo

The firefly mark: a round beetle seen from above, wings slightly open, its tail glowing between them. Use it from the Logos group: `dotty-mark.svg` (full colour), `dotty-mark-flat.svg` (below 48px), the two one-colour marks (tab bars, watermarks) and `dotty-app-icon.svg`. The wordmark is `dotty` in lowercase Quicksand Bold, in `ink`. Don't recolour the full-colour mark; keep clear space of half its width.

## Voice

Short, warm and plain. Talk about the bracelet and the person, not the technology: "Your bracelet is glowing. Double-tap it to say it's yours", not "Awaiting OOBE confirmation". Errors say what happened and what to do next, without apology.

## Components

- **Light:** `LightField` (every screen's base), `LightOrb`, `FireflyLoader`, `Logo`.
- **Surfaces:** `Glass`, `ListRow`.
- **Actions:** `Button` (light, glass, quiet), `IconButton`.
- **Controls:** `Toggle`, `Slider`, `ColorPicker`.
- **Status:** `StatusPill`.
- **Icons:** `Icon`.

Every lit component takes a `light` colour (any light token, or the bracelet's own RGB), so it can glow in the bracelet's colour.

## In the app

The SwiftUI code mirrors this system in `Dotty/DesignSystem/`: the same token names as `Color`, `Font` and constant extensions (`light-lagoon` → `.lightLagoon`), and a SwiftUI view per component with the same name. Parallax uses a shared camera (scroll offset and gyroscope tilt) that every layer reads its depth from.
