// Copyright 2026 Arkapravo Ghosh
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_ARK_ARK_MODEL_MANAGER_H_
#define CHROME_BROWSER_ARK_ARK_MODEL_MANAGER_H_

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "base/functional/callback_forward.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "components/download/public/common/download_item.h"

class Profile;

namespace network {
class SimpleURLLoader;
}

namespace ark {

struct ModelSearchResult {
  std::string id;
  int64_t downloads = 0;
  bool gated = false;
  bool prepared = false;
};

struct LocalModelState {
  std::string model_id = "qwen2.5-vl-7b-instruct";
  std::string display_name = "Qwen2.5-VL 7B Instruct";
  std::string variant = "Q4_K_M";
  std::string state = "available";
  std::string detail;
  double bytes_downloaded = 0;
  double bytes_total = 0;
  bool can_start = true;
  bool can_pause = false;
  bool can_resume = false;
  bool installed = false;
  bool runtime_compatible = true;
};

// Browser-owned Hugging Face discovery and local model download coordinator.
// Model bytes are streamed by Chromium's DownloadManager directly into Ark's
// private data tree; the WebUI never receives file handles or credentials.
class ArkModelManager : public download::DownloadItem::Observer {
 public:
  using SearchCallback =
      base::OnceCallback<void(std::vector<ModelSearchResult>, std::string)>;
  using StateCallback = base::OnceCallback<void(LocalModelState)>;

  ArkModelManager(Profile* profile, bool in_memory);
  ~ArkModelManager() override;

  void Search(std::string query, SearchCallback callback);
  void GetLocalModelState(StateCallback callback);
  LocalModelState GetState();
  void StartDownload(bool license_accepted, StateCallback callback);
  void PauseDownload(StateCallback callback);
  void ResumeDownload(StateCallback callback);
  void DeleteModel(StateCallback callback);

  // download::DownloadItem::Observer:
  void OnDownloadUpdated(download::DownloadItem* download) override;
  void OnDownloadDestroyed(download::DownloadItem* download) override;

 private:
  void CheckInstalledManifest();
  void OnManifestChecked(bool exists);
  void OnSearchLoaded(SearchCallback callback,
                      std::optional<std::string> response_body);
  void OnInstallDirectoryPrepared(bool success);
  void StartCurrentFile();
  void OnDownloadStarted(download::DownloadItem* download,
                         download::DownloadInterruptReason reason);
  void Observe(download::DownloadItem* download);
  void StopObserving();
  void ReconcileDownloads();
  bool IsVerified(const download::DownloadItem* download,
                  size_t file_index) const;
  void FinishInstall();
  void OnManifestWritten(bool success);
  void OnModelDeleted(StateCallback callback, bool success);

  const raw_ptr<Profile> profile_;
  const bool in_memory_;
  std::unique_ptr<network::SimpleURLLoader> search_loader_;
  raw_ptr<download::DownloadItem> observed_download_ = nullptr;
  size_t current_file_index_ = 0;
  bool preparing_ = false;
  bool starting_ = false;
  bool finalizing_ = false;
  bool installed_ = false;
  bool checking_manifest_ = false;
  bool manifest_checked_ = false;
  std::vector<StateCallback> pending_state_callbacks_;
  std::string error_;
  base::WeakPtrFactory<ArkModelManager> weak_ptr_factory_{this};
};

}  // namespace ark

#endif  // CHROME_BROWSER_ARK_ARK_MODEL_MANAGER_H_
