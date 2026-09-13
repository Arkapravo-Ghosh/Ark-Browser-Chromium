// Copyright 2026 Arkapravo Ghosh
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/ui/webui/help/version_updater_ark.h"

#include <array>
#include <cctype>
#include <optional>
#include <utility>
#include <vector>

#include "base/command_line.h"
#include "base/files/file.h"
#include "base/files/file_enumerator.h"
#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/files/scoped_temp_dir.h"
#include "base/functional/bind.h"
#include "base/json/json_reader.h"
#include "base/logging.h"
#include "base/memory/scoped_refptr.h"
#include "base/path_service.h"
#include "base/process/launch.h"
#include "base/process/process.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/thread_pool.h"
#include "base/values.h"
#include "build/build_config.h"
#include "chrome/browser/browser_process.h"
#include "chrome/common/ark_version_constants.h"
#include "content/public/browser/browser_thread.h"
#include "content/public/browser/web_contents.h"
#include "crypto/secure_hash.h"
#include "crypto/sha2.h"
#include "net/base/load_flags.h"
#include "net/traffic_annotation/network_traffic_annotation.h"
#include "services/network/public/cpp/resource_request.h"
#include "services/network/public/cpp/shared_url_loader_factory.h"
#include "services/network/public/cpp/simple_url_loader.h"
#include "services/network/public/mojom/fetch_api.mojom.h"
#include "url/gurl.h"

#if BUILDFLAG(IS_MAC)
#include "base/apple/bundle_locations.h"
#endif

namespace {

#if BUILDFLAG(IS_MAC)
#if defined(ARCH_CPU_ARM64)
constexpr char kTargetPlatform[] = "mac_arm64";
#else
// mac_x64 is not supported for Ark Browser releases.
constexpr char kTargetPlatform[] = "";
#endif
#elif BUILDFLAG(IS_WIN)
#if defined(ARCH_CPU_ARM64)
constexpr char kTargetPlatform[] = "win_arm64";
#else
constexpr char kTargetPlatform[] = "win_x64";
#endif
#elif BUILDFLAG(IS_LINUX)
constexpr char kTargetPlatform[] = "linux";
#else
constexpr char kTargetPlatform[] = "";
#endif

constexpr net::NetworkTrafficAnnotationTag kTrafficAnnotation =
    net::DefineNetworkTrafficAnnotation("ark_update_check", R"(
      semantics {
        sender: "Ark Browser Update Checker"
        description:
          "Checks whether an updated release of Ark Browser is available "
          "by querying the release manifest and downloading updates."
        trigger:
          "User navigates to the About Ark Browser page or initiates an update check."
        data: "None"
        destination: OTHER
        user_data {
          type: NONE
        }
        internal {
          contacts {
            email: "arkapravoghosh2004@gmail.com"
          }
        }
        last_reviewed: "2026-09-13"
      }
      policy {
        cookies_allowed: NO
        setting: "This feature cannot be disabled."
        chrome_policy {}
      })");

std::string ComputeFileSHA256(const base::FilePath& file_path) {
  std::unique_ptr<crypto::SecureHash> hash(
      crypto::SecureHash::Create(crypto::SecureHash::SHA256));
  base::File file(file_path, base::File::FLAG_OPEN | base::File::FLAG_READ);
  if (!file.IsValid()) {
    return "";
  }
  std::vector<uint8_t> buffer(64 * 1024);
  while (true) {
    std::optional<size_t> bytes_read = file.ReadAtCurrentPos(buffer);
    if (!bytes_read.has_value() || *bytes_read == 0) {
      break;
    }
    hash->Update(base::span(buffer).first(*bytes_read));
  }
  std::array<uint8_t, crypto::kSHA256Length> digest;
  hash->Finish(digest);
  return base::HexEncodeLower(digest);
}

}  // namespace

VersionUpdaterArk::VersionUpdaterArk(content::WebContents* web_contents)
    : web_contents_(web_contents) {}

VersionUpdaterArk::~VersionUpdaterArk() = default;

