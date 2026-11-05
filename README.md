# MXS Remastered: MX Simulator Dynamic First Person Camera Mod and Shaders upgrade for MX Simulator

Open source and purely OpenGL based.

**Discord:** [discord.gg/YvkUHCMFy6](https://discord.gg/YvkUHCMFy6)

> [!WARNING]
> **Work in progress.** Expect bugs and changes between updates.

---

## How to install

1. **Download `opengl32.dll`** from [`bin/x64/Release`](bin/x64/Release/opengl32.dll). It's the only file you need.
   - Only if you run an old **32-bit** MX Simulator: use the one in [`bin/Win32/Release`](bin/Win32/Release/opengl32.dll) instead.
2. **Open your MX Simulator folder**, the one with `mx.exe` in it. To find it, right-click your MX Simulator shortcut and pick **Open file location**.
3. **Put `opengl32.dll` in that folder**, next to `mx.exe`. Don't put it in Windows' own folders.
4. **Start the game** and get on a bike. Press **E** to open the menu.

You don't need `mxcam.ini`. The mod starts with its default settings and creates `mxcam.ini` in the same folder the first time you press **SAVE** in the menu.

### Uninstall

Delete `opengl32.dll` from your MX Simulator folder. To remove everything, also delete `mxcam.ini` and the `mxcam.log` / `mxcam_*.txt` files next to it.

### Not working?

- **The menu doesn't open with E:** make sure `opengl32.dll` sits right next to `mx.exe` and not in a subfolder. Check that an `mxcam.log` file appears there after you start the game. If it doesn't, the game isn't loading the mod.
- **Something else is wrong:** send `mxcam.log` from your MX Simulator folder with your bug report (see below).

---

## Updating

Download the new `opengl32.dll` and replace the old one in your MX Simulator folder. **Keep your `mxcam.ini`**: it holds your settings, keybinds and what the mod has learnt about your bike, and replacing it resets all of that.

---

## The menu

Press **E** in game to open the menu. It has four tabs: **FIRST PERSON**, **GRAPHICS**, **REPLAY** and **KEYBINDS**. Hover over (or select) any setting to see what it does. Changes apply straight away. Press **SAVE** to keep them for next time, or **DEFAULTS** to put everything back.

| Key | What it does |
|---|---|
| **E** | Opens and closes the menu |
| **F8** | Turns the first person mod on and off |
| **F7** | Turns the shaders on and off (for before / after comparisons) |

All three keys can be changed on the **KEYBINDS** tab: click a key, press the new one, then **SAVE**.

---

## First person camera

A dynamic first person view that rides with your bike: it leans, dips and turns like a rider's head would, without throwing you around. Everything is on the **FIRST PERSON** tab.

- **Two camera modes**: **GAME** improves the game's own first person camera. **CUSTOM** builds the view from your bike itself, with your own look-down angle (or **Keep game angle** to use the game's 1st Person Angle). After loading in with CUSTOM, ride a few metres so it can find your bike.
- **Stabilization**:
  - **Horizon lock**: how much of the bike's lean is taken out of the view, from leaning with the bike to a perfectly level horizon.
  - **Pitch lock**: how much of the nose up / down tilt (wheelies, jump faces) is taken out. Riding on flat ground looks the same at any setting.
  - **Steadiness**: keeps the view calm. The quick jolts of bumps, landings and the suspension diving under braking don't reach it, while it still eases into a corner's lean or a wheelie.
- **Head position**: move your eyes forward / back and up / down. **Hide torso & head** hides your own rider's body (arms and legs stay), so a high FOV doesn't show it from the inside.
- **Look to apex**: in corners your head turns toward the apex, more the tighter the corner, and eases back to straight ahead on the exit.
- **D-pad look**: on a controller, look left / right with the d-pad and behind with down. The look angle and turn speed are adjustable (CUSTOM camera: with its own eye position for each).
- **In the air**:
  - **Looseness**: the view keeps pointing forward while the bike swings left and right under it in scrubs and whips.
  - **Whip lock**: holds the view straight along your direction of travel, horizon level, while the bike swings, leans and tilts under you. It eases back onto the bike after landing.
- **Smoothing**: how quickly the view's rotation follows the bike (sharper or floatier), and how far it can fall behind.
- **Neck and impacts**: your head sinks on landings and big hits and springs back up, with a forward nod as the neck compresses. Stiffness, bounce and how far it sinks are all adjustable.

Turn off **First person mod** (or press **F8**) to get the game's own cameras back untouched.

---

## Replay cameras

New cameras for watching your replays, on the **REPLAY** tab. They work while the first person mod is on.

- **GAME**: the replay's own camera, as normal.
- **3RD PERSON**: the camera locked behind your rider. Set how far behind, how high and how far it looks down.
- **HELMET**: a GoPro-style camera mounted on your rider:
  - **Mounts**: **TOP** of the helmet, its **LEFT** or **RIGHT** side, or the **CHEST** (it turns with the body instead of the head). Each mount keeps its own height, forward position and tilt.
  - **Stabilization**: smooths the camera's rotation like a GoPro, from every jolt of the head to silky smooth.
  - **Horizon lock**: takes the rider's lean out of the view, up to a perfectly level horizon.

---

## Shaders

The mod also includes a free shader pack, built the same way as the camera: pure OpenGL, nothing in the game's files or memory is changed. Open the menu (**E**) and look at the **GRAPHICS** tab. **F7** turns all of it on and off, for before / after comparisons.

- **Time of day**: a slider from midnight to midnight. The sun moves across the sky and lights the bikes from where it is, sunsets turn the light and the sky orange, and at night the sky fills with stars and a moon lights the track (**Night brightness** sets how dark). The game's own lens flare is hidden while a time of day is set, since it would stay at the game's sun. **Game** keeps the game's own lighting.
- **Headlight**: a light on your bike's front plate for night riding. It turns with the bike and lights the track from the front wheel out, in first or third person.
- **Shadows**: real-time sun shadows from bikes, riders, trees and track objects (drawn from the sun, so even things off screen cast them), plus contact shadows from the track's own shape.
- **Ambient occlusion**: soft contact shadows in ruts, corners and under the bikes.
- **Dirt detail**: fine, high-resolution grain, pebbles and clods on the track surface up close, only on the ground.
- **Tire marks** and **tire dirt**: your tires dig ruts into the track behind you, and your tires get caked with mud.
- **Sun rays**, **bloom**, **depth of field** (auto-focus), **motion blur** (your own bike stays sharp).
- **Anti-aliasing** (FXAA), **sharpening**, **anisotropic and trilinear texture filtering**.
- **Brightness**, **contrast**, **saturation**, **vignette**.

Only want the shaders? Turn off **First person mod** (FIRST PERSON tab, or F8): the game's own cameras come back and the shaders keep working.

The HUD and menus are drawn after the effects, so they stay sharp and readable at night. If something looks wrong, set the effect to Off in the menu and send `mxcam.log` along with the bug report.

---

## Discord, suggestions and bugs

Join the Discord for updates, help and mod discussion: **[discord.gg/YvkUHCMFy6](https://discord.gg/YvkUHCMFy6)**

Found a bug or have an idea? Post it in the Discord or DM me: **voaks**. Please attach your `mxcam.log` with bug reports.
