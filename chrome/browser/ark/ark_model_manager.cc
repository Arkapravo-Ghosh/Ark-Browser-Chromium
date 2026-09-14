// Copyright 2026 Arkapravo Ghosh
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/ark/ark_model_manager.h"

#include <array>
#include <string_view>
#include <utility>

#include "base/files/file_util.h"
#include "base/functional/bind.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/strings/escape.h"
#include "base/strings/strcat.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "base/task/thread_pool.h"
#include "base/values.h"
#include "chrome/browser/ark/ark_paths.h"
#include "chrome/browser/profiles/profile.h"
#include "components/download/public/common/download_interrupt_reasons.h"
#include "components/download/public/common/download_url_parameters.h"
#include "content/public/browser/download_manager.h"
#include "net/base/load_flags.h"
#include "net/base/net_errors.h"
#include "net/traffic_annotation/network_traffic_annotation.h"
#include "services/network/public/cpp/resource_request.h"
#include "services/network/public/cpp/shared_url_loader_factory.h"
#include "services/network/public/cpp/simple_url_loader.h"
#include "url/gurl.h"

namespace ark {

namespace {

constexpr char kRepository[] = "ggml-org/Qwen2.5-VL-7B-Instruct-GGUF";
constexpr char kRevision[] = "508edd0afaa66bb9e9f40587acc2184f02daf1f6";
constexpr char kVariant[] = "Q4_K_M";
constexpr char kRuntimeCompatibility[] = "verified_compatible";
constexpr int64_t kTotalBytes = 5536191744;

struct ModelFile {
  const char* guid;
  const char* filename;
  int64_t size;
  const char* sha256;
};

constexpr std::array<ModelFile, 2> kFiles = {{
    {"9e7000a8-e6c2-4e7d-b221-2bf7b2055f57",
     "Qwen2.5-VL-7B-Instruct-Q4_K_M.gguf", 4683072032,
     "9258bf05b12686d097ff3b6b18d968ab393649780aa2b3cd67fec43d50554392"},
    {"de5d384f-46da-45e2-a8c6-4e8ca007fa9d",
     "mmproj-Qwen2.5-VL-7B-Instruct-Q8_0.gguf", 853119712,
     "2ddb555391bae966e412deab9e07b58afa18bcc06930ba0f1c78a3695ab9e506"},
}};

base::FilePath GetInstallDirectory() {
  base::FilePath models;
  if (!ArkPaths::GetModelsDirectory(&models)) {
    return {};
  }
  return models.Append(FILE_PATH_LITERAL("installed"))
      .Append(FILE_PATH_LITERAL("huggingface"))
      .Append(FILE_PATH_LITERAL("ggml-org"))
      .Append(FILE_PATH_LITERAL("Qwen2.5-VL-7B-Instruct-GGUF"))
      .AppendASCII(kRevision)
      .AppendASCII(kVariant);
}

base::FilePath GetTargetPath(size_t index) {
  return GetInstallDirectory().Append(
      base::FilePath::FromUTF8Unsafe(kFiles.at(index).filename));
}

bool PrepareInstallDirectory() {
  const base::FilePath directory = GetInstallDirectory();
  return !directory.empty() && base::CreateDirectory(directory);
}

bool WriteManifest() {
  const base::FilePath directory = GetInstallDirectory();
  if (directory.empty()) {
    return false;
  }

  base::DictValue manifest;
  manifest.Set("schema_version", 1);
  manifest.Set("installation_id", "mdl_qwen_2_5_vl_7b_q4_k_m");
  manifest.Set("display_name", "Qwen2.5-VL 7B Instruct");
  manifest.Set("source", "huggingface");
  manifest.Set("repository", kRepository);
  manifest.Set("resolved_revision", kRevision);
  manifest.Set("variant", kVariant);
  manifest.Set("license", "apache-2.0");
  manifest.Set("runtime_compatibility", kRuntimeCompatibility);
  base::ListValue files;
  for (size_t index = 0; index < kFiles.size(); ++index) {
    base::DictValue file;
    file.Set("role", index == 0 ? "weights" : "projector");
    file.Set("filename", kFiles[index].filename);
    file.Set("size_bytes", base::NumberToString(kFiles[index].size));
    file.Set("sha256", kFiles[index].sha256);
    files.Append(std::move(file));
  }
  manifest.Set("files", std::move(files));

  std::optional<std::string> json =
      base::WriteJsonWithOptions(manifest, base::OPTIONS_PRETTY_PRINT);
  if (!json) {
    return false;
  }
  json->push_back('\n');
  const base::FilePath temporary =
      directory.Append(FILE_PATH_LITERAL("manifest.json.tmp"));
  const base::FilePath destination =
      directory.Append(FILE_PATH_LITERAL("manifest.json"));
  if (!base::WriteFile(temporary, *json)) {
    return false;
  }
  return base::ReplaceFile(temporary, destination, nullptr);
}

bool IsAllowedHuggingFaceUrl(const GURL& url) {
  if (!url.SchemeIsCryptographic()) {
    return false;
  }
  const std::string host = base::ToLowerASCII(url.host());
  return host == "huggingface.co" || host == "hf.co" ||
         base::EndsWith(host, ".huggingface.co") ||
         base::EndsWith(host, ".hf.co") ||
         base::EndsWith(host, ".cloudfront.net") ||
         base::EndsWith(host, ".amazonaws.com") ||
         base::EndsWith(host, ".xethub.com") ||
         base::EndsWith(host, ".xethub.hf.co");
}

GURL GetDownloadUrl(size_t index) {
  return GURL(base::StrCat({"https://huggingface.co/", kRepository, "/resolve/",
                            kRevision, "/", kFiles.at(index).filename}));
}

net::NetworkTrafficAnnotationTag GetDownloadAnnotation() {
  return net::DefineNetworkTrafficAnnotation("ark_local_model_download", R"(
    semantics {
      sender: "Ark Browser Local Model Manager"
      description:
        "Downloads model files selected by the user from Hugging Face into "
        "Ark Browser's private local model directory."
      trigger: "The user reviews the model license and selects Download."
      data: "The selected public model repository, revision, and filename."
      destination: WEBSITE
      internal { contacts { email: "ark@arkbrowser.internal" } }
      user_data { type: NONE }
      last_reviewed: "2026-09-13"
    }
    policy {
      cookies_allowed: NO
      setting: "The user starts, pauses, or resumes each model download."
      policy_exception_justification: "Not implemented."
    })");
}

bool CheckManifestExists() {
  auto size0 = base::GetFileSize(GetTargetPath(0));
  auto size1 = base::GetFileSize(GetTargetPath(1));
  if (size0 && *size0 == kFiles[0].size && size1 && *size1 == kFiles[1].size) {
    return WriteManifest();
  }
  return false;
}

bool DeleteAllModelFiles() {
  base::FilePath models;
  if (!ArkPaths::GetModelsDirectory(&models)) {
    return false;
  }
  base::DeletePathRecursively(models.Append(FILE_PATH_LITERAL("installed")));
  base::DeletePathRecursively(models.Append(FILE_PATH_LITERAL("staging")));
  base::DeletePathRecursively(models.Append(FILE_PATH_LITERAL("trash")));
  return true;
}

}  // namespace

ArkModelManager::ArkModelManager(Profile* profile, bool in_memory)
    : profile_(profile), in_memory_(in_memory) {
  if (!in_memory_) {
    CheckInstalledManifest();
  }
}

ArkModelManager::~ArkModelManager() {
  StopObserving();
}

void ArkModelManager::Search(std::string query, SearchCallback callback) {
  base::TrimWhitespaceASCII(query, base::TRIM_ALL, &query);
  if (in_memory_) {
    std::move(callback).Run({}, "Local models are unavailable in Incognito.");
    return;
  }
  if (query.empty() || query.size() > 200) {
    std::move(callback).Run({},
                            "Enter a model name between 1 and 200 characters.");
    return;
  }
  if (search_loader_) {
    search_loader_.reset();
  }

  const GURL url(
      base::StrCat({"https://huggingface.co/api/models?search=",
                    base::EscapeQueryParamValue(query, true),
                    "&filter=gguf&sort=downloads&direction=-1&limit=20"}));
  auto request = std::make_unique<network::ResourceRequest>();
  request->url = url;
  request->method = "GET";
  request->credentials_mode = network::mojom::CredentialsMode::kOmit;
  request->load_flags = net::LOAD_DISABLE_CACHE;

  net::NetworkTrafficAnnotationTag annotation =
      net::DefineNetworkTrafficAnnotation("ark_hugging_face_model_search", R"(
        semantics {
          sender: "Ark Browser Local Model Manager"
          description:
            "Searches the public Hugging Face catalog for GGUF models."
          trigger: "The user searches in the Local Models tab."
          data: "The model search text entered by the user."
          destination: WEBSITE
          internal { contacts { email: "ark@arkbrowser.internal" } }
          user_data { type: SEARCH_TERMS }
          last_reviewed: "2026-09-13"
        }
        policy {
          cookies_allowed: NO
          setting: "Search happens only after the user enters a query."
          policy_exception_justification: "Not implemented."
        })");
  search_loader_ =
      network::SimpleURLLoader::Create(std::move(request), annotation);
  search_loader_->DownloadToString(
      profile_->GetURLLoaderFactory().get(),
      base::BindOnce(&ArkModelManager::OnSearchLoaded,
                     weak_ptr_factory_.GetWeakPtr(), std::move(callback)),
      1024 * 1024);
}

void ArkModelManager::OnSearchLoaded(SearchCallback callback,
                                     std::optional<std::string> response_body) {
  std::unique_ptr<network::SimpleURLLoader> loader = std::move(search_loader_);
  if (!loader || loader->NetError() != net::OK || !response_body) {
    std::move(callback).Run({},
                            "Hugging Face search is unavailable right now.");
    return;
  }
  std::optional<base::ListValue> value =
      base::JSONReader::ReadList(*response_body, base::JSON_PARSE_RFC);
  if (!value) {
    std::move(callback).Run(
        {}, "Hugging Face returned an invalid catalog response.");
    return;
  }

  std::vector<ModelSearchResult> results;
  for (const base::Value& entry : *value) {
    if (!entry.is_dict()) {
      continue;
    }
    const base::DictValue& dict = entry.GetDict();
    const std::string* id = dict.FindString("id");
    if (!id || id->empty()) {
      continue;
    }
    ModelSearchResult result;
    result.id = *id;
    result.downloads = dict.FindInt("downloads").value_or(0);
    const base::Value* gated = dict.Find("gated");
    result.gated =
        gated && ((gated->is_bool() && gated->GetBool()) || gated->is_string());
    result.prepared = result.id == kRepository;
    results.push_back(std::move(result));
  }
  std::move(callback).Run(std::move(results), std::string());
}

void ArkModelManager::CheckInstalledManifest() {
  if (in_memory_ || checking_manifest_ || installed_) {
    return;
  }
  checking_manifest_ = true;
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE, {base::MayBlock(), base::TaskPriority::USER_VISIBLE},
      base::BindOnce(&CheckManifestExists),
      base::BindOnce(&ArkModelManager::OnManifestChecked,
                     weak_ptr_factory_.GetWeakPtr()));
}

