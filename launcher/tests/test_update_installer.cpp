#include "update_installer.h"

#define NOMINMAX
#include <windows.h>
#include <bcrypt.h>
#include <shellapi.h>
#include <nlohmann/json.hpp>

#include <fstream>
#include <iostream>
#include <thread>

using namespace launcher_updates;
using namespace std::chrono_literals;

namespace {
void Require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
struct Temp {
  std::filesystem::path dir = std::filesystem::temp_directory_path() /
      ("narutorise-installer-test-" + std::to_string(GetCurrentProcessId()) + "-" +
       std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  Temp() { std::filesystem::create_directory(dir); }
  ~Temp() {
    std::error_code error;
    if (dir.is_absolute() && dir.parent_path() == std::filesystem::temp_directory_path() &&
        dir.filename().string().starts_with("narutorise-installer-test-")) std::filesystem::remove_all(dir, error);
  }
};
std::string Hash(std::string_view data) {
  unsigned char bytes[32];
  Require(BCryptHash(BCRYPT_SHA256_ALG_HANDLE, nullptr, 0,
      reinterpret_cast<PUCHAR>(const_cast<char*>(data.data())), static_cast<ULONG>(data.size()), bytes, 32) >= 0, "Test hash failed");
  std::string result;
  constexpr char hex[] = "0123456789abcdef";
  for (auto c : bytes) { result += hex[c >> 4]; result += hex[c & 15]; }
  return result;
}
nlohmann::json Metadata(std::uint64_t size = 3,
    std::string digest = "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad") {
  return {{"tag_name", "v0.3.0"}, {"prerelease", false}, {"draft", false},
    {"assets", nlohmann::json::array({{{"name", "NarutoRiseInstaller.exe"}, {"state", "uploaded"},
      {"size", size}, {"digest", "sha256:" + digest},
      {"browser_download_url", "https://github.com/jorjbarbosa/naruto-rise-recomp/releases/download/v0.3.0/NarutoRiseInstaller.exe"}}})}};
}
void Finish(InstallerDownload& download, std::chrono::seconds timeout = 5s) {
  const auto end = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < end) {
    const auto stage = download.Progress().stage;
    if (stage != DownloadStage::Downloading && stage != DownloadStage::Verifying) return;
    std::this_thread::sleep_for(2ms);
  }
  throw std::runtime_error("Download timed out");
}
auto Bytes(std::string value) {
  return [value](const std::string&, const std::atomic_bool&, const InstallerDownload::Sink& sink) {
    sink(std::span(value.data(), value.size()));
  };
}
std::vector<std::wstring> Arguments(const std::wstring& command) {
  int count = 0;
  auto argv = CommandLineToArgvW(command.c_str(), &count);
  Require(argv != nullptr, "Could not parse command line");
  std::vector<std::wstring> result(argv, argv + count);
  LocalFree(argv);
  return result;
}
// All registry writes are redirected to a unique test key. The real installation is untouched.
struct Registry {
  std::wstring key = L"Software\\NarutoRiseUpdateTests-" + std::to_wstring(GetCurrentProcessId());
  HKEY root = nullptr;
  Registry() {
    Require(RegCreateKeyExW(HKEY_CURRENT_USER, key.c_str(), 0, nullptr, 0, KEY_ALL_ACCESS, nullptr, &root, nullptr) == ERROR_SUCCESS,
            "Test registry creation failed");
    Require(RegOverridePredefKey(HKEY_CURRENT_USER, root) == ERROR_SUCCESS, "Registry isolation failed");
  }
  void Register(const std::filesystem::path& dir) {
    HKEY app = nullptr;
    Require(RegCreateKeyExW(HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\{E8B610A5-927A-412E-9C64-42841FA6DE51}_is1",
        0, nullptr, 0, KEY_ALL_ACCESS, nullptr, &app, nullptr) == ERROR_SUCCESS, "Test install key failed");
    const auto path = dir.wstring();
    const auto status = RegSetValueExW(app, L"Inno Setup: App Path", 0, REG_SZ,
        reinterpret_cast<const BYTE*>(path.c_str()), static_cast<DWORD>((path.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(app);
    Require(status == ERROR_SUCCESS, "Test install path failed");
  }
  ~Registry() { RegOverridePredefKey(HKEY_CURRENT_USER, nullptr); RegCloseKey(root); RegDeleteTreeW(HKEY_CURRENT_USER, key.c_str()); }
};
}  // namespace

int main(int argc, char* argv[]) {
  try {
    const auto args = Arguments(GetCommandLineW());
    // The test binary doubles as a harmless installer: record arguments, never install anything.
    if (args.size() > 1 && args[1] == L"/MODE=update") {
      nlohmann::json report = nlohmann::json::array();
      std::filesystem::path destination;
      for (const auto& argument : args) {
        const auto utf8 = std::filesystem::path(argument).u8string();
        report.push_back(std::string(reinterpret_cast<const char*>(utf8.data()), utf8.size()));
        if (argument.starts_with(L"/DIR=")) destination = argument.substr(5);
      }
      if (destination.empty() || !destination.parent_path().filename().wstring().starts_with(L"narutorise-installer-test-")) return 2;
      {
        std::ofstream output(destination / "handoff.tmp");
        output << report;
        output.close();
        if (!output) return 3;
      }
      std::filesystem::rename(destination / "handoff.tmp", destination / "handoff.json");
      return 0;
    }
    if (args.size() > 1 && args[1] == L"--hold") { Sleep(1500); return 0; }
    Temp temp;
    if (argc == 2 && std::string_view(argv[1]) == "--live") {
      const auto metadata = FetchLatestRelease();
      Require(metadata.has_value(), "Live metadata unavailable");
      const auto release = ParseRelease(*metadata);
      Require(release.has_value(), "Live release invalid");
      InstallerDownload download(temp.dir);
      download.Start(release->version);
      Finish(download, 120s);
      Require(download.Progress().stage == DownloadStage::Ready, "Live download or verification failed");
      std::cout << "Verified GitHub installer: " << download.Progress().received << " bytes; no installer executed\n";
      download.Cancel();
      return 0;
    }

    const auto metadata = [] { return Metadata().dump(); };
    Require(ParseRelease(Metadata().dump())->installer_size == 3, "Asset size missing");
    for (auto invalid : {nlohmann::json(nullptr), nlohmann::json("sha256:bad"), nlohmann::json(1)}) {
      auto json = Metadata(); json["assets"][0]["digest"] = invalid;
      const auto release = ParseRelease(json.dump());
      Require(release && release->installer_sha256.empty(), "Invalid digest enables install");
    }
    for (std::int64_t bad : {-1, 0, 1073741825}) {
      auto json = Metadata(); json["assets"][0]["size"] = bad;
      Require(ParseRelease(json.dump())->installer_size == 0, "Invalid size enables install");
    }
    for (auto url : {"http://github.com/jorjbarbosa/naruto-rise-recomp/releases/download/x/y",
        "https://github.com.evil/a", "https://evil@github.com/a", "https://github.com:444/a",
        "https://release-assets.githubusercontent.com.evil/a", "https://github.com/other/repo/a",
        "https://release-assets.githubusercontent.com/a\r\nX: y"})
      Require(!IsAllowedDownloadUrl(url), "Unsafe redirect accepted");
    Require(IsAllowedDownloadUrl("https://release-assets.githubusercontent.com/a?sig=abc%2Fdef"), "GitHub CDN rejected");

    const auto command = Arguments(InstallerCommandLine(L"C:\\download folder\\setup.exe", L"D:\\Games\\Naruto á\\", "pt_BR"));
    Require(command[0] == L"C:\\download folder\\setup.exe" && command[4] == L"/DIR=D:\\Games\\Naruto á\\" &&
            command[5] == L"/LANG=brazilianportuguese", "Command line quoting/language failed");
    {
      InstallerDownload download(temp.dir, metadata, Bytes("abc"));
      download.Start("0.3.0"); Finish(download);
      Require(download.Progress().stage == DownloadStage::Ready, "Valid download not ready");
      auto directory = std::filesystem::directory_iterator(temp.dir)->path();
      HANDLE writer = CreateFileW((directory / "NarutoRiseInstaller.exe").c_str(), GENERIC_WRITE,
          FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
      Require(writer == INVALID_HANDLE_VALUE, "Verified file was writable before launch");
      Require(download.Install(temp.dir, "en") == InstallResult::NotInstalled, "Portable copy converted automatically");
      Require(download.Install(temp.dir, "en", true) == InstallResult::MissingFiles,
              "Standalone launcher test folder accepted as a complete installation");
      download.Cancel();
      Require(std::filesystem::is_empty(temp.dir), "Cancelled ready download left files");
    }
    for (const auto* content : {"ab", "abcd", "abd"}) {
      InstallerDownload download(temp.dir, metadata, Bytes(content));
      download.Start("0.3.0"); Finish(download);
      Require(download.Progress().stage == DownloadStage::Failed && download.Progress().error == DownloadError::Integrity,
              "Short, oversized or altered download accepted");
      Require(std::filesystem::is_empty(temp.dir), "Rejected download left files");
    }
    {
      InstallerDownload download(temp.dir, metadata, [](const auto&, const auto&, const auto&) { throw DownloadError::Network; });
      download.Start("0.3.0"); Finish(download);
      Require(download.Progress().error == DownloadError::Network && std::filesystem::is_empty(temp.dir), "Network failure cleanup failed");
    }
    {
      InstallerDownload download(temp.dir, metadata, Bytes("abc"));
      download.Start("0.2.0"); Finish(download);
      Require(download.Progress().error == DownloadError::Metadata && std::filesystem::is_empty(temp.dir), "Changed release accepted");
      InstallerDownload offline(temp.dir, []() -> std::optional<std::string> { return {}; }, Bytes("abc"));
      offline.Start("0.3.0"); Finish(offline);
      Require(offline.Progress().error == DownloadError::Metadata, "Executed from stale metadata");
    }
    {
      InstallerDownload download({}, metadata, Bytes("abc"));
      download.Start("0.3.0"); Finish(download);
      Require(download.Progress().error == DownloadError::Storage, "Missing staging directory accepted");
    }
    {
      std::atomic_bool entered = false;
      InstallerDownload download(temp.dir, metadata, [&](const auto&, const std::atomic_bool& cancel, const auto&) {
        entered = true;
        while (!cancel) std::this_thread::sleep_for(1ms);
      });
      download.Start("0.3.0");
      const auto deadline = std::chrono::steady_clock::now() + 5s;
      while (!entered && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(1ms);
      Require(entered, "Cancellation fixture did not start");
      download.Cancel(); Finish(download);
      Require(download.Progress().stage == DownloadStage::Cancelled && std::filesystem::is_empty(temp.dir), "Cancellation cleanup failed");
    }
    {
      Registry registry;
      const auto installed = temp.dir / L"Installed game á";
      std::filesystem::create_directory(installed);
      std::ofstream(installed / "narutorise.version") << "0.2.0-beta.2";
      registry.Register(installed);
      Require(IsInstalledCopy(installed) && !IsInstalledCopy(temp.dir), "Registry path match failed");
      const std::filesystem::path self = args[0];
      const auto game = installed / "narutorise.exe";
      std::filesystem::copy_file(self, game);
      std::filesystem::copy_file(self, installed / "narutorise_launcher.exe");
      std::wstring hold = L"\"" + game.wstring() + L"\" --hold";
      STARTUPINFOW startup{}; startup.cb = sizeof(startup);
      PROCESS_INFORMATION process{};
      Require(CreateProcessW(game.c_str(), hold.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
          nullptr, installed.c_str(), &startup, &process), "Game process fixture failed");
      Require(InstallationInUse(installed), "Running game missed");
      Require(!InstallationInUse(temp.dir), "Unrelated installation blocked");
      WaitForSingleObject(process.hProcess, 5000);
      CloseHandle(process.hThread); CloseHandle(process.hProcess);
      Require(!InstallationInUse(installed), "Exited game still blocks update");

      std::ifstream stream(self, std::ios::binary);
      const std::string binary{std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
      const auto fixture = Metadata(binary.size(), Hash(binary)).dump();
      InstallerDownload download(temp.dir / "downloads", [fixture] { return fixture; }, Bytes(binary));
      download.Start("0.3.0"); Finish(download);
      Require(download.Progress().stage == DownloadStage::Ready, "Installer stub verification failed");
      std::ofstream(installed / "narutorise.version") << "0.4.0";
      Require(download.Install(installed, "pt_BR") == InstallResult::NotReady, "Installation updated during download was downgraded");
      std::ofstream(installed / "narutorise.version") << "0.2.0-beta.2";
      Require(download.Install(installed, "pt_BR") == InstallResult::Started, "Verified installer handoff failed");
      Require(download.Install(installed, "pt_BR") == InstallResult::NotReady, "Installer launched twice");
      const auto deadline = std::chrono::steady_clock::now() + 5s;
      while (!std::filesystem::exists(installed / "handoff.json") && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(10ms);
      nlohmann::json report;
      std::ifstream(installed / "handoff.json") >> report;
      const auto dir_utf8 = installed.u8string();
      Require(report[1] == "/MODE=update" && report[2] == "/NORESTART" && report[3] == "/NOFORCECLOSEAPPLICATIONS" &&
              report[4] == "/DIR=" + std::string(reinterpret_cast<const char*>(dir_utf8.data()), dir_utf8.size()) &&
              report[5] == "/LANG=brazilianportuguese", "Installer received wrong destination/options");

      const auto portable = temp.dir / "Portable game";
      std::filesystem::create_directory(portable);
      std::filesystem::copy_file(self, portable / "narutorise.exe");
      std::filesystem::copy_file(self, portable / "narutorise_launcher.exe");
      std::ofstream(portable / "narutorise.version") << "0.2.0-beta.2";
      InstallerDownload conversion(temp.dir / "portable-downloads", [fixture] { return fixture; }, Bytes(binary));
      conversion.Start("0.3.0"); Finish(conversion);
      Require(conversion.Install(portable, "en") == InstallResult::NotInstalled, "Portable consent was not required");
      Require(conversion.Install(portable, "en", true) == InstallResult::Started, "Consented portable conversion rejected");
      const auto conversion_deadline = std::chrono::steady_clock::now() + 5s;
      while (!std::filesystem::exists(portable / "handoff.json") && std::chrono::steady_clock::now() < conversion_deadline)
        std::this_thread::sleep_for(10ms);
      Require(std::filesystem::exists(portable / "handoff.json"), "Portable installer handoff did not complete");
    }
    std::cout << "Installer tests passed: metadata, SHA-256, size, locks, redirects, cancel, failures, portable gate, process detection and handoff\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
