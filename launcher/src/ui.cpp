#include "ui.h"

#include <imgui.h>
#include <SDL3/SDL.h>

#include <algorithm>
#include <iterator>
#include <utility>

#include "app_version.h"
#include "game_paths.h"
#include "localization.h"
#include "theme.h"
#include "widgets.h"

namespace ui = launcher_ui;

namespace {

std::filesystem::path UpdateCachePath() {
  char* path = SDL_GetPrefPath("NarutoRisePC", "Launcher");
  if (!path) return {};
  const auto result = std::filesystem::path(reinterpret_cast<const char8_t*>(path)) / "update-check.json";
  SDL_free(path);
  return result;
}

std::filesystem::path UpdateDownloadPath() {
  const auto cache = UpdateCachePath();
  return cache.empty() ? std::filesystem::path{} : cache.parent_path() / "updates";
}

struct Tab {
  ui::Page page;
  TextId label;
};

constexpr Tab kTabs[] = {
    {ui::Page::kPlay, TextId::TabPlay},
    {ui::Page::kSettings, TextId::TabSettings},
    {ui::Page::kGameFiles, TextId::TabGameFiles},
    {ui::Page::kDlc, TextId::TabDlc},
};

ui::GameStatus FindGame(const std::filesystem::path& base_dir, const GameConfig& config) {
  ui::GameStatus status;
  status.game_root = game_paths::Find(base_dir, config.custom_game_root);
#if defined(_WIN32)
  const auto executable = base_dir / "narutorise.exe";
#else
  const auto executable = base_dir / "narutorise";
#endif
  std::error_code error;
  status.executable_found = std::filesystem::is_regular_file(executable, error);
  return status;
}

void RenderLanguageSelector(GameConfig& config, float width) {
  ImGui::SetNextItemWidth(width);
  const auto& languages = Localization::GetLanguages();
  if (ImGui::BeginCombo("##Language", languages[static_cast<size_t>(Localization::GetLanguage())].name)) {
    for (const auto& option : languages) {
      const bool selected = Localization::GetLanguage() == option.language;
      if (ImGui::Selectable(option.name, selected)) {
        config.launcher_language = option.code;
        Localization::SetLanguage(option.language);
      }
      if (selected) ImGui::SetItemDefaultFocus();
    }
    ImGui::EndCombo();
  }
  if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", Tr(TextId::LauncherLanguageTooltip));
}

void RenderFooter(const std::string& status_message, float y) {
  ImGui::SetCursorPosY(y);
  ImGui::Separator();
  ImGui::Spacing();
  if (!status_message.empty()) {
    ImGui::TextColored(ui::kAccent, "%s", status_message.c_str());
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", status_message.c_str());
  } else {
    ui::Muted(Tr(TextId::FooterNotice));
  }
}

}  // namespace

LauncherUI::LauncherUI(SDL_Window* window, std::filesystem::path base_dir, Cover cover,
                       LauncherUpdateServices update_services)
    : window_(window), base_dir_(std::move(base_dir)), cover_(cover), version_(AppVersion(base_dir_)),
      updates_(version_, update_services.cache_file.empty() ? UpdateCachePath() : update_services.cache_file,
               update_services.metadata),
      download_(update_services.cache_file.empty() ? UpdateDownloadPath() : update_services.cache_file.parent_path() / "downloads",
                update_services.metadata, update_services.transfer),
      installed_copy_(launcher_updates::IsInstalledCopy(base_dir_)) {
  RefreshTranslationMods();
}

void LauncherUI::RefreshTranslationMods() {
  translation_mods_ = ui::DiscoverTranslationMods(base_dir_);
}

void LauncherUI::SetupTheme() { launcher_theme::Apply(); }