#if BUILDFLAG(IS_MAC)
void VersionUpdaterArk::PromoteUpdater() {}
#endif

std::vector<uint32_t> VersionUpdaterArk::ExtractVersionComponents(
    std::string_view version_str) {
  std::vector<uint32_t> components;
  uint32_t current_num = 0;
  bool in_number = false;

  for (char ch : version_str) {
    if (std::isdigit(static_cast<unsigned char>(ch))) {
      current_num = current_num * 10 + (ch - '0');
      in_number = true;
    } else {
      if (in_number) {
        components.push_back(current_num);
        current_num = 0;
        in_number = false;
      }
    }
  }
  if (in_number) {
    components.push_back(current_num);
  }
  return components;
}

int VersionUpdaterArk::CompareVersions(std::string_view v1, std::string_view v2) {
  std::vector<uint32_t> c1 = ExtractVersionComponents(v1);
  std::vector<uint32_t> c2 = ExtractVersionComponents(v2);

  size_t max_len = std::max(c1.size(), c2.size());
  for (size_t i = 0; i < max_len; ++i) {
    uint32_t num1 = (i < c1.size()) ? c1[i] : 0;
    uint32_t num2 = (i < c2.size()) ? c2[i] : 0;
    if (num1 < num2) {
      return -1;
    }
    if (num1 > num2) {
      return 1;
    }
  }
  return 0;
}

void VersionUpdaterArk::CheckForUpdate(StatusCallback status_callback,
                                      PromoteCallback /*promote_callback*/) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);

#if BUILDFLAG(IS_LINUX)
  // Linux updates are managed via native package managers; do not check or show errors.
  status_callback.Run(DISABLED, 0, false, false, "", 0, u"");
  return;
#elif BUILDFLAG(IS_MAC) && !defined(ARCH_CPU_ARM64)
  // mac_x64 is not supported for Ark Browser releases.
  status_callback.Run(DISABLED, 0, false, false, "", 0, u"");
  return;
#endif

  // Notify WebUI that checking has begun.
  status_callback.Run(CHECKING, 0, false, false, "", 0, u"");

  std::string manifest_url = ark::kArkDefaultUpdateManifestURL;
  const base::CommandLine* cmd_line = base::CommandLine::ForCurrentProcess();
  if (cmd_line->HasSwitch("ark-update-manifest-url")) {
    manifest_url = cmd_line->GetSwitchValueASCII("ark-update-manifest-url");
  }

  auto request = std::make_unique<network::ResourceRequest>();
  request->url = GURL(manifest_url);
  request->load_flags = net::LOAD_DISABLE_CACHE | net::LOAD_BYPASS_CACHE;
  request->credentials_mode = network::mojom::CredentialsMode::kOmit;
  request->priority = net::IDLE;

  scoped_refptr<network::SharedURLLoaderFactory> url_loader_factory;
  if (g_browser_process) {
    url_loader_factory = g_browser_process->shared_url_loader_factory();
  }

  if (!url_loader_factory) {
    status_callback.Run(
        FAILED, 0, false, false, "", 0,
        u"Network service unavailable for update check.");
    return;
  }

  url_loader_ = network::SimpleURLLoader::Create(std::move(request),
                                                 kTrafficAnnotation);
  url_loader_->SetRetryOptions(
      /*max_retries=*/2,
      network::SimpleURLLoader::RetryMode::RETRY_ON_NETWORK_CHANGE);

  network::SimpleURLLoader* raw_loader = url_loader_.get();
  raw_loader->DownloadToString(
      url_loader_factory.get(),
      base::BindOnce(&VersionUpdaterArk::OnManifestLoaded,
                     weak_factory_.GetWeakPtr(), status_callback,
                     std::move(url_loader_)),
      /*max_body_size=*/512 * 1024);
}

