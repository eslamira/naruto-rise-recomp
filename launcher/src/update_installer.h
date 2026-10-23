#pragma once

#include "update_checker.h"

#include <atomic>
#include <span>

namespace launcher_updates {

enum class DownloadStage { Idle, Downloading, Verifying, Ready, Failed, Cancelled };
enum class DownloadError { None, Metadata, Network, Storage, Integrity };
struct DownloadProgress {
  DownloadStage stage = DownloadStage::Idle;
  DownloadError error = DownloadError::None;
  std::uint64_t received = 0, total = 0;
  bool cancelling = false;
};
enum class InstallResult { Started, NotReady, NotInstalled, MissingFiles, GameRunning, Failed };

bool IsInstalledCopy(const std::filesystem::path& directory);
bool InstallationInUse(const std::filesystem::path& directory);
std::wstring InstallerCommandLine(const std::filesystem::path& installer,
                                 const std::filesystem::path& directory, std::string_view language);
// HTTPS only, including each redirect. Exposed for network-independent workflow tests.
bool IsAllowedDownloadUrl(std::string_view url);

class InstallerDownload {
 public:
  using Sink = std::function<void(std::span<const char>)>;
  using Transfer = std::function<void(const std::string&, const std::atomic_bool&, const Sink&)>;
  explicit InstallerDownload(std::filesystem::path staging_root,
      Checker::Fetch metadata = FetchLatestRelease, Transfer transfer = TransferAsset);
  ~InstallerDownload();
  InstallerDownload(const InstallerDownload&) = delete;
  InstallerDownload& operator=(const InstallerDownload&) = delete;
  void Start(const std::string& version);
  void Cancel();
  DownloadProgress Progress() const;
  bool Active() const;
  // Unregistered copies require the user's explicit consent to become installed.
  InstallResult Install(const std::filesystem::path& directory, std::string_view language,
                        bool allow_portable_conversion = false);
  static void TransferAsset(const std::string& url, const std::atomic_bool& cancel, const Sink& sink);

 private:
  struct Job;
  static void Run(const std::shared_ptr<Job>& job, const std::filesystem::path& root,
                  const std::string& version, const Checker::Fetch& metadata, const Transfer& transfer);
  std::filesystem::path root_;
  Checker::Fetch metadata_;
  Transfer transfer_;
  std::shared_ptr<Job> job_;
};
}  // namespace launcher_updates