void LauncherUI::Render(GameConfig& config, bool& request_launch, std::string& status_message) {
  updates_.Poll();
  Localization::SetLanguageCode(config.launcher_language);
  ui::ReconcileGameLanguage(config, translation_mods_);
  ui::GameStatus game;
  ui::PageContext context{window_, base_dir_, config, game, status_message, request_launch, page_,
                          translation_mods_};
  // A folder picked in a dialog counts in this frame's game status.
  ui::ApplyGameFolderSelection(context);
  ui::ApplyDlcFolderSelection(context);
  game = FindGame(base_dir_, config);

  // ImGui's display size is in logical coordinates, including on high-DPI displays.
  const ImVec2 size = ImGui::GetIO().DisplaySize;
  const float cover_width = std::clamp(size.x * 0.35f, 330.0f, 440.0f);
  ImGui::SetNextWindowPos(ImVec2(0, 0));
  ImGui::SetNextWindowSize(size);
  ImGui::Begin("LauncherRoot", nullptr, ImGuiWindowFlags_NoDecoration |
               ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
  RenderCover(cover_width, size.y);

  ImGui::SetCursorPos(ImVec2(cover_width, 0));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(24, 24));
  ImGui::BeginChild("ContentPanel", ImVec2(size.x - cover_width, size.y),
                    ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_NoScrollbar);
  RenderHeader(config);
  RenderUpdateNotice();
  // The page fills the space down to the footer; its ID keeps each page's scroll apart.
  constexpr float kFooterHeight = 54;
  ImGui::PushID(static_cast<int>(page_));
  ImGui::BeginChild("PageBody", ImVec2(0, ImGui::GetContentRegionAvail().y - kFooterHeight));
  RenderPage(context);
  if (request_launch) {
    RefreshTranslationMods();
    ui::ReconcileGameLanguage(config, translation_mods_);
  }
  ImGui::EndChild();
  ImGui::PopID();
  RenderFooter(status_message, size.y - 64);
  ImGui::EndChild();
  ImGui::PopStyleVar();
  ImGui::End();
}