void VersionUpdaterArk::OnManifestLoaded(
    StatusCallback status_callback,
    std::unique_ptr<network::SimpleURLLoader> loader,
    std::optional<std::string> response_body) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);

  if (!response_body || loader->NetError() != net::OK) {
    status_callback.Run(
        FAILED, 0, false, false, "", 0,
        u"Could not connect to update servers. Check your internet connection.");
    return;
  }

  std::optional<base::DictValue> root =
      base::JSONReader::ReadDict(*response_body,
                                 base::JSON_PARSE_CHROMIUM_EXTENSIONS);
  if (!root) {
    status_callback.Run(
        FAILED, 0, false, false, "", 0,
        u"Invalid update manifest format received.");
    return;
  }

  const std::string* remote_version_str = root->FindString("version");
  if (!remote_version_str || remote_version_str->empty()) {
    status_callback.Run(
        FAILED, 0, false, false, "", 0,
        u"Update manifest is missing release version.");
    return;
  }

  const std::string* channel_str = root->FindString("channel");
  if (channel_str && !channel_str->empty() && *channel_str != ark::kArkChannel) {
    status_callback.Run(UPDATED, 0, false, false, "", 0, u"");
    return;
  }

  const base::DictValue* platforms = root->FindDict("platforms");
  const base::DictValue* platform_info =
      platforms ? platforms->FindDict(kTargetPlatform) : nullptr;

  const std::string* download_url =
      platform_info ? platform_info->FindString("url") : nullptr;

  // If no download URL is specified for this platform yet, gracefully treat as up to date.
  if (!download_url || download_url->empty()) {
    status_callback.Run(UPDATED, 0, false, false, "", 0, u"");
    return;
  }

  int64_t asset_size = 0;
  if (platform_info) {
    if (std::optional<int> size_int = platform_info->FindInt("size")) {
      asset_size = *size_int;
    } else if (std::optional<double> size_double = platform_info->FindDouble("size")) {
      asset_size = static_cast<int64_t>(*size_double);
    }
  }

  int cmp = CompareVersions(*remote_version_str, ark::kArkVersion);
  if (cmp <= 0) {
    // Current browser version is up to date!
    status_callback.Run(UPDATED, 0, false, false, "", 0, u"");
  } else {
    // A newer version is available.
    // Begin the background download and staged installation process.
    status_callback_ = status_callback;
    target_version_ = *remote_version_str;
    expected_size_ = asset_size;
    target_sha256_ = platform_info ? (platform_info->FindString("sha256") ? *platform_info->FindString("sha256") : "") : "";

    StartDownload(*download_url);
  }
}

void VersionUpdaterArk::StartDownload(const std::string& download_url) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);

  // Notify WebUI of initial download progress.
  status_callback_.Run(UPDATING, 0, false, false, target_version_,
                       expected_size_, u"");

  auto request = std::make_unique<network::ResourceRequest>();
  request->url = GURL(download_url);
  request->load_flags = net::LOAD_DISABLE_CACHE | net::LOAD_BYPASS_CACHE;
  request->credentials_mode = network::mojom::CredentialsMode::kOmit;
  request->priority = net::MEDIUM;

  scoped_refptr<network::SharedURLLoaderFactory> url_loader_factory;
  if (g_browser_process) {
    url_loader_factory = g_browser_process->shared_url_loader_factory();
  }

  if (!url_loader_factory) {
    status_callback_.Run(
        FAILED, 0, false, false, "", 0,
        u"Network service unavailable for downloading update.");
    return;
  }

  download_loader_ = network::SimpleURLLoader::Create(std::move(request),
                                                      kTrafficAnnotation);
  download_loader_->SetRetryOptions(
      /*max_retries=*/2,
      network::SimpleURLLoader::RetryMode::RETRY_ON_NETWORK_CHANGE);
  download_loader_->SetOnDownloadProgressCallback(
      base::BindRepeating(&VersionUpdaterArk::OnDownloadProgress,
                          weak_factory_.GetWeakPtr()));
  download_loader_->DownloadToTempFile(
      url_loader_factory.get(),
      base::BindOnce(&VersionUpdaterArk::OnDownloadComplete,
                     weak_factory_.GetWeakPtr()));
}

