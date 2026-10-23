#include "update_installer.h"
#include "app_version.h"

#include <algorithm>
#include <array>
#include <mutex>
#include <thread>
#include <vector>

#if defined(_WIN32)
#define NOMINMAX
#include <windows.h>
#include <bcrypt.h>
#include <tlhelp32.h>
#include <winhttp.h>
#endif

namespace launcher_updates {
namespace {
struct Cancelled {};

std::wstring Quote(std::wstring_view argument) {
  std::wstring result = L"\"";
  size_t slashes = 0;
  for (wchar_t c : argument) {
    if (c == L'\\') { ++slashes; continue; }
    result.append(c == L'"' ? slashes * 2 + 1 : slashes, L'\\');
    slashes = 0;
    result += c;
  }
  result.append(slashes * 2, L'\\');
  return result + L'"';
}

#if defined(_WIN32)
struct File {
  HANDLE value = INVALID_HANDLE_VALUE;
  ~File() { Close(); }
  void Close() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); value = INVALID_HANDLE_VALUE; }
};
struct Internet {
  HINTERNET value = nullptr;
  ~Internet() { if (value) WinHttpCloseHandle(value); }
};
struct Prepared {
  std::filesystem::path directory, executable;
  File lock;
  bool launched = false;
  ~Prepared() {
    lock.Close();
    if (launched || directory.empty()) return;
    std::error_code error;
    // Only files created by this job, never a recursive delete.
    if (!executable.empty()) std::filesystem::remove(executable, error);
    std::filesystem::remove(directory, error);
  }
};

std::string HashFile(HANDLE file, const std::atomic_bool& cancel) {
  BCRYPT_ALG_HANDLE algorithm = nullptr;
  BCRYPT_HASH_HANDLE hash = nullptr;
  struct Cleanup {
    BCRYPT_ALG_HANDLE& algorithm; BCRYPT_HASH_HANDLE& hash;
    ~Cleanup() { if (hash) BCryptDestroyHash(hash); if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0); }
  } cleanup{algorithm, hash};
  if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0 ||
      BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0) < 0) throw DownloadError::Integrity;
  LARGE_INTEGER beginning{};
  if (!SetFilePointerEx(file, beginning, nullptr, FILE_BEGIN)) throw DownloadError::Storage;
  std::array<unsigned char, 65536> buffer;
  for (;;) {
    if (cancel.load()) throw Cancelled{};
    DWORD read = 0;
    if (!ReadFile(file, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr)) throw DownloadError::Storage;
    if (!read) break;
    if (BCryptHashData(hash, buffer.data(), read, 0) < 0) throw DownloadError::Integrity;
  }
  std::array<unsigned char, 32> digest;
  if (BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0) < 0) throw DownloadError::Integrity;
  constexpr char hex[] = "0123456789abcdef";
  std::string text;
  for (auto byte : digest) { text += hex[byte >> 4]; text += hex[byte & 15]; }
  return text;
}