void LauncherUI::RenderCover(float width, float height) {
  auto* draw = ImGui::GetWindowDrawList();
  if (cover_.texture && cover_.width > 0 && cover_.height > 0) {
    // Cover fit: crop symmetrically instead of stretching the artwork.
    const float image_aspect = static_cast<float>(cover_.width) / cover_.height;
    const float panel_aspect = width / height;
    ImVec2 uv0(0, 0), uv1(1, 1);
    if (panel_aspect < image_aspect) {
      const float visible = panel_aspect / image_aspect;
      uv0.x = (1 - visible) / 2; uv1.x = 1 - uv0.x;
    } else {
      const float visible = image_aspect / panel_aspect;
      uv0.y = (1 - visible) / 2; uv1.y = 1 - uv0.y;
    }
    draw->AddImage(reinterpret_cast<ImTextureID>(cover_.texture), ImVec2(0, 0), ImVec2(width, height), uv0, uv1);
  } else {
    draw->AddRectFilledMultiColor(ImVec2(0, 0), ImVec2(width, height),
        IM_COL32(59, 37, 27, 255), IM_COL32(31, 29, 35, 255),
        IM_COL32(17, 18, 23, 255), IM_COL32(27, 22, 22, 255));
    ImGui::SetCursorPos(ImVec2(28, height * 0.35f));
    ui::Heading("NARUTO", 38);
    ImGui::TextUnformatted("RISE OF A NINJA");
  }
  // A shade at the bottom keeps the version readable over the artwork.
  draw->AddRectFilledMultiColor(ImVec2(0, height - 210), ImVec2(width, height),
      IM_COL32(8, 9, 13, 0), IM_COL32(8, 9, 13, 0),
      IM_COL32(8, 9, 13, 230), IM_COL32(8, 9, 13, 230));
  draw->AddLine(ImVec2(width, 0), ImVec2(width, height), IM_COL32(122, 64, 31, 255));
  ImGui::SetCursorPos(ImVec2(24, height - 150));
  ImGui::BeginDisabled(!updates_.CanCheck() || download_.Active());
  if (ImGui::Button(Tr(TextId::UpdatesCheck), ImVec2(width - 48, 34))) {
    update_browser_failed_ = false;
    updates_.CheckNow();
  }
  ImGui::EndDisabled();
  if (!updates_.CanCheck() && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
    ImGui::SetTooltip("%s", Tr(TextId::UpdatesCheckCooldown));
  ImGui::SetCursorPos(ImVec2(24, height - 106));
  ImGui::PushTextWrapPos(width - 24);
  using launcher_updates::Status;
  switch (updates_.status()) {
    case Status::Checking: ImGui::TextWrapped("%s", Tr(TextId::UpdatesChecking)); break;
    case Status::Current: ImGui::TextWrapped("%s", Tr(TextId::UpdatesCurrent)); break;
    case Status::Failed: ImGui::TextWrapped("%s", Tr(TextId::UpdatesFailed)); break;
    case Status::UnknownVersion: ImGui::TextWrapped("%s", Tr(TextId::UpdatesUnknownVersion)); break;
    case Status::Available:
      ImGui::TextWrapped(Tr(TextId::UpdatesAvailable), updates_.release().version.c_str()); break;
  }
  ImGui::PopTextWrapPos();
  if (!version_.empty()) {
    ImGui::SetCursorPos(ImVec2(24, height - 40));
    ImGui::Text("v%s", version_.c_str());
  }
}

void LauncherUI::OpenUpdateUrl(const std::string& url) {
  update_browser_failed_ = !SDL_OpenURL(url.c_str());
}

void LauncherUI::RenderUpdateNotice() {
  if (updates_.status() != launcher_updates::Status::Available) return;
  const auto& release = updates_.release();
  using launcher_updates::DownloadStage;
  const auto progress = download_.Progress();
  const bool busy = progress.stage == DownloadStage::Downloading || progress.stage == DownloadStage::Verifying;
  bool ask_download = false, ask_install = false;

  ui::BeginCard("UpdateNotice", 0);
  ImGui::PushTextWrapPos(0);
  ImGui::TextColored(ui::kAccent, Tr(TextId::UpdatesAvailable), release.version.c_str());
  ImGui::PopTextWrapPos();
  if (busy) {
    ui::Muted(Tr(progress.cancelling ? TextId::UpdatesCancelling :
        progress.stage == DownloadStage::Verifying ? TextId::UpdatesVerifying : TextId::UpdatesDownloading));
    const float fraction = progress.total ? static_cast<float>(progress.received) / progress.total : 0;
    ImGui::ProgressBar(fraction, ImVec2(-1, 0));
    ImGui::Text("%.1f / %.1f MB", progress.received / 1048576.0, progress.total / 1048576.0);
  } else if (progress.stage == DownloadStage::Ready) {
    ui::Muted(Tr(TextId::UpdatesDownloadReady));
    if (!ready_confirmation_shown_) {
      ask_install = true;
      ready_confirmation_shown_ = true;
    }
  } else if (progress.stage == DownloadStage::Failed) {
    using launcher_updates::DownloadError;
    const TextId message = progress.error == DownloadError::Integrity ? TextId::UpdatesIntegrityFailed :
        progress.error == DownloadError::Metadata ? TextId::UpdatesMetadataFailed :
        progress.error == DownloadError::Storage ? TextId::UpdatesStorageFailed : TextId::UpdatesDownloadFailed;
    ImGui::TextWrapped("%s", Tr(message));
  } else if (progress.stage == DownloadStage::Cancelled) {
    ui::Muted(Tr(TextId::UpdatesCancelled));
  }
  if (install_error_ != TextId::Count) ImGui::TextWrapped("%s", Tr(install_error_));
  if (release.installer_url.empty()) ui::Muted(Tr(TextId::UpdatesNoInstaller));

  // Two actions only. During a transfer the primary action becomes Cancel.
  ImGui::BeginDisabled(progress.cancelling && busy);
  if (busy) {
    if (ImGui::Button(Tr(TextId::UpdatesCancelDownload), ImVec2(0, 36))) download_.Cancel();
  } else {
    ImGui::BeginDisabled(release.installer_url.empty());
    if (ui::PrimaryButton(Tr(TextId::UpdatesDownloadInstall), ImVec2(0, 36))) {
      if (progress.stage == DownloadStage::Ready) ask_install = true;
      else ask_download = true;
    }
    ImGui::EndDisabled();
  }
  ImGui::EndDisabled();
  const float notes_width = ImGui::CalcTextSize(Tr(TextId::UpdatesNotes)).x + ImGui::GetStyle().FramePadding.x * 2;
  if (ImGui::GetItemRectMax().x + ImGui::GetStyle().ItemSpacing.x + notes_width <=
      ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x) ImGui::SameLine();
  if (ImGui::Button(Tr(TextId::UpdatesNotes), ImVec2(0, 36))) OpenUpdateUrl(release.page_url);
  if (update_browser_failed_) ImGui::TextWrapped("%s", Tr(TextId::UpdatesBrowserFailed));
  ui::EndCard();
  ImGui::Spacing();
  RenderUpdateConfirmations(ask_download, ask_install);
}

void LauncherUI::RenderUpdateConfirmations(bool ask_download, bool ask_install) {
  const auto& release = updates_.release();
  const std::string download_title = std::string(Tr(TextId::UpdatesDownloadInstall)) + "###ConfirmUpdateDownload";
  const std::string install_title = std::string(Tr(TextId::UpdatesLaunchInstaller)) + "###ConfirmUpdateInstall";
  if (ask_download) {
    installed_copy_ = launcher_updates::IsInstalledCopy(base_dir_);
    ImGui::OpenPopup(download_title.c_str());
  }
  if (ask_install) ImGui::OpenPopup(install_title.c_str());

  const ImVec2 display = ImGui::GetIO().DisplaySize;
  auto prepare = [&] {
    ImGui::SetNextWindowPos(ImVec2(display.x / 2, display.y / 2), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(std::min(560.0f, display.x - 48), 0));
  };
  auto destination = [&] {
    ui::Muted(Tr(TextId::UpdatesDestination));
    const auto path = base_dir_.u8string();
    ImGui::TextWrapped("%s", reinterpret_cast<const char*>(path.c_str()));
  };
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(24, 22));
  prepare();
  if (ImGui::BeginPopupModal(download_title.c_str(), nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings)) {
    ImGui::TextWrapped(Tr(TextId::UpdatesAvailable), release.version.c_str());
    ImGui::Spacing();
    ImGui::TextWrapped("%s", Tr(TextId::UpdatesConfirmDownload));
    if (!installed_copy_) {
      ImGui::Spacing();
      ImGui::TextWrapped("%s", Tr(TextId::UpdatesPortableConversion));
    }
    ImGui::Spacing();
    destination();
    ImGui::Separator();
    ImGui::TextWrapped("%s", Tr(TextId::UpdatesContinue));
    if (ui::PrimaryButton(Tr(TextId::UpdatesYes), ImVec2(100, 36))) {
      portable_install_confirmed_ = !installed_copy_;
      ready_confirmation_shown_ = false;
      install_error_ = TextId::Count;
      download_.Start(release.version);
      ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button(Tr(TextId::UpdatesNo), ImVec2(100, 36)) || ImGui::IsKeyPressed(ImGuiKey_Escape))
      ImGui::CloseCurrentPopup();
    ImGui::SetItemDefaultFocus();
    ImGui::EndPopup();
  }
  prepare();
  if (ImGui::BeginPopupModal(install_title.c_str(), nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings)) {
    ImGui::TextWrapped("%s", Tr(TextId::UpdatesConfirmInstall));
    ImGui::Spacing();
    destination();
    ImGui::Separator();
    if (ui::PrimaryButton(Tr(TextId::UpdatesYes), ImVec2(100, 36))) {
      install_requested_ = true;
      ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button(Tr(TextId::UpdatesNo), ImVec2(100, 36)) || ImGui::IsKeyPressed(ImGuiKey_Escape))
      ImGui::CloseCurrentPopup();
    ImGui::SetItemDefaultFocus();
    ImGui::EndPopup();
  }
  ImGui::PopStyleVar();
}

