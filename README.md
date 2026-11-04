# MXCam: MX Simulator Dynamic First Person Camera Mod

Open source and purely OpenGL based.

> [!WARNING]
> **Work in progress.** Expect bugs and changes between updates.

---

## How to install: watch the tutorial video

> [!IMPORTANT]
> **Watch this before installing.** It shows every step.
>
> ### [CLICK HERE FOR THE VIDEO TUTORIAL](https://www.youtube.com/watch?v=wJv2dMv2B9Y)

[![Video tutorial: how to install the MX Simulator camera mod](https://img.youtube.com/vi/wJv2dMv2B9Y/hqdefault.jpg)](https://www.youtube.com/watch?v=wJv2dMv2B9Y)

---

## Updating

If you already have the mod and want to update it:

1. **Save your settings first.** Take a screenshot or write down your mod settings, just in case.
2. Replace **both files** shown in the [tutorial video](https://www.youtube.com/watch?v=wJv2dMv2B9Y) (`opengl32.dll` and `mxcam.ini`) with the new ones.

---

## Shaders (free)

MXCam also includes a free shader pack, built the same way as the camera: pure OpenGL, nothing in the game's files or memory is changed. Open the panel (**E**) and look at the **LIGHT** and **IMAGE** tabs. **F7** turns all of it on and off, for before / after comparisons.

- **Time of day**: a slider from midnight to midnight. The sun moves across the sky and lights the bikes from where it is, sunsets turn the light and the sky orange, and at night the sky fills with stars and a moon lights the track (**Night brightness** sets how dark). The game's own lens flare is hidden while a time of day is set, since it would stay at the game's sun. **Game** keeps the game's own lighting.
- **Headlight**: a light on your bike's front plate for night riding. It turns with the bike and lights the track from the front wheel out, in first or third person.
- **Shadows**: real-time sun shadows from bikes, riders, trees and track objects (drawn from the sun, so even things off screen cast them), plus contact shadows from the track's own shape.
- **Ambient occlusion**: soft contact shadows in ruts, corners and under the bikes.
- **Dirt detail**: fine, high-resolution grain, pebbles and clods on the track surface up close, only on the ground.
- **Tire marks** and **tire dirt**: your tires dig ruts into the track behind you, and your tires get caked with mud.
- **Sun rays**, **bloom**, **depth of field** (auto-focus), **motion blur** (your own bike stays sharp).
- **Anti-aliasing** (FXAA), **sharpening**, **anisotropic and trilinear texture filtering**.
- **Brightness**, **contrast**, **saturation**, **vignette**.

Only want the shaders? Turn off **Camera mod** (VIEW tab, the switch at the top of the panel, or F8): the game's own cameras come back and the shaders keep working.

The HUD and menus are drawn after the effects, so they stay sharp and readable at night. If something looks wrong, set the effect to Off in the panel and send `mxcam.log` along with the bug report.

---

## Suggestions and bugs

Found a bug or have an idea? DM me on Discord: **voaks**
