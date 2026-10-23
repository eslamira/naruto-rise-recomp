#pragma once

#include <filesystem>
#include <string>

#include "config_manager.h"
#include "localization.h"
#include "pages/pages.h"
#include "update_checker.h"
#include "update_installer.h"

struct SDL_Window;
struct ID3D11ShaderResourceView;

// C++ dependencies for isolated end-to-end tests; production uses GitHub and
// the normal per-user cache. No test endpoint is exposed in the application UI.
struct LauncherUpdateServices {
  launcher_updates::Checker::Fetch metadata = launcher_updates::FetchLatestRelease;
  launcher_updates::InstallerDownload::Transfer transfer = launcher_updates::InstallerDownload::TransferAsset;
  std::filesystem::path cache_file;
};

// The launcher window: the cover on the left; on the right the header with the
// tabs and the language, the page of the selected tab and the footer.
class LauncherUI {
 public:
  struct Cover {
    ID3D11ShaderResourceView* texture = nullptr;  // Null draws a gradient with the title.
    int width = 0;
    int height = 0;
  };

  LauncherUI(SDL_Window* window, std::filesystem::path base_dir, Cover cover,
             LauncherUpdateServices update_services = {});

  static void SetupTheme();
  void Render(GameConfig& config, bool& request_launch, std::string& status_message);
  void RefreshTranslationMods();
  bool ApplyRequestedUpdate(const GameConfig& config);

 private:
  void RenderCover(float width, float height);
  void RenderUpdateNotice();
  void RenderUpdateConfirmations(bool ask_download, bool ask_install);
  void OpenUpdateUrl(const std::string& url);
  void RenderHeader(GameConfig& config);
  void RenderPage(launcher_ui::PageContext& context) const;

  SDL_Window* window_;
  std::filesystem::path base_dir_;
  Cover cover_;
  std::string version_;
  launcher_updates::Checker updates_;
  launcher_updates::InstallerDownload download_;
  bool installed_copy_ = false;
  bool install_requested_ = false;
  TextId install_error_ = TextId::Count;
  bool ready_confirmation_shown_ = false;
  bool portable_install_confirmed_ = false;
  bool update_browser_failed_ = false;
  launcher_ui::Page page_ = launcher_ui::Page::kPlay;
  std::vector<launcher_ui::TranslationMod> translation_mods_;
};