std::filesystem::path NewDirectory(const std::filesystem::path& root) {
  if (root.empty() || !root.is_absolute()) throw DownloadError::Storage;
  std::error_code error;
  std::filesystem::create_directories(root, error);
  if (error) throw DownloadError::Storage;
  std::array<unsigned char, 16> random;
  if (BCryptGenRandom(nullptr, random.data(), static_cast<ULONG>(random.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0)
    throw DownloadError::Storage;
  std::string name = "download-";
  constexpr char hex[] = "0123456789abcdef";
  for (auto byte : random) { name += hex[byte >> 4]; name += hex[byte & 15]; }
  const auto path = root / name;
  if (!std::filesystem::create_directory(path, error) || error) throw DownloadError::Storage;
  return path;
}
#endif
}  // namespace

bool IsAllowedDownloadUrl(std::string_view url) {
  if (url.size() > 16384 || !url.starts_with("https://") ||
      std::any_of(url.begin(), url.end(), [](unsigned char c) { return c <= 32 || c >= 127 || c == '\\' || c == '#'; }))
    return false;
  const auto path_start = url.find('/', 8);
  if (path_start == std::string_view::npos) return false;
  const auto host = url.substr(8, path_start - 8);
  if (host == "github.com")
    return url.substr(path_start).starts_with("/jorjbarbosa/naruto-rise-recomp/releases/download/");
  return host == "release-assets.githubusercontent.com" || host == "objects.githubusercontent.com";
}

std::wstring InstallerCommandLine(const std::filesystem::path& installer,
                                 const std::filesystem::path& directory, std::string_view language) {
  std::wstring lang = L"english";
  if (language == "pt_BR" || language == "pt-BR" || language == "pt") lang = L"brazilianportuguese";
  else if (language == "fr") lang = L"french";
  else if (language == "de") lang = L"german";
  else if (language == "es") lang = L"spanish";
  else if (language == "it") lang = L"italian";
  else if (language == "ru") lang = L"russian";
  return Quote(installer.wstring()) + L" /MODE=update /NORESTART /NOFORCECLOSEAPPLICATIONS " +
      Quote(L"/DIR=" + directory.wstring()) + L" /LANG=" + lang;
}

bool IsInstalledCopy(const std::filesystem::path& directory) {
#if defined(_WIN32)
  constexpr const wchar_t* keys[] = {
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\{E8B610A5-927A-412E-9C64-42841FA6DE51}_is1",
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\NarutoRiseRecomp"};
  constexpr const wchar_t* values[] = {L"Inno Setup: App Path", L"InstallLocation"};
  for (size_t i = 0; i < std::size(keys); ++i) {
    for (DWORD view : {RRF_SUBKEY_WOW6464KEY, RRF_SUBKEY_WOW6432KEY}) {
      wchar_t path[32768]; DWORD bytes = sizeof(path);
      if (RegGetValueW(HKEY_CURRENT_USER, keys[i], values[i], RRF_RT_REG_SZ | view,
                      nullptr, path, &bytes) != ERROR_SUCCESS) continue;
      std::error_code error;
      if (std::filesystem::equivalent(directory, path, error) && !error) return true;
    }
  }
#endif
  return false;
}

bool InstallationInUse(const std::filesystem::path& directory) {
#if defined(_WIN32)
  File snapshot{CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0)};
  if (snapshot.value == INVALID_HANDLE_VALUE) return true;
  PROCESSENTRY32W entry{}; entry.dwSize = sizeof(entry);
  if (!Process32FirstW(snapshot.value, &entry)) return true;
  do {
    if (entry.th32ProcessID == GetCurrentProcessId()) continue;
    if (_wcsicmp(entry.szExeFile, L"narutorise.exe") != 0 &&
        _wcsicmp(entry.szExeFile, L"narutorise_launcher.exe") != 0) continue;
    File process{OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ProcessID)};
    if (!process.value) {
      process.value = INVALID_HANDLE_VALUE;
      if (GetLastError() == ERROR_INVALID_PARAMETER) continue; // Process exited.
      return true;
    }
    wchar_t path[32768]; DWORD size = static_cast<DWORD>(std::size(path));
    if (!QueryFullProcessImageNameW(process.value, 0, path, &size)) return true;
    std::error_code error;
    if (std::filesystem::equivalent(directory, std::filesystem::path(path).parent_path(), error) || error) return true;
  } while (Process32NextW(snapshot.value, &entry));
  return GetLastError() != ERROR_NO_MORE_FILES;
#else
  return true;
#endif
}