void VersionUpdaterArk::OnDownloadProgress(uint64_t current_bytes) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);

  int progress = 0;
  if (expected_size_ > 0) {
    progress = static_cast<int>((current_bytes * 100) / expected_size_);
    if (progress > 98) {
      progress = 98;  // Reserve 99-100% for extraction & verification
    }
  }

  status_callback_.Run(UPDATING, progress, false, false, target_version_,
                       expected_size_, u"");
}

void VersionUpdaterArk::OnDownloadComplete(base::FilePath temp_file_path) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);

  if (!download_loader_ || download_loader_->NetError() != net::OK ||
      temp_file_path.empty()) {
    status_callback_.Run(FAILED, 0, false, false, "", 0,
                         u"Failed to download update package.");
    return;
  }

  // Update WebUI progress to 99% (Verifying and staging)
  status_callback_.Run(UPDATING, 99, false, false, target_version_,
                       expected_size_, u"");

  base::FilePath target_bundle;
#if BUILDFLAG(IS_MAC)
  target_bundle = base::apple::OuterBundlePath();
#endif

  // Run the disk verification, mounting, extraction, and staging on a worker thread.
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE, {base::MayBlock(), base::TaskPriority::USER_VISIBLE},
      base::BindOnce(
          &VersionUpdaterArk::ExtractAndStageUpdateOnBackgroundThread,
          temp_file_path, target_sha256_, target_bundle),
      base::BindOnce(&VersionUpdaterArk::OnStageComplete,
                     weak_factory_.GetWeakPtr()));
}

