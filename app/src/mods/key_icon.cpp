#include "mods/key_icon.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <string>

// stb_truetype from the SDK's Dear ImGui, a private copy (ImGui keeps its own static one).
#define STBTT_STATIC
#define STB_TRUETYPE_IMPLEMENTATION
#include "imstb_truetype.h"

namespace narutorise::key_icon {
namespace {

uint8_t At(const Image& image, uint32_t x, uint32_t y, uint32_t c) {
  return image.rgba[(size_t(y) * image.width + x) * 4 + c];
}

// --- DXT encoding ---
uint16_t To565(const int c[3]) {
  return uint16_t(((c[0] * 31 + 127) / 255) << 11 | ((c[1] * 63 + 127) / 255) << 5 | ((c[2] * 31 + 127) / 255));
}
void From565(uint16_t v, int c[3]) {
  const int r = (v >> 11) & 31, g = (v >> 5) & 63, b = v & 31;
  c[0] = (r << 3) | (r >> 2), c[1] = (g << 2) | (g >> 4), c[2] = (b << 3) | (b >> 2);
}
int Distance(const int a[3], const uint8_t* b) {
  const int dr = a[0] - b[0], dg = a[1] - b[1], db = a[2] - b[2];
  return dr * dr * 2 + dg * dg * 4 + db * db;
}
// One 4 x 4 block of texels (missing texels past the image repeat the edge).
void GatherBlock(const Image& image, uint32_t bx, uint32_t by, uint8_t texels[16][4]) {
  for (uint32_t y = 0; y < 4; ++y)
    for (uint32_t x = 0; x < 4; ++x) {
      const uint32_t sx = std::min(bx * 4 + x, image.width - 1), sy = std::min(by * 4 + y, image.height - 1);
      for (uint32_t c = 0; c < 4; ++c) texels[y * 4 + x][c] = At(image, sx, sy, c);
    }
}
// The colour part (8 bytes): endpoints from the bounding box of the visible
// texels, four-colour mode (c0 > c1), or three colours plus transparent.
void ColorBlock(const uint8_t texels[16][4], bool transparent_mode, uint8_t out[8]) {
  int lo[3] = {255, 255, 255}, hi[3] = {0, 0, 0};
  bool any = false;
  for (int i = 0; i < 16; ++i) {
    if (transparent_mode && texels[i][3] < 128) continue;
    any = true;
    for (int c = 0; c < 3; ++c) lo[c] = std::min<int>(lo[c], texels[i][c]), hi[c] = std::max<int>(hi[c], texels[i][c]);
  }
  if (!any) lo[0] = lo[1] = lo[2] = hi[0] = hi[1] = hi[2] = 0;
  uint16_t c0 = To565(hi), c1 = To565(lo);
  if (transparent_mode) {
    if (c0 > c1) std::swap(c0, c1);  // c0 <= c1: three colours and transparent.
  } else if (c0 < c1) {
    std::swap(c0, c1);
  } else if (c0 == c1) {
    // One colour: four-colour mode needs c0 > c1; every index 0 reads c0.
    // (c0 == 0, black, reads the same in the three-colour mode.)
    if (c0) c1 = uint16_t(c0 - 1);
    out[0] = uint8_t(c0), out[1] = uint8_t(c0 >> 8), out[2] = uint8_t(c1), out[3] = uint8_t(c1 >> 8);
    std::memset(out + 4, 0, 4);
    return;
  }
  int p[4][3];
  From565(c0, p[0]);
  From565(c1, p[1]);
  const bool four = c0 > c1;
  for (int c = 0; c < 3; ++c) {
    if (four) {
      p[2][c] = (2 * p[0][c] + p[1][c]) / 3;
      p[3][c] = (p[0][c] + 2 * p[1][c]) / 3;
    } else {
      p[2][c] = (p[0][c] + p[1][c]) / 2;
      p[3][c] = 0;
    }
  }
  uint32_t indices = 0;
  for (int i = 0; i < 16; ++i) {
    uint32_t best = 0;
    if (!four && texels[i][3] < 128) {
      best = 3;
    } else {
      int best_d = 1 << 30;
      for (uint32_t k = 0; k < (four ? 4u : 3u); ++k) {
        const int d = Distance(p[k], texels[i]);
        if (d < best_d) best_d = d, best = k;
      }
    }
    indices |= best << (2 * i);
  }
  out[0] = uint8_t(c0), out[1] = uint8_t(c0 >> 8), out[2] = uint8_t(c1), out[3] = uint8_t(c1 >> 8);
  std::memcpy(out + 4, &indices, 4);
}
// The DXT5 alpha part (8 bytes): eight-value mode between the block's extremes.
void AlphaBlock(const uint8_t texels[16][4], uint8_t out[8]) {
  int a0 = 0, a1 = 255;
  for (int i = 0; i < 16; ++i) a0 = std::max<int>(a0, texels[i][3]), a1 = std::min<int>(a1, texels[i][3]);
  if (a0 == a1) {
    out[0] = uint8_t(a0), out[1] = uint8_t(a1);
    std::memset(out + 2, 0, 6);
    return;
  }
  int palette[8] = {a0, a1};
  for (int k = 1; k <= 6; ++k) palette[k + 1] = ((7 - k) * a0 + k * a1) / 7;
  uint64_t bits = 0;
  for (int i = 0; i < 16; ++i) {
    int best = 0, best_d = 1 << 30;
    for (int k = 0; k < 8; ++k) {
      const int d = std::abs(palette[k] - texels[i][3]);
      if (d < best_d) best_d = d, best = k;
    }
    bits |= uint64_t(best) << (3 * i);
  }
  out[0] = uint8_t(a0), out[1] = uint8_t(a1);
  for (int b = 0; b < 6; ++b) out[2 + b] = uint8_t(bits >> (8 * b));
}

// --- Drawing ---
float Clamp01(float v) { return v < 0 ? 0 : v > 1 ? 1 : v; }
// Coverage of a rounded rectangle at a texel centre (anti-aliased over one texel).
float RoundedCoverage(float px, float py, float x0, float y0, float x1, float y1, float radius) {
  const float cx = std::clamp(px, x0 + radius, x1 - radius), cy = std::clamp(py, y0 + radius, y1 - radius);
  const float d = std::hypot(px - cx, py - cy) - radius;
  return Clamp01(0.5f - d);
}
void Blend(Image& image, uint32_t x, uint32_t y, const uint8_t rgb[3], float alpha) {
  if (alpha <= 0) return;
  uint8_t* t = &image.rgba[(size_t(y) * image.width + x) * 4];
  const float da = t[3] / 255.0f, out_a = alpha + da * (1 - alpha);
  for (int c = 0; c < 3; ++c)
    t[c] = uint8_t(std::lround((rgb[c] * alpha + t[c] * da * (1 - alpha)) / std::max(out_a, 1e-6f)));
  t[3] = uint8_t(std::lround(out_a * 255));
}
}  // namespace

Style MeasureStyle(const Image& original) {
  Style s;
  uint64_t sum[3] = {0, 0, 0}, n = 0;
  s.x0 = original.width, s.y0 = original.height;
  for (uint32_t y = 0; y < original.height; ++y)
    for (uint32_t x = 0; x < original.width; ++x) {
      if (At(original, x, y, 3) < 128) continue;
      s.any = true;
      s.x0 = std::min(s.x0, x), s.y0 = std::min(s.y0, y), s.x1 = std::max(s.x1, x + 1), s.y1 = std::max(s.y1, y + 1);
      for (int c = 0; c < 3; ++c) sum[c] += At(original, x, y, c);
      ++n;
    }
  if (!s.any) s.x0 = s.y0 = 0, s.x1 = original.width, s.y1 = original.height;
  if (n)
    for (int c = 0; c < 3; ++c) s.fill[c] = uint8_t(sum[c] / n);
  return s;
}

Image RenderKeycap(const Image& original, std::string_view label, std::span<const uint8_t> font) {
  Image out{original.width, original.height, std::vector<uint8_t>(size_t(original.width) * original.height * 4, 0)};
  const Style s = MeasureStyle(original);
  const float x0 = float(s.x0), y0 = float(s.y0), x1 = float(s.x1), y1 = float(s.y1);
  const float w = x1 - x0, h = y1 - y0;
  if (w < 2 || h < 2) return out;
  const float radius = std::min(w, h) * 0.22f, border = std::max(1.0f, std::min(w, h) * 0.07f);
  const uint8_t edge[3] = {235, 235, 240}, face[3] = {38, 38, 46}, ink[3] = {250, 250, 252};
  for (uint32_t y = s.y0; y < s.y1; ++y)
    for (uint32_t x = s.x0; x < s.x1; ++x) {
      const float px = x + 0.5f, py = y + 0.5f;
      const float outer = RoundedCoverage(px, py, x0, y0, x1, y1, radius);
      const float inner = RoundedCoverage(px, py, x0 + border, y0 + border, x1 - border, y1 - border,
                                          std::max(radius - border, 0.0f));
      Blend(out, x, y, edge, outer);
      Blend(out, x, y, face, inner);
    }
  if (font.empty() || label.empty()) return out;
  stbtt_fontinfo info;
  if (!stbtt_InitFont(&info, font.data(), stbtt_GetFontOffsetForIndex(font.data(), 0))) return out;
  // The largest size (up to 62% of the keycap's height) whose label fits 84% of its width.
  const float inner_w = (w - 2 * border) * 0.84f, max_h = (h - 2 * border) * 0.62f;
  auto measure = [&](float scale) {
    int width = 0, advance, lsb;
    for (size_t i = 0; i < label.size(); ++i) {
      stbtt_GetCodepointHMetrics(&info, uint8_t(label[i]), &advance, &lsb);
      width += advance;
      if (i + 1 < label.size()) width += stbtt_GetCodepointKernAdvance(&info, uint8_t(label[i]), uint8_t(label[i + 1]));
    }
    return width * scale;
  };
  int ascent, descent, gap;
  stbtt_GetFontVMetrics(&info, &ascent, &descent, &gap);
  float scale = stbtt_ScaleForPixelHeight(&info, max_h / 0.72f);  // Cap height is about 72% of the pixel height.
  if (const float lw = measure(scale); lw > inner_w) scale *= inner_w / lw;
  const float text_w = measure(scale);
  // Centre the capital height: ascent * 0.72 approximates it for Lato.
  const float cap = ascent * scale * 0.72f;
  float pen_x = x0 + (w - text_w) / 2;
  const float baseline = y0 + (h + cap) / 2;
  for (size_t i = 0; i < label.size(); ++i) {
    const int cp = uint8_t(label[i]);
    int gw, gh, gx, gy;
    const float shift_x = pen_x - std::floor(pen_x);
    unsigned char* bitmap =
        stbtt_GetCodepointBitmapSubpixel(&info, scale, scale, shift_x, 0, cp, &gw, &gh, &gx, &gy);
    const int ox = int(std::floor(pen_x)) + gx, oy = int(std::lround(baseline)) + gy;
    for (int y = 0; y < gh; ++y)
      for (int x = 0; x < gw; ++x) {
        const int tx = ox + x, ty = oy + y;
        if (tx < 0 || ty < 0 || tx >= int(out.width) || ty >= int(out.height)) continue;
        Blend(out, uint32_t(tx), uint32_t(ty), ink, bitmap[y * gw + x] / 255.0f);
      }
    stbtt_FreeBitmap(bitmap, nullptr);
    int advance, lsb;
    stbtt_GetCodepointHMetrics(&info, cp, &advance, &lsb);
    pen_x += advance * scale;
    if (i + 1 < label.size()) pen_x += stbtt_GetCodepointKernAdvance(&info, cp, uint8_t(label[i + 1])) * scale;
  }
  return out;
}

Image Downscale(const Image& image) {
  Image out{std::max(image.width / 2, 1u), std::max(image.height / 2, 1u), {}};
  out.rgba.resize(size_t(out.width) * out.height * 4);
  for (uint32_t y = 0; y < out.height; ++y)
    for (uint32_t x = 0; x < out.width; ++x)
      for (uint32_t c = 0; c < 4; ++c) {
        uint32_t sum = 0, n = 0;
        for (uint32_t dy = 0; dy < 2; ++dy)
          for (uint32_t dx = 0; dx < 2; ++dx) {
            const uint32_t sx = std::min(x * 2 + dx, image.width - 1), sy = std::min(y * 2 + dy, image.height - 1);
            sum += At(image, sx, sy, c), ++n;
          }
        out.rgba[(size_t(y) * out.width + x) * 4 + c] = uint8_t((sum + n / 2) / n);
      }
  return out;
}

std::vector<uint8_t> EncodeDXT1(const Image& image) {
  const uint32_t bw = (image.width + 3) / 4, bh = (image.height + 3) / 4;
  std::vector<uint8_t> out(size_t(bw) * bh * 8);
  uint8_t texels[16][4];
  for (uint32_t by = 0; by < bh; ++by)
    for (uint32_t bx = 0; bx < bw; ++bx) {
      GatherBlock(image, bx, by, texels);
      bool transparent = false;
      for (auto& t : texels) transparent |= t[3] < 128;
      ColorBlock(texels, transparent, &out[(size_t(by) * bw + bx) * 8]);
    }
  return out;
}

std::vector<uint8_t> EncodeDXT5(const Image& image) {
  const uint32_t bw = (image.width + 3) / 4, bh = (image.height + 3) / 4;
  std::vector<uint8_t> out(size_t(bw) * bh * 16);
  uint8_t texels[16][4];
  for (uint32_t by = 0; by < bh; ++by)
    for (uint32_t bx = 0; bx < bw; ++bx) {
      GatherBlock(image, bx, by, texels);
      uint8_t* block = &out[(size_t(by) * bw + bx) * 16];
      AlphaBlock(texels, block);
      ColorBlock(texels, false, block + 8);
    }
  return out;
}

}  // namespace narutorise::key_icon