void InstallerDownload::TransferAsset(const std::string& initial_url, const std::atomic_bool& cancel, const Sink& sink) {
#if defined(_WIN32)
  Internet session{WinHttpOpen(L"NarutoRiseLauncher-UpdateDownload/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
      WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0)};
  if (!session.value || !WinHttpSetTimeouts(session.value, 5000, 5000, 5000, 5000)) throw DownloadError::Network;
  std::string url = initial_url;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::hours(1);
  for (int redirects = 0; redirects <= 5; ++redirects) {
    if (cancel.load()) throw Cancelled{};
    if (!IsAllowedDownloadUrl(url)) throw DownloadError::Network;
    const std::wstring wide(url.begin(), url.end());
    URL_COMPONENTS parts{}; parts.dwStructSize = sizeof(parts);
    parts.dwHostNameLength = parts.dwUrlPathLength = parts.dwExtraInfoLength = static_cast<DWORD>(-1);
    if (!WinHttpCrackUrl(wide.c_str(), 0, ICU_REJECT_USERPWD, &parts) ||
        parts.nScheme != INTERNET_SCHEME_HTTPS || parts.nPort != INTERNET_DEFAULT_HTTPS_PORT) throw DownloadError::Network;
    const std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
    std::wstring resource(parts.lpszUrlPath, parts.dwUrlPathLength);
    if (parts.dwExtraInfoLength) resource.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);
    Internet connection{WinHttpConnect(session.value, host.c_str(), parts.nPort, 0)};
    if (!connection.value) throw DownloadError::Network;
    Internet request{WinHttpOpenRequest(connection.value, L"GET", resource.c_str(), nullptr,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE)};
    DWORD policy = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
    if (!request.value || !WinHttpSetOption(request.value, WINHTTP_OPTION_REDIRECT_POLICY, &policy, sizeof(policy)) ||
        !WinHttpSendRequest(request.value, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(request.value, nullptr)) throw DownloadError::Network;
    DWORD status = 0, size = sizeof(status);
    if (!WinHttpQueryHeaders(request.value, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
        WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX)) throw DownloadError::Network;
    if (status == 301 || status == 302 || status == 303 || status == 307 || status == 308) {
      std::array<wchar_t, 16384> location{}; size = static_cast<DWORD>(sizeof(location));
      if (!WinHttpQueryHeaders(request.value, WINHTTP_QUERY_LOCATION, WINHTTP_HEADER_NAME_BY_INDEX,
          location.data(), &size, WINHTTP_NO_HEADER_INDEX)) throw DownloadError::Network;
      const std::wstring_view target(location.data());
      if (std::any_of(target.begin(), target.end(), [](wchar_t c) { return c > 127; })) throw DownloadError::Network;
      url.assign(target.begin(), target.end());
      continue;
    }
    if (status != 200) throw DownloadError::Network;
    std::array<char, 65536> buffer;
    for (;;) {
      if (cancel.load()) throw Cancelled{};
      if (std::chrono::steady_clock::now() > deadline) throw DownloadError::Network;
      DWORD read = 0;
      if (!WinHttpReadData(request.value, buffer.data(), static_cast<DWORD>(buffer.size()), &read)) throw DownloadError::Network;
      if (!read) return;
      sink(std::span(buffer.data(), read));
    }
  }
#endif
  throw DownloadError::Network;
}

struct InstallerDownload::Job {
  std::string version;
  std::atomic_bool cancel{false};
  std::atomic<DownloadStage> stage{DownloadStage::Downloading};
  std::atomic<DownloadError> error{DownloadError::None};
  std::atomic<std::uint64_t> received{0}, total{0};
  std::mutex mutex;
#if defined(_WIN32)
  std::shared_ptr<Prepared> prepared;
#endif
};

InstallerDownload::InstallerDownload(std::filesystem::path root, Checker::Fetch metadata, Transfer transfer)
    : root_(std::move(root)), metadata_(std::move(metadata)), transfer_(std::move(transfer)) {}
InstallerDownload::~InstallerDownload() { Cancel(); }

bool InstallerDownload::Active() const {
  const auto stage = Progress().stage;
  return stage == DownloadStage::Downloading || stage == DownloadStage::Verifying || stage == DownloadStage::Ready;
}

DownloadProgress InstallerDownload::Progress() const {
  if (!job_) return {};
  return {job_->stage.load(), job_->error.load(), job_->received.load(), job_->total.load(), job_->cancel.load()};
}

void InstallerDownload::Cancel() {
  if (!job_) return;
  job_->cancel = true;
  std::lock_guard lock(job_->mutex);
  if (job_->stage == DownloadStage::Ready) {
#if defined(_WIN32)
    job_->prepared.reset();
#endif
    job_->stage = DownloadStage::Cancelled;
  }
}

void InstallerDownload::Start(const std::string& version) {
  if (Active()) return;
  job_ = std::make_shared<Job>();
  job_->version = version;
  try {
    std::thread([job = job_, root = root_, version, metadata = metadata_, transfer = transfer_] {
      Run(job, root, version, metadata, transfer);
    }).detach();
  } catch (const std::system_error&) {
    job_->error = DownloadError::Storage;
    job_->stage = DownloadStage::Failed;
  }
}