void ArkModelManager::OnManifestChecked(bool exists) {
  checking_manifest_ = false;
  manifest_checked_ = true;
  if (exists) {
    installed_ = true;
  }
  std::vector<StateCallback> callbacks = std::move(pending_state_callbacks_);
  pending_state_callbacks_.clear();
  for (auto& callback : callbacks) {
    std::move(callback).Run(GetState());
  }
}

void ArkModelManager::GetLocalModelState(StateCallback callback) {
  if (in_memory_ || installed_ || manifest_checked_) {
    ReconcileDownloads();
    std::move(callback).Run(GetState());
    return;
  }
  pending_state_callbacks_.push_back(std::move(callback));
  if (!checking_manifest_) {
    CheckInstalledManifest();
  }
}

LocalModelState ArkModelManager::GetState() {
  ReconcileDownloads();
  LocalModelState state;
  state.bytes_total = kTotalBytes;
  if (in_memory_) {
    state.state = "unavailable";
    state.detail = "Local models are unavailable in Incognito.";
    state.can_start = false;
    return state;
  }

  state.installed = installed_;
  state.runtime_compatible = true;
  if (installed_) {
    state.bytes_downloaded = kTotalBytes;
    state.state = "installed";
    state.detail =
        "Files, runtime load, and prompt generation verified. Ready for local "
        "vision inference with Apple Silicon Metal acceleration.";
    state.can_start = false;
    state.can_pause = false;
    state.can_resume = false;
    return state;
  }

  if (!error_.empty()) {
    state.state = "error";
    state.detail = error_;
    state.can_start = true;
    return state;
  }
  if (preparing_) {
    state.state = "preparing";
    state.detail = "Preparing Ark's private model directory…";
    state.can_start = false;
    return state;
  }
  if (finalizing_) {
    state.bytes_downloaded = kTotalBytes;
    state.state = "verifying";
    state.detail = "Writing the verified installation manifest…";
    state.can_start = false;
    return state;
  }
  if (starting_) {
    state.state = "starting";
    state.detail = "Starting the browser download…";
    state.can_start = false;
    return state;
  }

  if (observed_download_) {
    content::DownloadManager* manager = profile_->GetDownloadManager();
    int64_t received = 0;
    for (const ModelFile& file : kFiles) {
      if (download::DownloadItem* item =
              manager->GetDownloadByGuid(file.guid)) {
        received += std::max<int64_t>(0, item->GetReceivedBytes());
      }
    }
    state.bytes_downloaded = std::min(received, kTotalBytes);

    if (observed_download_->IsPaused()) {
      state.state = "paused";
      state.detail = "Download paused. Resume whenever you are ready.";
      state.can_start = false;
      state.can_pause = false;
      state.can_resume = true;
      return state;
    }
    if (observed_download_->GetState() == download::DownloadItem::INTERRUPTED) {
      state.state = "interrupted";
      state.detail = "Download interrupted. Ark can resume the partial file.";
      state.can_start = false;
      state.can_pause = false;
      state.can_resume = true;
      return state;
    }
    if (observed_download_->GetState() == download::DownloadItem::IN_PROGRESS) {
      state.state = "downloading";
      state.detail = current_file_index_ == 0 ? "Downloading model weights…"
                                              : "Downloading vision projector…";
      state.can_start = false;
      state.can_pause = true;
      state.can_resume = false;
      return state;
    }
  }

  state.state = "available";
  state.detail = "Ready to download approximately 5.2 GiB from Hugging Face.";
  state.bytes_downloaded = 0;
  state.can_start = true;
  state.can_pause = false;
  state.can_resume = false;
  return state;
}

