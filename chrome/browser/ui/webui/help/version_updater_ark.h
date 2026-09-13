// Copyright 2026 Arkapravo Ghosh
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_UI_WEBUI_HELP_VERSION_UPDATER_ARK_H_
#define CHROME_BROWSER_UI_WEBUI_HELP_VERSION_UPDATER_ARK_H_

#include <memory>
#include <string>
#include <string_view>
#include "base/files/file_path.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "build/build_config.h"
#include "chrome/browser/ui/webui/help/version_updater.h"

namespace content {
class WebContents;
}

namespace network {
class SimpleURLLoader;
}

// Ark Browser native version updater.
// Checks remote release manifest (JSON) on GitHub Releases / CDN,
// handles platform filtering, downloads DMG in background with live progress %,
// verifies SHA-256, extracts & stages the update, and swaps binaries on relaunch.
class VersionUpdaterArk : public VersionUpdater {
 public:
  explicit VersionUpdaterArk(content::WebContents* web_contents);
  VersionUpdaterArk(const VersionUpdaterArk&) = delete;
  VersionUpdaterArk& operator=(const VersionUpdaterArk&) = delete;
  ~VersionUpdaterArk() override;

  // VersionUpdater implementation.
  void CheckForUpdate(StatusCallback status_callback,
                      PromoteCallback promote_callback) override;

#if BUILDFLAG(IS_MAC)
  void PromoteUpdater() override;
#endif

  // Parses dotted or semantic version like "155.0.8049.0-alpha.0.0.1" into
  // integer components for comparison.
  static std::vector<uint32_t> ExtractVersionComponents(std::string_view version_str);

  // Returns -1 if v1 < v2, 0 if v1 == v2, 1 if v1 > v2.
  static int CompareVersions(std::string_view v1, std::string_view v2);

 private:
  void OnManifestLoaded(StatusCallback status_callback,
                        std::unique_ptr<network::SimpleURLLoader> loader,
                        std::optional<std::string> response_body);

  void StartDownload(const std::string& download_url);
  void OnDownloadProgress(uint64_t current_bytes);
  void OnDownloadComplete(base::FilePath temp_file_path);
  void OnStageComplete(std::string error_message);

  static std::string ExtractAndStageUpdateOnBackgroundThread(
      base::FilePath temp_dmg_path,
      std::string expected_sha256,
      base::FilePath target_bundle);

  raw_ptr<content::WebContents> web_contents_;
  std::unique_ptr<network::SimpleURLLoader> url_loader_;
  std::unique_ptr<network::SimpleURLLoader> download_loader_;

  StatusCallback status_callback_;
  std::string target_version_;
  std::string target_sha256_;
  int64_t expected_size_ = 0;

  base::WeakPtrFactory<VersionUpdaterArk> weak_factory_{this};
};

#endif  // CHROME_BROWSER_UI_WEBUI_HELP_VERSION_UPDATER_ARK_H_