void InstallerDownload::Run(const std::shared_ptr<Job>& job, const std::filesystem::path& root,
                           const std::string& version, const Checker::Fetch& metadata, const Transfer& transfer) {
  try {
    // Never execute based on the disk cache: obtain the URL, size and digest again over HTTPS.
    const auto payload = metadata();
    if (job->cancel) throw Cancelled{};
    const auto release = payload ? ParseRelease(*payload) : std::nullopt;
    if (!release || release->version != version || release->installer_sha256.empty() ||
        !release->installer_size || release->installer_url.empty()) throw DownloadError::Metadata;
    job->total = release->installer_size;
#if defined(_WIN32)
    auto prepared = std::make_shared<Prepared>();
    prepared->directory = NewDirectory(root);
    prepared->executable = prepared->directory / "NarutoRiseInstaller.exe";
    ULARGE_INTEGER free{};
    if (!GetDiskFreeSpaceExW(prepared->directory.c_str(), &free, nullptr, nullptr) ||
        free.QuadPart < release->installer_size + 16 * 1024 * 1024) throw DownloadError::Storage;
    {
      File output{CreateFileW(prepared->executable.c_str(), GENERIC_WRITE, 0, nullptr,
                              CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr)};
      if (output.value == INVALID_HANDLE_VALUE) throw DownloadError::Storage;
      transfer(release->installer_url, job->cancel, [&](std::span<const char> bytes) {
        if (job->cancel) throw Cancelled{};
        if (bytes.size() > release->installer_size - job->received.load()) throw DownloadError::Integrity;
        DWORD written = 0;
        if (!WriteFile(output.value, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) ||
            written != bytes.size()) throw DownloadError::Storage;
        job->received += bytes.size();
      });
      if (!FlushFileBuffers(output.value)) throw DownloadError::Storage;
    }
    if (job->cancel) throw Cancelled{};
    if (job->received != release->installer_size) throw DownloadError::Integrity;
    job->stage = DownloadStage::Verifying;
    // Reopen read-only before hashing and keep this handle until CreateProcess.
    // FILE_SHARE_READ prevents writes, renames or deletion after verification.
    prepared->lock.value = CreateFileW(prepared->executable.c_str(), GENERIC_READ, FILE_SHARE_READ,
                                     nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (prepared->lock.value == INVALID_HANDLE_VALUE) throw DownloadError::Storage;
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(prepared->lock.value, &size) || size.QuadPart != release->installer_size ||
        HashFile(prepared->lock.value, job->cancel) != release->installer_sha256) throw DownloadError::Integrity;
    std::lock_guard lock(job->mutex);
    if (job->cancel) throw Cancelled{};
    job->prepared = std::move(prepared);
    job->stage = DownloadStage::Ready;
#else
    throw DownloadError::Storage;
#endif
  } catch (const Cancelled&) {
    job->stage = DownloadStage::Cancelled;
  } catch (DownloadError error) {
    job->error = error;
    job->stage = job->cancel ? DownloadStage::Cancelled : DownloadStage::Failed;
  } catch (...) {
    job->error = DownloadError::Storage;
    job->stage = job->cancel ? DownloadStage::Cancelled : DownloadStage::Failed;
  }
}

InstallResult InstallerDownload::Install(const std::filesystem::path& directory, std::string_view language,
                                         bool allow_portable_conversion) {
  if (!job_ || job_->stage != DownloadStage::Ready || job_->cancel) return InstallResult::NotReady;
  if (!IsInstalledCopy(directory) && !allow_portable_conversion) return InstallResult::NotInstalled;
  std::error_code error;
  if (!std::filesystem::is_regular_file(directory / "narutorise.exe", error) || error ||
      !std::filesystem::is_regular_file(directory / "narutorise_launcher.exe", error) || error)
    return InstallResult::MissingFiles;
  if (!IsNewerRelease(AppVersion(directory), job_->version).value_or(false)) return InstallResult::NotReady;
  if (InstallationInUse(directory)) return InstallResult::GameRunning;
#if defined(_WIN32)
  std::lock_guard lock(job_->mutex);
  auto prepared = job_->prepared;
  if (!prepared || prepared->launched) return InstallResult::NotReady;
  auto command = InstallerCommandLine(prepared->executable, directory, language);
  STARTUPINFOW startup{}; startup.cb = sizeof(startup);
  PROCESS_INFORMATION process{};
  if (!CreateProcessW(prepared->executable.c_str(), command.data(), nullptr, nullptr, FALSE, 0,
                      nullptr, prepared->directory.c_str(), &startup, &process)) return InstallResult::Failed;
  CloseHandle(process.hThread);
  CloseHandle(process.hProcess);
  // Setup may reopen its own executable while extracting. Retain it after handoff.
  prepared->launched = true;
  return InstallResult::Started;
#else
  return InstallResult::Failed;
#endif
}
}  // namespace launcher_updates