bool LauncherUI::ApplyRequestedUpdate(const GameConfig& config) {
  if (!install_requested_) return false;
  install_requested_ = false;
  if (!ConfigManager::Save(base_dir_ / "narutorise.toml", config)) {
    install_error_ = TextId::UpdatesSaveFailed;
    return false;
  }
  const auto result = download_.Install(base_dir_, config.launcher_language, portable_install_confirmed_);
  if (result == launcher_updates::InstallResult::Started) return true;
  install_error_ = result == launcher_updates::InstallResult::GameRunning ? TextId::UpdatesCloseGame :
      result == launcher_updates::InstallResult::MissingFiles ? TextId::UpdatesMissingPortFiles :
      result == launcher_updates::InstallResult::NotInstalled ? TextId::UpdatesNotInstalled : TextId::UpdatesInstallFailed;
  return false;
}

void LauncherUI::RenderHeader(GameConfig& config) {
  constexpr float kLanguageWidth = 130;
  // Each tab is as wide as its label, plus an equal share of the width left
  // beside the language selector.
  float label_widths[std::size(kTabs)];
  float spare = ImGui::GetContentRegionAvail().x - kLanguageWidth - 28;
  for (size_t i = 0; i < std::size(kTabs); ++i) {
    label_widths[i] = ImGui::CalcTextSize(Tr(kTabs[i].label)).x + ImGui::GetStyle().FramePadding.x * 2;
    spare -= label_widths[i];
  }
  const float share = std::max(0.0f, spare / std::size(kTabs));
  for (size_t i = 0; i < std::size(kTabs); ++i) {
    if (i) ImGui::SameLine(0, 4);
    ImGui::PushID(static_cast<int>(i));
    ImGui::PushStyleColor(ImGuiCol_Button, page_ == kTabs[i].page ? ui::kPrimary : ui::kIdleTab);
    if (ImGui::Button(Tr(kTabs[i].label), ImVec2(label_widths[i] + share, 38))) page_ = kTabs[i].page;
    ImGui::PopStyleColor();
    ImGui::PopID();
  }
  ImGui::SameLine(0, 16);
  RenderLanguageSelector(config, kLanguageWidth);
  ImGui::Spacing();
  ImGui::Separator();
  ImGui::Spacing();
}

void LauncherUI::RenderPage(ui::PageContext& context) const {
  switch (page_) {
    case ui::Page::kPlay: ui::RenderPlayPage(context); break;
    case ui::Page::kSettings: ui::RenderSettingsPage(context); break;
    case ui::Page::kGameFiles: ui::RenderGameFilesPage(context); break;
    case ui::Page::kDlc: ui::RenderDlcPage(context); break;
  }
}