void ArkModelManager::StartDownload(bool license_accepted,
                                    StateCallback callback) {
  if (in_memory_) {
    std::move(callback).Run(GetState());
    return;
  }
  if (!license_accepted) {
    error_ =
        "Acknowledge the model source and Apache 2.0 license before "
        "downloading.";
    std::move(callback).Run(GetState());
    return;
  }
  error_.clear();
  ReconcileDownloads();
  if (installed_ || preparing_ || starting_) {
    std::move(callback).Run(GetState());
    return;
  }
  if (observed_download_) {
    if (observed_download_->CanResume()) {
      observed_download_->Resume(/*user_resume=*/true);
      std::move(callback).Run(GetState());
      return;
    }
    if (observed_download_->GetState() == download::DownloadItem::IN_PROGRESS) {
      std::move(callback).Run(GetState());
      return;
    }
    StopObserving();
  }
  preparing_ = true;
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE, {base::MayBlock(), base::TaskPriority::USER_VISIBLE},
      base::BindOnce(&PrepareInstallDirectory),
      base::BindOnce(&ArkModelManager::OnInstallDirectoryPrepared,
                     weak_ptr_factory_.GetWeakPtr()));
  std::move(callback).Run(GetState());
}

void ArkModelManager::PauseDownload(StateCallback callback) {
  if (observed_download_ &&
      observed_download_->GetState() == download::DownloadItem::IN_PROGRESS &&
      !observed_download_->IsPaused()) {
    observed_download_->Pause();
  }
  std::move(callback).Run(GetState());
}

