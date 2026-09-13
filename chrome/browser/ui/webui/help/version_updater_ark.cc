// Copyright 2026 Arkapravo Ghosh
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/ui/webui/help/version_updater_ark.h"

#include <cctype>
#include <optional>
#include <utility>

#include "base/command_line.h"
#include "base/functional/bind.h"
#include "base/json/json_reader.h"
#include "base/logging.h"
#include "base/memory/scoped_refptr.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/utf_string_conversions.h"
#include "base/values.h"
#include "build/build_config.h"
#include "chrome/browser/browser_process.h"
#include "chrome/common/ark_version_constants.h"
#include "content/public/browser/browser_thread.h"
#include "content/public/browser/web_contents.h"
#include "net/base/load_flags.h"
#include "net/traffic_annotation/network_traffic_annotation.h"
#include "services/network/public/cpp/resource_request.h"
#include "services/network/public/cpp/shared_url_loader_factory.h"
#include "services/network/public/cpp/simple_url_loader.h"
#include "services/network/public/mojom/fetch_api.mojom.h"
#include "url/gurl.h"

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
          "by querying the release manifest."
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

  if (!response_body) {
    status_callback.Run(
        FAILED, 0, false, false, "", 0,
        u"Could not connect to the update server. Please check your internet connection.");
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
    // NEARLY_UPDATED displays the success badge and [Relaunch] button on settings/help.
    status_callback.Run(NEARLY_UPDATED, 0, false, false, *remote_version_str,
                        asset_size, u"");
  }
}