std::string VersionUpdaterArk::ExtractAndStageUpdateOnBackgroundThread(
    base::FilePath temp_dmg_path,
    std::string expected_sha256,
    base::FilePath target_bundle) {
#if BUILDFLAG(IS_MAC)
  // 1. Verify SHA-256 Checksum
  std::string computed_hash = ComputeFileSHA256(temp_dmg_path);
  if (!expected_sha256.empty() &&
      !base::EqualsCaseInsensitiveASCII(computed_hash, expected_sha256)) {
    base::DeleteFile(temp_dmg_path);
    return "SHA-256 checksum mismatch on downloaded update package.";
  }

  // 2. Create temporary mount point directory
  base::ScopedTempDir mount_temp_dir;
  if (!mount_temp_dir.CreateUniqueTempDir()) {
    base::DeleteFile(temp_dmg_path);
    return "Failed to create temporary directory for DMG mount.";
  }
  base::FilePath mount_point = mount_temp_dir.GetPath();

  // 3. Attach DMG quietly via hdiutil
  std::vector<std::string> attach_argv = {
      "/usr/bin/hdiutil", "attach", "-nobrowse", "-readonly",
      temp_dmg_path.value(), "-mountpoint", mount_point.value()};
  base::CommandLine attach_cmd(attach_argv);
  int attach_exit = 0;
  base::Process attach_proc =
      base::LaunchProcess(attach_cmd, base::LaunchOptions());
  if (!attach_proc.IsValid() || !attach_proc.WaitForExit(&attach_exit) ||
      attach_exit != 0) {
    base::DeleteFile(temp_dmg_path);
    return "Failed to attach update disk image.";
  }

  // 4. Find the .app bundle inside the mounted volume
  base::FileEnumerator enumerator(mount_point, false,
                                  base::FileEnumerator::DIRECTORIES, "*.app");
  base::FilePath source_app = enumerator.Next();

  if (source_app.empty()) {
    base::CommandLine detach_cmd({"/usr/bin/hdiutil", "detach", mount_point.value(), "-force"});
    int detach_exit = 0;
    base::Process detach_proc = base::LaunchProcess(detach_cmd, base::LaunchOptions());
    if (detach_proc.IsValid()) {
      detach_proc.WaitForExit(&detach_exit);
    }
    base::DeleteFile(temp_dmg_path);
    return "No application bundle found inside update disk image.";
  }

  // 5. Locate staging folder in Application Support
  base::FilePath staged_dir;
  if (!base::PathService::Get(base::DIR_APP_DATA, &staged_dir)) {
    staged_dir = base::GetHomeDir().Append("Library").Append("Application Support").Append("Ark Browser");
  }
  staged_dir = staged_dir.Append("StagedUpdate");
  base::DeletePathRecursively(staged_dir);
  base::CreateDirectory(staged_dir);
  base::FilePath staged_app = staged_dir.Append(source_app.BaseName());

  // Copy via /usr/bin/ditto to preserve code signatures, extended attributes, and symlinks
  std::vector<std::string> ditto_argv = {"/usr/bin/ditto", source_app.value(),
                                         staged_app.value()};
  base::CommandLine ditto_cmd(ditto_argv);
  int ditto_exit = 0;
  base::Process ditto_proc =
      base::LaunchProcess(ditto_cmd, base::LaunchOptions());
  bool ditto_ok =
      ditto_proc.IsValid() && ditto_proc.WaitForExit(&ditto_exit) && ditto_exit == 0;

  // 6. Detach DMG and cleanup temp file
  base::CommandLine detach_cmd({"/usr/bin/hdiutil", "detach", mount_point.value(), "-force"});
  int detach_exit = 0;
  base::Process detach_proc = base::LaunchProcess(detach_cmd, base::LaunchOptions());
  if (detach_proc.IsValid()) {
    detach_proc.WaitForExit(&detach_exit);
  }
  base::DeleteFile(temp_dmg_path);

  if (!ditto_ok) {
    base::DeletePathRecursively(staged_dir);
    return "Failed to copy staged application bundle.";
  }

  // 7. Atomic Swap into Target Bundle
  // Determine target bundle path: OuterBundlePath or /Applications/Ark Browser.app
  base::FilePath default_apps_bundle("/Applications/Ark Browser.app");

  base::FilePath install_destination = target_bundle;
  if (!base::PathExists(install_destination) && base::PathExists(default_apps_bundle)) {
    install_destination = default_apps_bundle;
  }

  base::FilePath backup_path = install_destination.DirName().Append(
      install_destination.BaseName().value() + ".old");
  base::DeletePathRecursively(backup_path);

  bool swapped = false;
  if (base::Move(install_destination, backup_path)) {
    if (base::Move(staged_app, install_destination)) {
      base::DeletePathRecursively(backup_path);
      swapped = true;
    } else {
      // Restore on failure
      base::Move(backup_path, install_destination);
    }
  }

  if (!swapped) {
    // Fallback: direct ditto replacement into destination
    std::vector<std::string> install_ditto_argv = {
        "/usr/bin/ditto", staged_app.value(), install_destination.value()};
    base::CommandLine install_ditto_cmd(install_ditto_argv);
    int install_exit = 0;
    base::Process install_proc =
        base::LaunchProcess(install_ditto_cmd, base::LaunchOptions());
    if (install_proc.IsValid() && install_proc.WaitForExit(&install_exit) &&
        install_exit == 0) {
      swapped = true;
    }
  }

  base::DeletePathRecursively(staged_dir);

  if (!swapped) {
    return "Failed to install update into destination (" +
           install_destination.value() + "). Check folder permissions.";
  }

  return "";  // Success
#else
  return "Automatic in-place update is not supported on this platform.";
#endif
}

void VersionUpdaterArk::OnStageComplete(std::string error_message) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);

  if (!error_message.empty()) {
    status_callback_.Run(FAILED, 0, false, false, "", 0,
                         base::UTF8ToUTF16(error_message));
    return;
  }

  // Update has been successfully downloaded, verified, and installed!
  // Emitting NEARLY_UPDATED displays the success badge and [Relaunch] button.
  // When the user clicks [Relaunch], the relaunched app will run the new binary.
  status_callback_.Run(NEARLY_UPDATED, 0, false, false, target_version_,
                       expected_size_, u"");
}