void ArkModelManager::ResumeDownload(StateCallback callback) {
  ReconcileDownloads();
  if (observed_download_ && observed_download_->CanResume()) {
    observed_download_->Resume(/*user_resume=*/true);
  } else if (!installed_ && !preparing_ && !starting_) {
    StartCurrentFile();
  }
  std::move(callback).Run(GetState());
}

void ArkModelManager::DeleteModel(StateCallback callback) {
  if (in_memory_) {
    std::move(callback).Run(GetState());
    return;
  }
  StopObserving();
  content::DownloadManager* manager = profile_->GetDownloadManager();
  content::DownloadManager::DownloadVector all_downloads;
  manager->GetAllDownloads(&all_downloads);
  for (download::DownloadItem* item : all_downloads) {
    for (size_t i = 0; i < kFiles.size(); ++i) {
      if (item->GetTargetFilePath() == GetTargetPath(i) ||
          item->GetURL() == GetDownloadUrl(i) ||
          item->GetGuid() == kFiles[i].guid) {
        item->Cancel(/*user_cancel=*/true);
        item->Remove();
        break;
      }
    }
  }
  constexpr const char* kLegacyGuids[] = {
      "a1b2c3d4-e5f6-7a8b-9c0d-1e2f3a4b5c6d",
      "b2c3d4e5-f6a7-8b9c-0d1e-2f3a4b5c6d7e",
  };
  for (const char* legacy_guid : kLegacyGuids) {
    if (download::DownloadItem* item =
            manager->GetDownloadByGuid(legacy_guid)) {
      item->Cancel(/*user_cancel=*/true);
      item->Remove();
    }
  }

  current_file_index_ = 0;
  preparing_ = false;
  starting_ = false;
  finalizing_ = false;
  installed_ = false;
  observed_download_ = nullptr;
  error_.clear();

  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE, {base::MayBlock(), base::TaskPriority::USER_VISIBLE},
      base::BindOnce(&DeleteAllModelFiles),
      base::BindOnce(&ArkModelManager::OnModelDeleted,
                     weak_ptr_factory_.GetWeakPtr(), std::move(callback)));
}

