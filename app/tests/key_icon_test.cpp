// Host test of the keyboard button icons' image functions: the DXT encoders
// against the renderer's own decoders (formats.cpp), the guest write against
// the guest read, and the keycap drawing.
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

#include "gpu/textures/formats.h"
#include "mods/key_icon.h"

using namespace narutorise::key_icon;
namespace {
unsigned checks = 0;
void Check(bool value, const std::string& label) {
  ++checks;
  if (!value) throw std::runtime_error(label);
}

Image Gradient(uint32_t w, uint32_t h, bool alpha_edges) {
  Image image{w, h, std::vector<uint8_t>(size_t(w) * h * 4)};
  for (uint32_t y = 0; y < h; ++y)
    for (uint32_t x = 0; x < w; ++x) {
      uint8_t* t = &image.rgba[(size_t(y) * w + x) * 4];
      t[0] = uint8_t(x * 255 / std::max(w - 1, 1u));
      t[1] = uint8_t(y * 255 / std::max(h - 1, 1u));
      t[2] = 96;
      t[3] = alpha_edges && (x < 2 || y < 2) ? 0 : 255;
    }
  return image;
}

// Mean absolute error per channel after an encode and the renderer's decode.
double RoundTripError(const Image& image, bool dxt5, int channels) {
  auto blocks = dxt5 ? EncodeDXT5(image) : EncodeDXT1(image);
  const uint32_t bw = (image.width + 3) / 4, bh = (image.height + 3) / 4;
  Check(blocks.size() == size_t(bw) * bh * (dxt5 ? 16 : 8), "block count");
  gpu::ConvertLevel(dxt5 ? gpu::TexelConversion::kDXT5ToRGBA8 : gpu::TexelConversion::kDXT1ToRGBA8, blocks, bw, bh,
                    image.width, image.height);
  Check(blocks.size() == image.rgba.size(), "decoded size");
  double sum = 0;
  size_t n = 0;
  for (size_t i = 0; i < blocks.size(); i += 4)
    for (int c = 0; c < channels; ++c) sum += std::abs(int(blocks[i + c]) - int(image.rgba[i + c])), ++n;
  return sum / n;
}

void EncoderTests() {
  const Image smooth = Gradient(64, 32, false);
  Check(RoundTripError(smooth, true, 4) < 6, "DXT5 gradient error too large");
  Check(RoundTripError(smooth, false, 3) < 6, "DXT1 gradient error too large");

  // A flat colour comes back exactly (565-representable) or within rounding.
  Image flat{8, 8, {}};
  for (int i = 0; i < 64; ++i) flat.rgba.insert(flat.rgba.end(), {248, 0, 0, 255});
  Check(RoundTripError(flat, true, 4) < 1, "DXT5 flat colour");
  Check(RoundTripError(flat, false, 4) < 1, "DXT1 flat colour");

  // DXT1 transparency: texels under 128 alpha decode transparent, the rest opaque.
  const Image holes = Gradient(16, 16, true);
  auto blocks = EncodeDXT1(holes);
  gpu::ConvertLevel(gpu::TexelConversion::kDXT1ToRGBA8, blocks, 4, 4, 16, 16);
  for (size_t i = 0; i < blocks.size(); i += 4)
    Check((blocks[i + 3] == 0) == (holes.rgba[i + 3] < 128), "DXT1 transparency");

  // DXT5 alpha keeps hard edges.
  blocks = EncodeDXT5(holes);
  gpu::ConvertLevel(gpu::TexelConversion::kDXT5ToRGBA8, blocks, 4, 4, 16, 16);
  for (size_t i = 0; i < blocks.size(); i += 4) Check(blocks[i + 3] == holes.rgba[i + 3], "DXT5 alpha edge");

  // Sizes that are not multiples of 4: the partial blocks repeat the edge, and
  // the decode crops back to the size.
  Image odd{6, 10, {}};
  for (int i = 0; i < 60; ++i) odd.rgba.insert(odd.rgba.end(), {0, 132, 255, 200});
  Check(RoundTripError(odd, true, 4) < 2, "DXT5 odd size");
  Check(RoundTripError(odd, false, 3) < 2, "DXT1 odd size");
}

void GuestWriteTests() {
  for (bool tiled : {false, true})
    for (uint32_t endian : {0u, 1u, 2u, 3u})
      for (uint32_t bpb : {8u, 16u}) {
        gpu::GuestLevel level;
        level.width_blocks = 40, level.height_blocks = 36, level.bytes_per_block = bpb;
        level.pitch_blocks = 64, level.row_pitch_bytes = 64 * bpb;
        level.tiled = tiled, level.endian = endian;
        level.region_bytes = 64 * 64 * bpb;
        std::vector<uint8_t> memory(level.region_bytes, 0xEE), blocks(size_t(40) * 36 * bpb);
        for (size_t i = 0; i < blocks.size(); ++i) blocks[i] = uint8_t(i * 7 + 3);
        Check(gpu::WriteGuestLevel(level, blocks, memory.data()), "guest write");
        level.memory = memory.data();
        std::vector<uint8_t> read;
        Check(gpu::ReadGuestLevel(level, read), "guest read back");
        Check(read == blocks, "write is the inverse of read (tiled " + std::to_string(tiled) + ", endian " +
                                  std::to_string(endian) + ", bpb " + std::to_string(bpb) + ")");
        level.region_bytes = 16;
        std::vector<uint8_t> untouched(memory);
        Check(!gpu::WriteGuestLevel(level, blocks, memory.data()) && memory == untouched,
              "out-of-range write must change nothing");
      }
}

void KeycapTests(const std::vector<uint8_t>& font) {
  // An icon: a disc in a 64 x 64 transparent texture.
  Image icon{64, 64, std::vector<uint8_t>(64 * 64 * 4, 0)};
  for (uint32_t y = 0; y < 64; ++y)
    for (uint32_t x = 0; x < 64; ++x)
      if ((x - 32.0) * (x - 32.0) + (y - 32.0) * (y - 32.0) < 24 * 24) {
        uint8_t* t = &icon.rgba[(y * 64 + x) * 4];
        t[0] = 20, t[1] = 60, t[2] = 200, t[3] = 255;
      }
  const Style style = MeasureStyle(icon);
  Check(style.any && style.x0 == 9 && style.y0 == 9 && style.x1 == 56 && style.y1 == 56, "opaque bounds");
  const Image cap = RenderKeycap(icon, "Space", font);
  Check(cap.width == 64 && cap.height == 64, "keycap size");
  auto alpha = [&](uint32_t x, uint32_t y) { return cap.rgba[(y * 64 + x) * 4 + 3]; };
  Check(alpha(2, 2) == 0 && alpha(62, 62) == 0, "outside the bounds stays transparent");
  Check(alpha(32, 12) == 255, "keycap is opaque inside");
  if (!font.empty()) {
    // The label: bright texels in the middle band of the face.
    unsigned bright = 0;
    for (uint32_t y = 24; y < 40; ++y)
      for (uint32_t x = 14; x < 50; ++x)
        if (cap.rgba[(y * 64 + x) * 4] > 200) ++bright;
    Check(bright > 20, "label drawn");
  }
  const Image half = Downscale(cap);
  Check(half.width == 32 && half.height == 32 && half.rgba.size() == 32 * 32 * 4, "downscale size");
  Check(Downscale(Image{1, 1, {1, 2, 3, 4}}).rgba == std::vector<uint8_t>{1, 2, 3, 4}, "downscale 1x1");
}
}  // namespace

int main(int argc, char** argv) {
  try {
    std::vector<uint8_t> font;
    if (argc > 1) {
      std::ifstream f(argv[1], std::ios::binary);
      font.assign(std::istreambuf_iterator<char>(f), {});
      if (font.empty()) throw std::runtime_error(std::string("cannot read font ") + argv[1]);
    }
    EncoderTests();
    GuestWriteTests();
    KeycapTests(font);
    std::cout << "key icon PASS checks=" << checks << (font.empty() ? " (no font: label not checked)" : "") << '\n';
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "key icon FAIL after " << checks << " checks: " << error.what() << '\n';
    return 1;
  }
}
