// The real main loop/UI, with fixture downloads and queued ImGui input. This
// executable is built only by the opt-in integration target, never shipped.
#include "ui.h"
#include "localization.h"
#include <SDL3/SDL.h>
#include <imgui.h>
#include <imgui_internal.h>
#include <backends/imgui_impl_sdl3.h>

#include <atomic>
#include <fstream>
#include <thread>
#define NOMINMAX
#include <windows.h>

namespace {
std::filesystem::path test_root;
std::atomic<int> transfers{0};
int phase = 0, phase_frames = 0;
int click_frame = 0;
ImVec2 click_position;
std::chrono::steady_clock::time_point started;

struct TestRegistration {
  HKEY root = nullptr;
  std::wstring key = L"Software\\NarutoRiseUpdateUITest-" + std::to_wstring(GetCurrentProcessId());
  explicit TestRegistration(const std::filesystem::path& installation) {
    if (RegCreateKeyExW(HKEY_CURRENT_USER, key.c_str(), 0, nullptr, 0, KEY_ALL_ACCESS, nullptr, &root, nullptr) != ERROR_SUCCESS ||
        RegOverridePredefKey(HKEY_CURRENT_USER, root) != ERROR_SUCCESS) throw std::runtime_error("Cannot isolate test registry");
    HKEY app = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\{E8B610A5-927A-412E-9C64-42841FA6DE51}_is1",
        0, nullptr, 0, KEY_ALL_ACCESS, nullptr, &app, nullptr) != ERROR_SUCCESS) throw std::runtime_error("Cannot register fixture");
    const auto path = installation.wstring();
    const auto result = RegSetValueExW(app, L"Inno Setup: App Path", 0, REG_SZ,
        reinterpret_cast<const BYTE*>(path.c_str()), static_cast<DWORD>((path.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(app);
    if (result != ERROR_SUCCESS) throw std::runtime_error("Cannot set fixture path");
  }
  ~TestRegistration() {
    RegOverridePredefKey(HKEY_CURRENT_USER, nullptr);
    if (root) RegCloseKey(root);
    RegDeleteTreeW(HKEY_CURRENT_USER, key.c_str());
  }
};
std::unique_ptr<TestRegistration> test_registration;

void Record(const char* event) {
  std::ofstream(test_root / "launcher-events.log", std::ios::app) << event << '\n';
}

LauncherUpdateServices Services(const std::filesystem::path& base) {
  test_root = std::filesystem::weakly_canonical(base / "..");
  if (!test_root.filename().wstring().starts_with(L"update-validation-") ||
      !std::filesystem::is_regular_file(test_root / "test-manifest.json"))
    throw std::runtime_error("UI integration test requires its isolated fixture directory");
  started = std::chrono::steady_clock::now();
  if (std::filesystem::exists(test_root / "registered-mode")) {
    // Process-local view only. Setup runs normally with its unique fixture AppId.
    test_registration = std::make_unique<TestRegistration>(base);
    if (!launcher_updates::IsInstalledCopy(base)) throw std::runtime_error("Registered fixture was not recognized");
    Record("registered_copy_recognized");
  } else Record("portable_copy");
  LauncherUpdateServices services;
  services.cache_file = test_root / "cache/check.json";
  services.metadata = []() -> std::optional<std::string> {
    std::ifstream stream(test_root / "test-manifest.json", std::ios::binary);
    if (!stream) return {};
    return std::string(std::istreambuf_iterator<char>(stream), {});
  };
  services.transfer = [](const std::string&, const std::atomic_bool& cancel,
                         const launcher_updates::InstallerDownload::Sink& sink) {
    ++transfers;
    Record("download_started");
    std::ifstream stream(test_root / "new/NarutoRiseInstaller.exe", std::ios::binary);
    if (!stream) throw launcher_updates::DownloadError::Network;
    char buffer[65536];
    while (stream && !cancel) {
      stream.read(buffer, sizeof(buffer));
      if (stream.gcount()) sink(std::span(buffer, static_cast<size_t>(stream.gcount())));
      std::this_thread::sleep_for(std::chrono::milliseconds(3));
    }
  };
  Record("launcher_started");
  return services;
}

ImGuiWindow* Popup(const char* suffix) {
  for (auto* window : ImGui::GetCurrentContext()->Windows)
    if (window->Active && !window->Hidden && std::string_view(window->Name).find(suffix) != std::string_view::npos)
      return window;
  return nullptr;
}

void Click(ImVec2 point) { click_position = point; click_frame = 1; }
void Choose(ImGuiWindow* window, bool yes) {
  // Dialog action row uses 24 px padding, 100x36 buttons and the shared spacing.
  Click(ImVec2(window->Pos.x + 24 + 50 + (yes ? 0 : 100 + ImGui::GetStyle().ItemSpacing.x),
               window->Pos.y + window->Size.y - 22 - 18));
}

void BeforeFrame() {
  ImGui_ImplSDL3_NewFrame();
  // Queued after backend input, never sent to the user's desktop/window.
  auto& io = ImGui::GetIO();
  io.AddFocusEvent(true);
  if (click_frame) {
    io.AddMousePosEvent(click_position.x, click_position.y);
    io.AddMouseButtonEvent(0, click_frame == 1);
    click_frame = click_frame == 1 ? 2 : 0;
  }
}

void AfterFrame() {
  if (test_root.empty()) return;
  if (std::chrono::steady_clock::now() - started > std::chrono::seconds(45)) {
    Record("ERROR_ui_timeout");
    SDL_Event quit{}; quit.type = SDL_EVENT_QUIT; SDL_PushEvent(&quit);
    return;
  }
  if (click_frame) return;
  ++phase_frames;
  const auto advance = [] { ++phase; phase_frames = 0; };
  const auto main_action = [] {
    for (auto* window : ImGui::GetCurrentContext()->Windows) {
      if (window->Active && !window->Hidden && std::string_view(window->Name).find("/UpdateNotice_") != std::string_view::npos) {
        Click(ImVec2(window->Pos.x + 18 + 50, window->Pos.y + window->Size.y - 16 - 18));
        return;
      }
    }
    throw std::runtime_error("Update notice is not visible");
  };
  auto* first = Popup("###ConfirmUpdateDownload");
  auto* second = Popup("###ConfirmUpdateInstall");
  switch (phase) {
    case 0: if (phase_frames > 20) { main_action(); advance(); } break;
    case 1: if (first && phase_frames > 5) { Record("download_confirmation_visible"); Choose(first, false); advance(); } break;
    case 2: if (!first && phase_frames > 10) {
      if (transfers != 0) throw std::runtime_error("Download started after No");
      Record("download_declined_no_transfer"); main_action(); advance();
    } break;
    case 3: if (first && phase_frames > 5) { Choose(first, true); Record("download_confirmed"); advance(); } break;
    case 4: if (second && phase_frames > 5) { Record("verified_install_confirmation_visible"); Choose(second, false); advance(); } break;
    case 5: if (!second && phase_frames > 10) {
      if (transfers != 1) throw std::runtime_error("Unexpected transfer count");
      Record("install_declined_launcher_still_running"); main_action(); advance();
    } break;
    case 6: if (second && phase_frames > 5) {
      if (transfers != 1) throw std::runtime_error("Retry downloaded the installer twice");
      Record("install_confirmed"); Choose(second, true); advance();
    } break;
    default: break;
  }
}

class TestLauncherUI : public LauncherUI {
 public:
  TestLauncherUI(SDL_Window* window, std::filesystem::path base, Cover cover)
      : LauncherUI(window, base, cover, Services(base)) {}
  void Render(GameConfig& config, bool& launch, std::string& status) {
    LauncherUI::Render(config, launch, status);
    AfterFrame();
  }
  bool ApplyRequestedUpdate(const GameConfig& config) {
    const bool applied = LauncherUI::ApplyRequestedUpdate(config);
    if (applied) Record("installer_handoff_started");
    return applied;
  }
};

SDL_Window* TestCreateWindow(const char* title, int width, int height, SDL_WindowFlags flags) {
  return SDL_CreateWindow(title, width, height, flags | SDL_WINDOW_HIDDEN);
}
}  // namespace

#define LauncherUI TestLauncherUI
#define ImGui_ImplSDL3_NewFrame BeforeFrame
#define SDL_CreateWindow TestCreateWindow
#include "../src/main.cpp"
