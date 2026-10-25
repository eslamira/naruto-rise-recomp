#include "mods/button_icons.h"

#include <fmt/format.h>
#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/logging.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <string>
#include <unordered_set>
#include <vector>

#include "gpu/common/guest_memory.h"
#include "gpu/common/hash.h"
#include "gpu/textures/formats.h"
#include "gpu/textures/layout.h"
#include "mods/key_icon.h"
#include "mods/key_prompts.h"

REXCVAR_DEFINE_BOOL(texture_dump, false, "narutorise/Diagnostics",
                    "Write each texture the game fills to texture_dump/ next to the executable (PNG, once "
                    "per content). For finding textures to replace.");

namespace narutorise::button_icons {
namespace {
using gpu::Get32;

// The asset formats with an RGBA decode (formats.cpp): k_8_8_8_8, k_DXT1, k_DXT4_5.
constexpr uint32_t kRGBA8 = 6, kDXT1 = 18, kDXT5 = 20;
bool Covered(uint32_t format) { return format == kRGBA8 || format == kDXT1 || format == kDXT5; }
// The icons are at most 116 texels on a side (the HUD's X); larger textures are not read.
constexpr uint32_t kMaxIconSide = 128;

enum class Input { kA, kB, kX, kY, kLB, kRB, kLT, kRT, kLeftStick, kRightStick };
struct KnownIcon {
  uint64_t hash;  // FastHash64 of the base level's guest blocks (ReadBase), as texture_dump names it.
  Input input;
};
// The Xbox button icons of the English assets (texture_dump of the menus,
// Story Mode and the fights; the same hashes on both renderers).
constexpr KnownIcon kIcons[] = {
    {0x4DEAC7421C3BCDD2, Input::kA},  {0x558389D1999D6FF1, Input::kA},  {0x7382358FBF17A96E, Input::kA},
    {0x9279A5283ECFDE1E, Input::kA},  {0x9B84584AD2284466, Input::kA},  {0x635BB38BD25EF592, Input::kA},
    {0xF9EC05EF34E7E88A, Input::kA},  // The tutorial boards.
    {0x3F869ECF4BB82F4B, Input::kB},  {0x5313002F71713B5A, Input::kB},  {0x98984C0505DBCC9E, Input::kB},
    {0xD8C092932C833410, Input::kB},  {0x037CF194E3F82360, Input::kB},
    {0x8A9806E4A28D5BD6, Input::kX},  {0xB7BA55CDEDE20824, Input::kX},  {0xE8028E3E2DF5CE1E, Input::kX},
    {0x06FA4B935E5BC64D, Input::kX},  {0x750D29EEC18B0126, Input::kX},  // 116 x 116: the HUD's Talk.
    {0x04C321BD35B2F0FA, Input::kY},  {0x138FC56EA71279EE, Input::kY},  {0xACAF630778789575, Input::kY},
    {0x6AEA192797993403, Input::kY},  {0x7A458F7CA2C3D031, Input::kY},
    {0x768D8446B271CCFF, Input::kLB}, {0x315B7D0193FEA085, Input::kLB}, {0x4C880BF358B09D93, Input::kLB},
    {0xF494E6368259F557, Input::kRB}, {0x53E2E0F3AB4B1890, Input::kRB},
    {0x44DAACACECBA60BB, Input::kLT}, {0xA605CF6D274BBEBB, Input::kLT},
    {0x85E0BB80F3A0213E, Input::kRT}, {0x2F58DD4457670E69, Input::kRT},
    {0xB10AB606E529FB94, Input::kLeftStick},  {0x293B355323C8242E, Input::kLeftStick},
    {0xFF97112CB9D38A01, Input::kLeftStick},  {0x594F7E0361A23DAD, Input::kLeftStick},
    {0x3830CD644B86C11A, Input::kRightStick}, {0xFFB89F94546960D9, Input::kRightStick},
};

// --- The labels, read once (a rebind applies on the next start, like the texts) ---
std::string QueryKey(const char* option) {
  return key_prompts::FirstKey(rex::cvar::Query<std::string>(option));
}
std::string StickLabel(const char* up, const char* left, const char* down, const char* right, const char* group) {
  const std::array<std::string, 4> keys{QueryKey(up), QueryKey(left), QueryKey(down), QueryKey(right)};
  // Four single keys read as one word ("WASD"); arrow keys as "Arrows".
  if (std::all_of(keys.begin(), keys.end(), [](const std::string& k) { return k.size() == 1; }))
    return keys[0] + keys[1] + keys[2] + keys[3];
  if (keys == std::array<std::string, 4>{"Up", "Left", "Down", "Right"}) return "Arrows";
  return group;
}
struct Labels {
  std::array<std::string, 10> text;  // By Input.
  std::vector<uint8_t> font;
};
const Labels& CurrentLabels() {
  static const Labels labels = [] {
    Labels l;
    auto key = [](const char* option) { return key_prompts::ShortKeyLabel(QueryKey(option)); };
    l.text[size_t(Input::kA)] = key("keybind_a");
    l.text[size_t(Input::kB)] = key("keybind_b");
    l.text[size_t(Input::kX)] = key("keybind_x");
    l.text[size_t(Input::kY)] = key("keybind_y");
    l.text[size_t(Input::kLB)] = key("keybind_left_shoulder");
    l.text[size_t(Input::kRB)] = key("keybind_right_shoulder");
    l.text[size_t(Input::kLT)] = key("keybind_left_trigger");
    l.text[size_t(Input::kRT)] = key("keybind_right_trigger");
    l.text[size_t(Input::kLeftStick)] =
        StickLabel("keybind_lstick_up", "keybind_lstick_left", "keybind_lstick_down", "keybind_lstick_right", "Move");
    l.text[size_t(Input::kRightStick)] =
        rex::cvar::Query<bool>("mnk_mouse")
            ? std::string("Mouse")
            : StickLabel("keybind_rstick_up", "keybind_rstick_left", "keybind_rstick_down", "keybind_rstick_right",
                         "Camera");
    const auto path = rex::filesystem::GetExecutableFolder() / "assets" / "fonts" / "Lato-Bold.ttf";
    std::ifstream f(path, std::ios::binary);
    l.font.assign(std::istreambuf_iterator<char>(f), {});
    if (l.font.empty()) REXLOG_WARN("[button icons] {} not found: the keycaps have no labels", path.string());
    return l;
  }();
  return labels;
}
bool ReplaceEnabled() {
  static const bool enabled = rex::cvar::Query<bool>("mnk_mode") && rex::cvar::Query<bool>("mnk_key_prompts");
  return enabled;
}

// --- Guest texture access ---
struct Texture {
  std::array<uint32_t, 6> fetch;
  gpu::GuestTextureLayout layout;
  const gpu::GuestFormat* format = nullptr;  // The decompressing row: RGBA8 texels.
  uint32_t format_id = 0;
};
bool Describe(const std::array<uint32_t, 6>& d, uint32_t max_side, Texture& out) {
  out.fetch = d;
  out.format_id = d[1] & 0x3F;
  if ((d[0] & 3) != 2 || ((d[5] >> 9) & 3) != 1 || !Covered(out.format_id)) return false;
  // The size before the layout: most textures stop here.
  const uint32_t width = (d[2] & 0x1FFF) + 1, height = ((d[2] >> 13) & 0x1FFF) + 1;
  if (width > max_side || height > max_side) return false;
  std::string reason;
  if (!gpu::GetTextureLayout(d, out.layout, reason) || !out.layout.has_base || out.layout.levels.empty())
    return false;
  out.format = gpu::FindGuestFormat(out.format_id, true);
  if (!out.format) out.format = gpu::FindGuestFormat(out.format_id, false);
  return out.format != nullptr;
}
gpu::GuestLevel LevelOf(uint8_t* base, const Texture& t, uint32_t level) {
  const gpu::LevelPlace& place = t.layout.levels[level];
  const uint32_t block = t.format->block, bpb = t.format->bytes_per_block;
  const uint32_t w = std::max(t.layout.width >> level, 1u), h = std::max(t.layout.height >> level, 1u);
  gpu::GuestLevel g;
  g.memory = base + 0x100000000ull + place.address;
  g.region_bytes = place.region_bytes;
  g.pitch_blocks = place.row_pitch_bytes / bpb;
  g.row_pitch_bytes = place.row_pitch_bytes;
  g.tiled = t.fetch[0] >> 31;
  g.endian = (t.fetch[1] >> 6) & 3;
  g.x0_blocks = place.x0_blocks;
  g.y0_blocks = place.y0_blocks;
  g.width_blocks = (w + block - 1) / block;
  g.height_blocks = (h + block - 1) / block;
  g.bytes_per_block = bpb;
  return g;
}
key_icon::Image DecodeRGBA(const Texture& t, const gpu::GuestLevel& g, std::vector<uint8_t> blocks, uint32_t level) {
  const uint32_t w = std::max(t.layout.width >> level, 1u), h = std::max(t.layout.height >> level, 1u);
  // k_8_8_8_8: the guest blocks after the endian swap are R, G, B, A.
  gpu::ConvertLevel(t.format->conversion, blocks, g.width_blocks, g.height_blocks, w, h);
  return key_icon::Image{w, h, std::move(blocks)};
}
std::vector<uint8_t> Encode(uint32_t format_id, const key_icon::Image& image) {
  switch (format_id) {
    case kDXT1: return key_icon::EncodeDXT1(image);
    case kDXT5: return key_icon::EncodeDXT5(image);
    default: return image.rgba;  // k_8_8_8_8.
  }
}

// Every level of the texture: a keycap over the base, box-filtered down the chain.
bool Replace(uint8_t* base, const Texture& t, const std::vector<uint8_t>& base_blocks, const std::string& label,
             const std::vector<uint8_t>& font) {
  const gpu::GuestLevel g0 = LevelOf(base, t, 0);
  key_icon::Image image = key_icon::RenderKeycap(DecodeRGBA(t, g0, base_blocks, 0), label, font);
  for (uint32_t level = 0; level < t.layout.levels.size(); ++level) {
    if (level) image = key_icon::Downscale(image);
    const gpu::GuestLevel g = LevelOf(base, t, level);
    if (!gpu::WriteGuestLevel(g, Encode(t.format_id, image), const_cast<uint8_t*>(g.memory))) return false;
  }
  return true;
}

// --- texture_dump: PNG with stored deflate blocks (no compression, a debug aid) ---
uint32_t Crc32(const uint8_t* p, size_t n, uint32_t c = 0xFFFFFFFFu) {
  static const auto table = [] {
    std::array<uint32_t, 256> t{};
    for (uint32_t i = 0; i < 256; ++i) {
      uint32_t v = i;
      for (int k = 0; k < 8; ++k) v = v & 1 ? 0xEDB88320u ^ (v >> 1) : v >> 1;
      t[i] = v;
    }
    return t;
  }();
  for (size_t i = 0; i < n; ++i) c = table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
  return c;
}
void PutBE(std::vector<uint8_t>& v, uint32_t x) {
  for (int s = 24; s >= 0; s -= 8) v.push_back(uint8_t(x >> s));
}
void Chunk(std::vector<uint8_t>& png, const char* type, const std::vector<uint8_t>& data) {
  PutBE(png, uint32_t(data.size()));
  const size_t start = png.size();
  png.insert(png.end(), type, type + 4);
  png.insert(png.end(), data.begin(), data.end());
  PutBE(png, Crc32(png.data() + start, png.size() - start) ^ 0xFFFFFFFFu);
}
bool WritePng(const std::filesystem::path& path, const key_icon::Image& image) {
  const uint32_t w = image.width, h = image.height;
  std::vector<uint8_t> raw;
  raw.reserve(size_t(h) * (w * 4 + 1));
  for (uint32_t y = 0; y < h; ++y) {
    raw.push_back(0);
    raw.insert(raw.end(), image.rgba.begin() + size_t(y) * w * 4, image.rgba.begin() + size_t(y + 1) * w * 4);
  }
  std::vector<uint8_t> z{0x78, 0x01};
  uint32_t a = 1, b = 0;
  for (uint8_t c : raw) a = (a + c) % 65521, b = (b + a) % 65521;
  for (size_t at = 0;;) {
    const size_t n = std::min<size_t>(65535, raw.size() - at);
    const bool last = at + n >= raw.size();
    z.push_back(last ? 1 : 0);
    z.push_back(uint8_t(n)), z.push_back(uint8_t(n >> 8)), z.push_back(uint8_t(~n)), z.push_back(uint8_t(~n >> 8));
    z.insert(z.end(), raw.begin() + at, raw.begin() + at + n);
    at += n;
    if (last) break;
  }
  PutBE(z, (b << 16) | a);
  std::vector<uint8_t> png{0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
  std::vector<uint8_t> ihdr;
  PutBE(ihdr, w), PutBE(ihdr, h);
  ihdr.insert(ihdr.end(), {8, 6, 0, 0, 0});
  Chunk(png, "IHDR", ihdr);
  Chunk(png, "IDAT", z);
  Chunk(png, "IEND", {});
  std::ofstream f(path, std::ios::binary);
  f.write(reinterpret_cast<const char*>(png.data()), std::streamsize(png.size()));
  return bool(f);
}
std::mutex dump_mutex;
std::unordered_set<uint64_t> dumped;
void Dump(const Texture& t, const gpu::GuestLevel& g, const std::vector<uint8_t>& blocks, uint64_t hash) {
  {
    std::lock_guard lock(dump_mutex);
    if (!dumped.insert(hash).second) return;
  }
  const auto dir = rex::filesystem::GetExecutableFolder() / "texture_dump";
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  const auto name = fmt::format("{:016X}_{}x{}_f{}.png", hash, t.layout.width, t.layout.height, t.format_id);
  if (!WritePng(dir / name, DecodeRGBA(t, g, blocks, 0))) REXLOG_WARN("[button icons] cannot write {}", name);
}

std::atomic<uint32_t> replaced{0};
}  // namespace

void OnUnlock(uint8_t* base, uint32_t resource) {
  const bool dump = REXCVAR_GET(texture_dump), replace = ReplaceEnabled();
  if (!dump && !replace) return;
  if ((Get32(base, resource) & 0xF) != 3) return;  // Not a texture.
  std::array<uint32_t, 6> d;
  for (unsigned i = 0; i < 6; ++i) d[i] = Get32(base, resource + 0x1C + 4 * i);
  if (!(d[1] & 0xFFFFF000u)) return;
  // The header holds virtual bases; the texture is read through the physical mapping.
  d[1] = gpu::GuestPhysicalAlias(d[1] & 0xFFFFF000u) | (d[1] & 0xFFF);
  if (d[5] & 0xFFFFF000u) d[5] = gpu::GuestPhysicalAlias(d[5] & 0xFFFFF000u) | (d[5] & 0xFFF);
  Texture t;
  if (!Describe(d, dump ? 8192 : kMaxIconSide, t)) return;
  const gpu::GuestLevel g0 = LevelOf(base, t, 0);
  std::vector<uint8_t> blocks;
  if (!gpu::ReadGuestLevel(g0, blocks)) return;
  const uint64_t hash = gpu::FastHash64(blocks.data(), blocks.size(), 0xCBF29CE484222325ull);
  if (dump) Dump(t, g0, blocks, hash);
  if (!replace) return;
  const auto known = std::find_if(std::begin(kIcons), std::end(kIcons), [hash](const KnownIcon& k) { return k.hash == hash; });
  if (known == std::end(kIcons)) return;
  const Labels& labels = CurrentLabels();
  const std::string& label = labels.text[size_t(known->input)];
  if (label.empty()) return;  // Unbound: the Xbox icon stays.
  if (Replace(base, t, blocks, label, labels.font)) {
    if (replaced.fetch_add(1) < 64)
      REXLOG_INFO("[button icons] {:016X} {}x{} f{} -> [{}]", hash, t.layout.width, t.layout.height, t.format_id,
                  label);
  } else {
    REXLOG_WARN("[button icons] {:016X}: the keycap does not fit the texture's place; icon kept", hash);
  }
}
}  // namespace narutorise::button_icons
