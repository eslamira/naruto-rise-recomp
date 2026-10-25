#pragma once

// Pure image functions of the keyboard button icons (button_icons.cpp), kept
// free of the runtime so the host test can run them: a keycap drawn with a key
// label, and the DXT encoders that put it back in the game's texture formats
// (the inverse of the decoders in gpu/textures/formats.cpp).
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace narutorise::key_icon {

// RGBA8 texels, row after row.
struct Image {
  uint32_t width = 0, height = 0;
  std::vector<uint8_t> rgba;
};

// The look of an icon to replace, measured on the original (MeasureStyle):
// the rectangle its opaque texels cover and their mean colour, so the keycap
// sits where the button did and keeps its brightness.
struct Style {
  uint32_t x0 = 0, y0 = 0, x1 = 0, y1 = 0;  // Opaque bounds, x1/y1 exclusive.
  uint8_t fill[3] = {40, 40, 48};
  bool any = false;
};
Style MeasureStyle(const Image& original);

// A keycap with label drawn over a transparent image the size of original:
// a rounded rectangle in the style's bounds, light border, the label centred
// and shrunk to fit. font: a TrueType file (empty: no label is drawn).
Image RenderKeycap(const Image& original, std::string_view label, std::span<const uint8_t> font);

// A box-filtered half of an image (one mip level down; odd sizes round down, minimum 1).
Image Downscale(const Image& image);

// DXT blocks of an RGBA8 image, 4 x 4 texels each, row after row of blocks,
// in host byte order (as ReadGuestLevel returns them after the endian swap).
// DXT1 uses its transparent mode when a block has alpha below 128.
std::vector<uint8_t> EncodeDXT1(const Image& image);
std::vector<uint8_t> EncodeDXT5(const Image& image);

}  // namespace narutorise::key_icon