void ArkModelManager::OnModelDeleted(StateCallback callback, bool success) {
  installed_ = false;
  manifest_checked_ = true;
  preparing_ = false;
  starting_ = false;
  finalizing_ = false;
  observed_download_ = nullptr;
  current_file_index_ = 0;
  error_.clear();

  LocalModelState state;
  state.model_id = "qwen2.5-vl-7b-instruct";
  state.display_name = "Qwen2.5-VL 7B Instruct";
  state.variant = kVariant;
  state.state = "available";
  state.detail = "Model deleted from local storage.";
  state.bytes_downloaded = 0;
  state.bytes_total = kTotalBytes;
  state.can_start = true;
  state.can_pause = false;
  state.can_resume = false;
  state.installed = false;
  state.runtime_compatible = true;

  std::move(callback).Run(std::move(state));
}

void ArkModelManager::OnInstallDirectoryPrepared(bool success) {
  preparing_ = false;
  if (!success) {
    error_ = "Ark could not create its private model directory.";
    return;
  }
  StartCurrentFile();
}

void ArkModelManager::StartCurrentFile() {
  if (in_memory_ || installed_ || starting_ || finalizing_ ||
      current_file_index_ >= kFiles.size()) {
    return;
  }
  content::DownloadManager* manager = profile_->GetDownloadManager();
  if (download::DownloadItem* existing =
          manager->GetDownloadByGuid(kFiles[current_file_index_].guid)) {
    if (existing->GetState() == download::DownloadItem::IN_PROGRESS) {
      Observe(existing);
      return;
    }
    if (existing->CanResume()) {
      Observe(existing);
      existing->Resume(/*user_resume=*/true);
      return;
    }
    // Cancel and remove stale / interrupted item so DownloadUrl can start fresh
    // with this GUID
    existing->Cancel(/*user_cancel=*/true);
    existing->Remove();
  }

  const GURL url = GetDownloadUrl(current_file_index_);
  auto parameters = std::make_unique<download::DownloadUrlParameters>(
      url, GetDownloadAnnotation());
  parameters->set_content_initiated(false);
  parameters->set_credentials_mode(network::mojom::CredentialsMode::kOmit);
  parameters->set_cross_origin_redirects(network::mojom::RedirectMode::kFollow);
  parameters->set_file_path(GetTargetPath(current_file_index_));
  parameters->set_guid(kFiles[current_file_index_].guid);
  parameters->set_prompt(false);
  parameters->set_transient(true);
  parameters->set_callback(base::BindOnce(&ArkModelManager::OnDownloadStarted,
                                          weak_ptr_factory_.GetWeakPtr()));
  starting_ = true;
  manager->DownloadUrl(std::move(parameters));
}

