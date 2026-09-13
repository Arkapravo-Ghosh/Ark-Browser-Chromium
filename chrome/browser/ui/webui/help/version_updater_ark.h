// Copyright 2026 Arkapravo Ghosh
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_UI_WEBUI_HELP_VERSION_UPDATER_ARK_H_
#define CHROME_BROWSER_UI_WEBUI_HELP_VERSION_UPDATER_ARK_H_

#include <memory>
#include <string>
#include <string_view>
#include <vector>

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
// handles platform filtering (macOS arm64 only, Windows open, Linux disabled),
// and informs the WebUI About/Help page.
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

  raw_ptr<content::WebContents> web_contents_;
  std::unique_ptr<network::SimpleURLLoader> url_loader_;
  base::WeakPtrFactory<VersionUpdaterArk> weak_factory_{this};
};

#endif  // CHROME_BROWSER_UI_WEBUI_HELP_VERSION_UPDATER_ARK_H_
