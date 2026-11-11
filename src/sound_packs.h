// sound_packs.h
//   Engine sound packs (OTHER tab, SOUND MODS). MX Simulator plays a bike's engine from a
//   soundtable of recorded patches; the stock ones are a single engine cycle each, looped, and sound
//   bland. Each pack (made by tools/engine_sounds/make_engine_sounds.py from the stock recordings)
//   replaces one stock engine's soundtable with patches of 16 cycles, each a little different. The
//   packs ship in the MXS_REMASTERED_sounds folder next to opengl32.dll; installing one copies it into the
//   game folder as a .saf archive (the game reads those when it starts), removing it deletes it.
//   When the game folder can't be written to without administrator rights (Program Files), the pack
//   goes in the game's personal folder (%LOCALAPPDATA%\MX Simulator) instead, which it also reads.
//   Only files: nothing of the game's own is changed, in memory or on disk.
#pragma once

namespace mxcam {
namespace soundpacks {

constexpr int kCount = 5;  // 125, 250, 250F, 450F, and the 450F's clear one (one 450 or the other, not both)

// The stock engine a pack replaces (its soundtable's name), and what it is.
const char* Engine(int i);
const char* Label(int i);

// At startup: which packs are installed, refreshing any whose shipped copy has changed (a new
// version of the mod), and removing any that couldn't be removed while the game had it open.
void Startup();

bool Installed(int i);

// Installs or removes pack i. Returns the line for the panel's status (what happened, and that it's
// heard from the next time the game starts).
const char* Set(int i, bool on);

}  // namespace soundpacks
}  // namespace mxcam
