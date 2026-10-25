#pragma once

// Keyboard button icons: the game's Xbox button icons (the HUD, menu legends,
// tutorial boards) are textures of its archive. With keyboard and
// mouse on, the textures whose content matches a known icon are rewritten in
// guest memory, at the last Unlock that fills them and before either renderer
// reads them, with the key bound to that button. The rewrite is in the guest's
// own format (tiled, endian-swapped, DXT re-encoded), so the native renderer
// and Xenos sample the same texels.
//
// texture_dump (diagnostics) writes every texture filled at an Unlock to
// texture_dump/<hash>_<width>x<height>_f<format>.png next to the executable,
// once per content: the way to find the icons' hashes.
#include <cstdint>

namespace narutorise::button_icons {
// At the last Unlock of a resource (8212DBE0), after the original ran.
void OnUnlock(uint8_t* base, uint32_t resource);
}  // namespace narutorise::button_icons