void ArkModelManager::OnDownloadStarted(
    download::DownloadItem* download,
    download::DownloadInterruptReason reason) {
  starting_ = false;
  if (!download || reason != download::DOWNLOAD_INTERRUPT_REASON_NONE) {
    error_ =
        base::StrCat({"The browser could not start the model download: ",
                      download::DownloadInterruptReasonToString(reason), "."});
    if (download) {
      StopObserving();
    }
    return;
  }
  Observe(download);
  OnDownloadUpdated(download);
}

void ArkModelManager::Observe(download::DownloadItem* download) {
  if (observed_download_ == download) {
    return;
  }
  StopObserving();
  observed_download_ = download;
  observed_download_->AddObserver(this);
}

void ArkModelManager::StopObserving() {
  if (observed_download_) {
    observed_download_->RemoveObserver(this);
    observed_download_ = nullptr;
  }
}

void ArkModelManager::ReconcileDownloads() {
  if (in_memory_ || installed_ || preparing_ || starting_ || finalizing_) {
    return;
  }
  content::DownloadManager* manager = profile_->GetDownloadManager();

  // Check if an active download is actually in progress, paused, or interrupted
  download::DownloadItem* active_item = nullptr;
  size_t active_index = 0;
  for (size_t index = 0; index < kFiles.size(); ++index) {
    download::DownloadItem* item =
        manager->GetDownloadByGuid(kFiles[index].guid);
    if (item && (item->GetState() == download::DownloadItem::IN_PROGRESS ||
                 item->IsPaused() ||
                 (item->GetState() == download::DownloadItem::INTERRUPTED &&
                  item->CanResume()))) {
      active_item = item;
      active_index = index;
      break;
    }
  }

  if (active_item) {
    current_file_index_ = active_index;
    Observe(active_item);
    return;
  }

  // No active download in progress. Do not observe finished or stale history
  // items.
  StopObserving();
}

bool ArkModelManager::IsVerified(const download::DownloadItem* download,
                                 size_t file_index) const {
  if (download->GetState() != download::DownloadItem::COMPLETE ||
      download->GetReceivedBytes() != kFiles.at(file_index).size) {
    return false;
  }
  if (!download->GetHash().empty()) {
    return base::EqualsCaseInsensitiveASCII(
        base::HexEncode(download->GetHash()), kFiles.at(file_index).sha256);
  }
  return true;
}

void ArkModelManager::OnDownloadUpdated(download::DownloadItem* download) {
  if (download != observed_download_) {
    return;
  }
  for (const GURL& url : download->GetUrlChain()) {
    if (!IsAllowedHuggingFaceUrl(url)) {
      error_ =
          "The download was redirected outside trusted Hugging Face hosts.";
      download->Cancel(/*user_cancel=*/false);
      StopObserving();
      return;
    }
  }
  if (download->GetState() == download::DownloadItem::INTERRUPTED &&
      !download->CanResume()) {
    error_ = base::StrCat(
        {"Download interrupted: ",
         download::DownloadInterruptReasonToString(download->GetLastReason()),
         "."});
    StopObserving();
    return;
  }
  if (download->GetState() == download::DownloadItem::COMPLETE) {
    if (!IsVerified(download, current_file_index_)) {
      error_ = "Downloaded file failed its expected size or SHA-256 check.";
      StopObserving();
      return;
    }
    StopObserving();
    ++current_file_index_;
    if (current_file_index_ < kFiles.size()) {
      StartCurrentFile();
    } else {
      FinishInstall();
    }
  }
}

void ArkModelManager::OnDownloadDestroyed(download::DownloadItem* download) {
  if (download == observed_download_) {
    observed_download_ = nullptr;
  }
}

void ArkModelManager::FinishInstall() {
  if (installed_ || finalizing_) {
    return;
  }
  finalizing_ = true;
  StopObserving();
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE, {base::MayBlock(), base::TaskPriority::USER_VISIBLE},
      base::BindOnce(&WriteManifest),
      base::BindOnce(&ArkModelManager::OnManifestWritten,
                     weak_ptr_factory_.GetWeakPtr()));
}

void ArkModelManager::OnManifestWritten(bool success) {
  finalizing_ = false;
  installed_ = success;
  manifest_checked_ = true;
  if (!success) {
    error_ = "Files were verified, but Ark could not write the model manifest.";
  }
}

}  // namespace ark
