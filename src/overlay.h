// overlay.h
//   In-game settings panel (Config::menuKey, E by default).
#pragma once

#include <windows.h>

namespace mxcam {

// Polls the menu key and, while the panel is open, handles its input and draws it into the
// current back buffer. Called by the swap hook right before the real buffer swap.
void OverlayFrame(HDC dc);

// True while a KEYBINDS row waits for its new key (the hotkeys stay quiet meanwhile).
bool OverlayListeningForKey();

// Hiding the game's HUD and cursor while riding (OTHER tab: Config::hideHud / hideCursor).
// They're hidden while the 3D scene is on screen (riding, or a replay; never the game's menus)
// and the mouse is left alone: moving or clicking it (or Esc) shows them again for a few seconds,
// so the game's menus over the race stay usable. cursor: asks about the cursor, else the HUD.
bool HideGameOverlayNow(bool cursor);

// The mouse in the game window's client area (pixels from its top left), false if unknown.
bool GameCursorPos(float& x, float& y);

}  // namespace mxcam
