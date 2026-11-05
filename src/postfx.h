// postfx.h
//   Shaders for MX Simulator without touching the game: the finished 3D image is copied (colour
//   and depth) right before the HUD is drawn over it, reworked by a chain of fullscreen passes of
//   our own, and written back. Everything the passes need about the scene comes from the GL calls
//   the hooks already see: the camera (the terrain's u_modelview), the projection (any perspective
//   matrix upload), the sun (u_light_position) and the moment the terrain pass ends (for a mask of
//   the ground). See postfx.cpp for the passes.
#pragma once

namespace mxcam {
namespace postfx {

// ---- What the hooks tell it ----------------------------------------------------------------

// This frame's camera: the game's view matrix and the one actually drawn with (after the helmet
// cam's correction; the same matrix when it is off). Column-major. Marks the frame as 3D.
void NoteView(const double raw[16], const double rendered[16]);

// A perspective projection the game uploaded or loaded (column-major). fromCamera: it went to
// the terrain program (preferred over any other).
void NoteProjection(const float m[16], bool fromCamera);

// The light direction the game sent to its shaders (before any change of ours), in the eye
// space of its own camera. components: 3, or 4 with w = 0 for a direction.
void NoteGameLight(const float* value, int components);

// Time of day: the light directions to upload instead of the game's (same layout as value),
// turning the game's own sun. Returns false to keep the game's.
bool OverrideLight(const float* value, int count, int components, float* out);

// Every skinned draw (riders, bikes) before it is made: its bones as uploaded (bind-pose centres,
// positions, rotations; v = R (p - c) + t) and the u_modelview it's drawn with (as the game sent
// it). A rigid draw of a skinned program comes as one bone at its origin. Your own rider is the
// one nearest the camera; the bones under it that keep turning are your bike's wheels.
void NoteSkeleton(unsigned program, const float* centres, const float* positions, const float* rotations, bool transposed,
                  int bones, const float modelview[16]);

// Every object the game draws in the 3D scene this frame (its u_modelview as sent, View * Model,
// column-major): one of them is your bike, which carries the headlight in any camera.
void NoteObject(unsigned program, const float viewModel[16]);

// The game has drawn the terrain (its program is still bound): the depth buffer now holds the
// ground and nothing else, which makes the ground mask for the dirt detail.
void TerrainPassDone();

// ---- Shadow maps: the game's own draw calls, repeated from the sun ----------------------------
// While the 3D scene is drawn, every object's draw call is drawn a second time into a depth
// texture seen from the sun (shadow map), with the same program and uniforms except its
// modelview and projection. The caller (proc_hooks.cpp) swaps those two uniforms around the
// repeat; postfx owns the sun's camera and the depth texture.

// Cheap: whether objects drawn now should cast shadows.
bool WantsShadowCasters();

// For one object: uploaded is the modelview the game's draw uses (as sent to the driver, after
// any correction), anchorEye an eye-space point the object is around (its skeleton; null: its
// origin). Gives the modelview and projection that draw it from the sun, or false when it is too
// far away to matter.
bool ShadowMatrices(const float uploaded[16], const float* anchorEye, float lightModelview[16], float lightProjection[16]);

// Around the repeated draw: draw into the shadow map (cleared on the first of the frame), then
// put the game's framebuffer, viewport and depth state back.
bool BeginShadowDraw();
void EndShadowDraw();

// Cheap: whether the game's lens flare (and its sun sprite) should be hidden: a time of day is
// set, so the game's sun is in the wrong place, and at night there's no sun to flare at all.
bool HidesGameFlare();

// "TIRES: ..." for the settings panel: whether your bike's wheels were found (empty: tire marks and
// tire dirt are off).
const char* TireStatusText();

// Cheap: whether draw calls should be checked for the start of the HUD.
bool WantsSceneEnd();

// The HUD is about to be drawn (why: for the log). Applies the effects now.
void SceneEnd(unsigned program, const char* why);

// Right before the buffer swap: applies the effects if SceneEnd didn't, then starts a new frame.
// apply = false only starts a new frame.
void FrameEnd(bool apply = true);

// One line for the settings panel.
const char* StatusText();

}  // namespace postfx
}  // namespace mxcam
