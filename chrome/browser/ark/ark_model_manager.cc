// Copyright 2026 Arkapravo Ghosh
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/ark/ark_model_manager.h"

#include <array>
#include <algorithm>
#include <string_view>
#include <utility>

#include "base/files/file_util.h"
#include "base/files/file_enumerator.h"
#include "base/functional/bind.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/strings/escape.h"
#include "base/strings/strcat.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "base/task/thread_pool.h"
#include "base/uuid.h"
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

constexpr char kRepository[] =
    "mlx-community/Llama-3.2-11B-Vision-Instruct-4bit";
constexpr char kRevision[] = "82f31be9840fa0d4c7e99257fe2e28b59a46df97";
constexpr char kVariant[] = "MLX-4bit";
constexpr char kRuntimeBackend[] = "mlx-vlm";
constexpr char kRuntimeCompatibility[] =
    "supported_by_bundled_mlx_vlm_0_5_0";
constexpr int64_t kTotalBytes = 6024911143;

std::vector<LocalModelFile> PreparedFiles() {
  return {
      {"d294f1dc-7083-44d3-8ecc-dd8452856d7a", "config",
       "chat_template.json", 558, ""},
      {"1dd07b5d-3394-4389-be08-c20d008781a6", "config",
       "config.json", 6184, ""},
      {"11a1edbc-7533-4319-ab7b-f1bd74cd1750", "weights",
       "model-00001-of-00002.safetensors", 5349710085,
       "15c251bc77860ca2c4b61a73e86960a803e69f32b7a2ddf81f6046e9ad1e1841"},
      {"f68a6ae7-e95a-456d-a6a4-283e052f8a4e", "weights",
       "model-00002-of-00002.safetensors", 657728351,
       "681031d989d5e62b06a1db29fcb65540760cae006062254a68c67554cfb81de8"},
      {"5a207549-fd73-4d48-a79f-673861dd4bda", "config",
       "model.safetensors.index.json", 200270, ""},
      {"5bec89cf-3589-4de6-bc0a-76b9c2e33710", "config",
       "preprocessor_config.json", 477, ""},
      {"59e7810f-c716-4427-b758-b35344e8155c", "tokenizer",
       "special_tokens_map.json", 454, ""},
      {"5603b601-bc90-43e9-bc12-1672df8d8444", "tokenizer",
       "tokenizer.json", 17208880,
       "47be6519609d58a5f29b3497045b8a2798d0d0978955ea90a893ad80e2ecdd4d"},
      {"eab19583-0051-4164-bcbc-a19d082b80e1", "tokenizer",
       "tokenizer_config.json", 55884, ""},
  };
}

bool SplitRepository(const std::string& repository,
                     std::string* owner,
                     std::string* name) {
  const size_t slash = repository.find('/');
  if (slash == std::string::npos || slash == 0 ||
      slash + 1 >= repository.size() ||
      repository.find('/', slash + 1) != std::string::npos) {
    return false;
  }
  *owner = repository.substr(0, slash);
  *name = repository.substr(slash + 1);
  auto safe = [](const std::string& value) {
    return !value.empty() && std::ranges::all_of(value, [](char c) {
      return base::IsAsciiAlphaNumeric(c) || c == '-' || c == '_' || c == '.';
    }) && value != "." && value != "..";
  };
  return safe(*owner) && safe(*name);
}

std::string ModelIdFor(const std::string& repository,
                       const std::string& revision,
                       const std::string& variant) {
  if (repository == kRepository) {
    return "local:mlx:llama-3.2-11b-vision-instruct";
  }
  return base::StrCat({"local:huggingface:", repository, "@", revision, ":",
                       variant});
}

base::FilePath GetInstallDirectory(const std::string& repository,
                                   const std::string& revision,
                                   const std::string& variant) {
  base::FilePath models;
  if (!ArkPaths::GetModelsDirectory(&models)) {
    return {};
  }
  std::string owner;
  std::string name;
  if (!SplitRepository(repository, &owner, &name)) {
    return {};
  }
  return models.Append(FILE_PATH_LITERAL("installed"))
      .Append(FILE_PATH_LITERAL("huggingface"))
      .Append(base::FilePath::FromUTF8Unsafe(owner))
      .Append(base::FilePath::FromUTF8Unsafe(name))
      .AppendASCII(revision)
      .AppendASCII(variant);
}

base::FilePath GetTargetPath(const std::string& repository,
                             const std::string& revision,
                             const std::string& variant,
                             const LocalModelFile& file) {
  return GetInstallDirectory(repository, revision, variant)
      .Append(base::FilePath::FromUTF8Unsafe(file.filename));
}

bool PrepareInstallDirectory(std::string repository,
                             std::string revision,
                             std::string variant) {
  const base::FilePath directory =
      GetInstallDirectory(repository, revision, variant);
  return !directory.empty() && base::CreateDirectory(directory);
}

bool WriteManifest(std::string repository,
                   std::string revision,
                   std::string variant,
                   std::string model_id,
                   std::string display_name,
                   std::string license,
                   std::string runtime_backend,
                   std::string runtime_compatibility,
                   std::vector<LocalModelFile> model_files) {
  const base::FilePath directory =
      GetInstallDirectory(repository, revision, variant);
  if (directory.empty()) {
    return false;
  }

  base::DictValue manifest;
  manifest.Set("schema_version", 1);
  manifest.Set("installation_id", model_id);
  manifest.Set("model_id", model_id);
  manifest.Set("display_name", display_name);
  manifest.Set("source", "huggingface");
  manifest.Set("repository", repository);
  manifest.Set("resolved_revision", revision);
  manifest.Set("variant", variant);
  manifest.Set("license", license);
  manifest.Set("runtime_backend", runtime_backend);
  manifest.Set("runtime_compatibility", runtime_compatibility);
  base::ListValue files;
  for (const LocalModelFile& model_file : model_files) {
    base::DictValue file;
    file.Set("role", model_file.role);
    file.Set("filename", model_file.filename);
    file.Set("size_bytes", base::NumberToString(model_file.size));
    if (!model_file.sha256.empty()) {
      file.Set("sha256", model_file.sha256);
    }
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

GURL GetDownloadUrl(const std::string& repository,
                    const std::string& revision,
                    const LocalModelFile& file) {
  return GURL(base::StrCat({"https://huggingface.co/", repository, "/resolve/",
                            revision, "/", file.filename}));
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
  const std::vector<LocalModelFile> files = PreparedFiles();
  const bool complete = std::ranges::all_of(files, [](const LocalModelFile& file) {
    const std::optional<int64_t> size = base::GetFileSize(
        GetTargetPath(kRepository, kRevision, kVariant, file));
    return size && *size == file.size;
  });
  if (complete) {
    return WriteManifest(kRepository, kRevision, kVariant,
                         "local:mlx:llama-3.2-11b-vision-instruct",
                         "Llama 3.2 11B Vision Instruct", "llama3.2",
                         kRuntimeBackend, kRuntimeCompatibility, files);
  }
  return false;
}

std::vector<InstalledLocalModel> ScanInstalledModels() {
  std::vector<InstalledLocalModel> models;
  base::FilePath root;
  if (!ArkPaths::GetModelsDirectory(&root)) {
    return models;
  }
  root = root.Append(FILE_PATH_LITERAL("installed"));
  base::FileEnumerator files(root, /*recursive=*/true,
                             base::FileEnumerator::FILES);
  for (base::FilePath path = files.Next(); !path.empty(); path = files.Next()) {
    if (path.BaseName() != base::FilePath(FILE_PATH_LITERAL("manifest.json"))) {
      continue;
    }
    std::string json;
    std::optional<base::DictValue> manifest;
    if (!base::ReadFileToString(path, &json) ||
        !(manifest = base::JSONReader::ReadDict(json, base::JSON_PARSE_RFC))) {
      continue;
    }
    const std::string* repository = manifest->FindString("repository");
    const std::string* revision = manifest->FindString("resolved_revision");
    const std::string* variant = manifest->FindString("variant");
    const std::string* display_name = manifest->FindString("display_name");
    if (!repository || !revision || !variant || !display_name) {
      continue;
    }
    InstalledLocalModel model;
    model.repository = *repository;
    model.revision = *revision;
    model.variant = *variant;
    model.display_name = *display_name;
    const std::string* runtime_backend =
        manifest->FindString("runtime_backend");
    model.runtime_backend = runtime_backend ? *runtime_backend : "llama.cpp";
    const std::string* stored_id = manifest->FindString("model_id");
    model.model_id = stored_id ? *stored_id
                               : ModelIdFor(*repository, *revision, *variant);
    const std::string* compatibility_value =
        manifest->FindString("runtime_compatibility");
    const std::string compatibility =
        compatibility_value ? *compatibility_value : std::string();
    model.runtime_compatible =
        compatibility == kRuntimeCompatibility ||
        compatibility == "supported_by_bundled_mlx_vlm_0_5_0" ||
        compatibility == "supported_by_bundled_llama_cpp_0_4_0" ||
        compatibility == "downloaded_unverified" ||
        compatibility == "downloaded_unverified_mlx";
    if (const base::ListValue* entries = manifest->FindList("files")) {
      for (const base::Value& value : *entries) {
        const base::DictValue* entry = value.GetIfDict();
        const std::string* size = entry ? entry->FindString("size_bytes") : nullptr;
        int64_t parsed = 0;
        if (size && base::StringToInt64(*size, &parsed)) {
          model.bytes_total += parsed;
        }
      }
    }
    models.push_back(std::move(model));
  }
  std::ranges::sort(models, {}, &InstalledLocalModel::display_name);
  return models;
}

bool DeleteInstalledModel(std::string model_id) {
  base::FilePath root;
  if (!ArkPaths::GetModelsDirectory(&root)) {
    return false;
  }
  root = root.Append(FILE_PATH_LITERAL("installed"));
  base::FileEnumerator files(root, /*recursive=*/true,
                             base::FileEnumerator::FILES);
  for (base::FilePath path = files.Next(); !path.empty(); path = files.Next()) {
    if (path.BaseName() != base::FilePath(FILE_PATH_LITERAL("manifest.json"))) {
      continue;
    }
    std::string json;
    std::optional<base::DictValue> manifest;
    if (!base::ReadFileToString(path, &json) ||
        !(manifest = base::JSONReader::ReadDict(json, base::JSON_PARSE_RFC))) {
      continue;
    }
    const std::string* repository = manifest->FindString("repository");
    const std::string* revision = manifest->FindString("resolved_revision");
    const std::string* variant = manifest->FindString("variant");
    if (!repository || !revision || !variant) {
      continue;
    }
    const std::string* stored_id = manifest->FindString("model_id");
    const std::string candidate =
        stored_id ? *stored_id : ModelIdFor(*repository, *revision, *variant);
    if (candidate == model_id) {
      return base::DeletePathRecursively(path.DirName());
    }
  }
  return false;
}

bool RuntimeSupportsArchitecture(std::string architecture) {
  architecture = base::ToLowerASCII(architecture);
  constexpr std::string_view kSupported[] = {
      "llama", "qwen2", "qwen2vl", "qwen3", "gemma", "gemma2",
      "gemma3", "gemma4", "mistral", "mixtral", "phi2", "phi3", "phi4",
      "deepseek2", "command-r", "starcoder2", "stablelm"};
  return std::ranges::find(kSupported, architecture) != std::end(kSupported);
}

std::string DisplayNameForRepository(const std::string& repository) {
  const size_t slash = repository.find('/');
  std::string name = slash == std::string::npos ? repository
                                                : repository.substr(slash + 1);
  if (base::EndsWith(name, "-GGUF", base::CompareCase::INSENSITIVE_ASCII)) {
    name.resize(name.size() - 5);
  }
  std::replace(name.begin(), name.end(), '-', ' ');
  return name;
}

}  // namespace

ArkModelManager::ArkModelManager(Profile* profile, bool in_memory)
    : profile_(profile), in_memory_(in_memory) {
  ResetToPreparedModel();
  if (!in_memory_) {
    CheckInstalledManifest();
  }
}

void ArkModelManager::ResetToPreparedModel() {
  repository_ = kRepository;
  revision_ = kRevision;
  variant_ = kVariant;
  model_id_ = "local:mlx:llama-3.2-11b-vision-instruct";
  display_name_ = "Llama 3.2 11B Vision Instruct";
  license_ = "llama3.2";
  runtime_backend_ = kRuntimeBackend;
  runtime_compatibility_ = kRuntimeCompatibility;
  files_ = PreparedFiles();
  total_bytes_ = kTotalBytes;
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

  const GURL url(base::StrCat(
      {"https://huggingface.co/api/models?search=",
       base::EscapeQueryParamValue(query, true),
       "&sort=downloads&direction=-1&limit=100&full=true"}));
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
            "Searches the public Hugging Face catalog for MLX and GGUF models."
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
    const base::ListValue* tags = dict.FindList("tags");
    bool is_mlx = false;
    bool is_gguf = false;
    if (tags) {
      for (const base::Value& tag : *tags) {
        const std::string* tag_value = tag.GetIfString();
        if (!tag_value) {
          continue;
        }
        is_mlx |= base::EqualsCaseInsensitiveASCII(*tag_value, "mlx");
        is_gguf |= base::EqualsCaseInsensitiveASCII(*tag_value, "gguf");
      }
    }
    if (!is_mlx && !is_gguf) {
      continue;
    }
    result.runtime_backend = is_mlx ? "mlx-vlm" : "llama.cpp";
    result.downloads = dict.FindInt("downloads").value_or(0);
    const base::Value* gated = dict.Find("gated");
    result.gated =
        gated && ((gated->is_bool() && gated->GetBool()) || gated->is_string());
    result.prepared = result.id == kRepository;
    results.push_back(std::move(result));
  }
  std::ranges::stable_sort(results, [](const ModelSearchResult& left,
                                      const ModelSearchResult& right) {
    return left.runtime_backend == "mlx-vlm" &&
           right.runtime_backend != "mlx-vlm";
  });
  if (results.size() > 20) {
    results.resize(20);
  }
  std::move(callback).Run(std::move(results), std::string());
}

void ArkModelManager::GetInstalledModels(InstalledCallback callback) {
  if (in_memory_) {
    std::move(callback).Run({});
    return;
  }
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE, {base::MayBlock(), base::TaskPriority::USER_VISIBLE},
      base::BindOnce(&ScanInstalledModels), std::move(callback));
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
  state.model_id = model_id_;
  state.display_name = display_name_;
  state.variant = variant_;
  state.runtime_backend = runtime_backend_;
  state.bytes_total = total_bytes_;
  if (in_memory_) {
    state.state = "unavailable";
    state.detail = "Local models are unavailable in Incognito.";
    state.can_start = false;
    return state;
  }

  state.installed = installed_;
  state.runtime_compatible = runtime_compatibility_ == kRuntimeCompatibility ||
                             runtime_compatibility_ ==
                                 "supported_by_bundled_mlx_vlm_0_5_0" ||
                             runtime_compatibility_ ==
                                 "supported_by_bundled_llama_cpp_0_4_0" ||
                             runtime_compatibility_ == "downloaded_unverified" ||
                             runtime_compatibility_ ==
                                 "downloaded_unverified_mlx";
  if (installed_) {
    state.bytes_downloaded = total_bytes_;
    state.state = "installed";
    state.detail =
        "Installed for local inference with Apple Silicon Metal acceleration.";
    state.can_start = false;
    state.can_pause = false;
    state.can_resume = false;
    return state;
  }

  if (!error_.empty()) {
    state.state = "error";
    state.detail = error_;
    state.can_start = state.runtime_compatible;
    return state;
  }
  if (preparing_) {
    state.state = "preparing";
    state.detail = "Preparing Ark's private model directory…";
    state.can_start = false;
    return state;
  }
  if (finalizing_) {
    state.bytes_downloaded = total_bytes_;
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
    for (const LocalModelFile& file : files_) {
      if (download::DownloadItem* item =
              manager->GetDownloadByGuid(file.guid)) {
        received += std::max<int64_t>(0, item->GetReceivedBytes());
      }
    }
    state.bytes_downloaded = std::min(received, total_bytes_);

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
    state.detail = total_bytes_ > 0
                       ? "Ready to download the recommended Apple Silicon model from Hugging Face."
                       : "Choose an MLX or legacy GGUF model from search results.";
  state.bytes_downloaded = 0;
  state.can_start = true;
  state.can_pause = false;
  state.can_resume = false;
  return state;
}

void ArkModelManager::StartDownload(std::string repository,
                                    bool license_accepted,
                                    StateCallback callback) {
  if (in_memory_) {
    std::move(callback).Run(GetState());
    return;
  }
  base::TrimWhitespaceASCII(repository, base::TRIM_ALL, &repository);
  std::string owner;
  std::string name;
  if (!SplitRepository(repository, &owner, &name)) {
    error_ = "Choose a valid Hugging Face repository (owner/name).";
    std::move(callback).Run(GetState());
    return;
  }
  if (!license_accepted) {
    error_ = "Acknowledge the model source and license before downloading.";
    std::move(callback).Run(GetState());
    return;
  }
  error_.clear();
  ReconcileDownloads();
  if (preparing_ || starting_ || finalizing_ || observed_download_) {
    error_ = "Finish or pause the current model download first.";
    std::move(callback).Run(GetState());
    return;
  }
  installed_ = false;
  repository_ = repository;
  display_name_ = DisplayNameForRepository(repository);
  model_id_ = base::StrCat({"local:huggingface:", repository});
  revision_.clear();
  variant_.clear();
  files_.clear();
  total_bytes_ = 0;
  runtime_compatibility_.clear();
  runtime_backend_.clear();
  preparing_ = true;
  auto request = std::make_unique<network::ResourceRequest>();
  request->url = GURL(base::StrCat(
      {"https://huggingface.co/api/models/", repository, "?blobs=true"}));
  request->method = "GET";
  request->credentials_mode = network::mojom::CredentialsMode::kOmit;
  request->load_flags = net::LOAD_DISABLE_CACHE;
  metadata_loader_ = network::SimpleURLLoader::Create(
      std::move(request), GetDownloadAnnotation());
  metadata_loader_->DownloadToString(
      profile_->GetURLLoaderFactory().get(),
      base::BindOnce(&ArkModelManager::OnModelMetadataLoaded,
                     weak_ptr_factory_.GetWeakPtr(), std::move(callback)),
      4 * 1024 * 1024);
}

void ArkModelManager::OnModelMetadataLoaded(
    StateCallback callback,
    std::optional<std::string> response_body) {
  std::unique_ptr<network::SimpleURLLoader> loader = std::move(metadata_loader_);
  if (!loader || loader->NetError() != net::OK || !response_body) {
    preparing_ = false;
    error_ = "Ark could not inspect this Hugging Face repository.";
    std::move(callback).Run(GetState());
    return;
  }
  std::optional<base::DictValue> metadata =
      base::JSONReader::ReadDict(*response_body, base::JSON_PARSE_RFC);
  const std::string* revision = metadata ? metadata->FindString("sha") : nullptr;
  const base::ListValue* siblings = metadata ? metadata->FindList("siblings") : nullptr;
  const base::DictValue* gguf = metadata ? metadata->FindDict("gguf") : nullptr;
  const std::string* architecture_value =
      gguf ? gguf->FindString("architecture") : nullptr;
  const std::string architecture =
      architecture_value ? *architecture_value : std::string();
  if (!revision || revision->empty() || !siblings) {
    preparing_ = false;
    error_ = "This repository has no immutable model file metadata.";
    std::move(callback).Run(GetState());
    return;
  }
  const base::DictValue* card = metadata->FindDict("cardData");
  const std::string* license = card ? card->FindString("license") : nullptr;
  license_ = license ? *license : "unknown";
  revision_ = *revision;
  bool is_mlx = false;
  if (const base::ListValue* tags = metadata->FindList("tags")) {
    for (const base::Value& tag : *tags) {
      const std::string* value = tag.GetIfString();
      is_mlx |= value && base::EqualsCaseInsensitiveASCII(*value, "mlx");
    }
  }
  const base::DictValue* config = metadata->FindDict("config");
  const std::string* model_type = config ? config->FindString("model_type") : nullptr;
  if (is_mlx) {
    runtime_backend_ = "mlx-vlm";
    runtime_compatibility_ = model_type && *model_type == "mllama"
                                 ? "supported_by_bundled_mlx_vlm_0_5_0"
                                 : "downloaded_unverified_mlx";
    const std::string lower_repository = base::ToLowerASCII(repository_);
    if (lower_repository.find("4bit") != std::string::npos ||
        lower_repository.find("4-bit") != std::string::npos) {
      variant_ = "MLX-4bit";
    } else if (lower_repository.find("8bit") != std::string::npos ||
               lower_repository.find("8-bit") != std::string::npos) {
      variant_ = "MLX-8bit";
    } else {
      variant_ = "MLX";
    }
    bool has_weights = false;
    bool has_config = false;
    for (const base::Value& value : *siblings) {
      const base::DictValue* item = value.GetIfDict();
      const std::string* filename = item ? item->FindString("rfilename") : nullptr;
      const std::optional<double> item_size =
          item ? item->FindDouble("size") : std::nullopt;
      if (!filename || !item_size || *item_size <= 0 ||
          filename->find('/') != std::string::npos) {
        continue;
      }
      const std::string lower = base::ToLowerASCII(*filename);
      const bool weights = base::EndsWith(
          lower, ".safetensors", base::CompareCase::SENSITIVE);
      const bool required_metadata =
          base::EndsWith(lower, ".json", base::CompareCase::SENSITIVE) ||
          base::EndsWith(lower, ".model", base::CompareCase::SENSITIVE) ||
          base::EndsWith(lower, ".txt", base::CompareCase::SENSITIVE);
      if (!weights && !required_metadata) {
        continue;
      }
      const base::DictValue* lfs = item->FindDict("lfs");
      const std::string* sha256 = lfs ? lfs->FindString("sha256") : nullptr;
      const std::string role = weights ? "weights" :
          lower.find("token") != std::string::npos ||
                  lower == "vocab.json" || lower == "merges.txt"
              ? "tokenizer"
              : "config";
      files_.push_back({base::Uuid::GenerateRandomV4().AsLowercaseString(),
                        role, *filename, static_cast<int64_t>(*item_size),
                        sha256 ? *sha256 : std::string()});
      total_bytes_ += static_cast<int64_t>(*item_size);
      has_weights |= weights;
      has_config |= lower == "config.json";
    }
    if (!has_weights || !has_config) {
      preparing_ = false;
      files_.clear();
      total_bytes_ = 0;
      error_ = "This MLX repository is missing weights or config.json.";
      std::move(callback).Run(GetState());
      return;
    }
    model_id_ = ModelIdFor(repository_, revision_, variant_);
    base::ThreadPool::PostTaskAndReplyWithResult(
        FROM_HERE, {base::MayBlock(), base::TaskPriority::USER_VISIBLE},
        base::BindOnce(&PrepareInstallDirectory, repository_, revision_, variant_),
        base::BindOnce(&ArkModelManager::OnInstallDirectoryPrepared,
                       weak_ptr_factory_.GetWeakPtr()));
    std::move(callback).Run(GetState());
    return;
  }
  runtime_backend_ = "llama.cpp";
  runtime_compatibility_ = RuntimeSupportsArchitecture(architecture)
                               ? "supported_by_bundled_llama_cpp_0_4_0"
                               : "downloaded_unverified";
  struct Candidate {
    std::string filename;
    int64_t size = 0;
    std::string sha256;
    int rank = 99;
  };
  std::vector<Candidate> weights;
  std::vector<Candidate> projectors;
  for (const base::Value& value : *siblings) {
    const base::DictValue* item = value.GetIfDict();
    const std::string* filename = item ? item->FindString("rfilename") : nullptr;
    const base::DictValue* lfs = item ? item->FindDict("lfs") : nullptr;
    const std::string* hash = lfs ? lfs->FindString("sha256") : nullptr;
    const std::optional<double> size_value = lfs ? lfs->FindDouble("size") : std::nullopt;
    if (!filename || !hash || !size_value || *size_value <= 0 ||
        filename->find('/') != std::string::npos ||
        !base::EndsWith(*filename, ".gguf", base::CompareCase::INSENSITIVE_ASCII)) {
      continue;
    }
    const std::string lower = base::ToLowerASCII(*filename);
    Candidate candidate{*filename, static_cast<int64_t>(*size_value), *hash, 99};
    if (lower.find("q4_k_m") != std::string::npos) candidate.rank = 0;
    else if (lower.find("q4_k_s") != std::string::npos) candidate.rank = 1;
    else if (lower.find("q4_0") != std::string::npos) candidate.rank = 2;
    else if (lower.find("q5_k_m") != std::string::npos) candidate.rank = 3;
    else if (lower.find("q8_0") != std::string::npos) candidate.rank = 4;
    else if (lower.find("f16") != std::string::npos) candidate.rank = 5;
    (lower.find("mmproj") != std::string::npos ? projectors : weights)
        .push_back(std::move(candidate));
  }
  if (weights.empty()) {
    preparing_ = false;
    error_ = "No complete, verifiable GGUF weight file was found.";
    std::move(callback).Run(GetState());
    return;
  }
  auto choose = [](std::vector<Candidate>& candidates) -> Candidate {
    return *std::min_element(candidates.begin(), candidates.end(),
                             [](const Candidate& a, const Candidate& b) {
                               return a.rank != b.rank ? a.rank < b.rank
                                                       : a.size < b.size;
                             });
  };
  const Candidate weight = choose(weights);
  variant_ = weight.rank == 0 ? "Q4_K_M" :
             weight.rank == 1 ? "Q4_K_S" :
             weight.rank == 2 ? "Q4_0" :
             weight.rank == 3 ? "Q5_K_M" :
             weight.rank == 4 ? "Q8_0" : "F16";
  files_.push_back({base::Uuid::GenerateRandomV4().AsLowercaseString(),
                    "weights", weight.filename, weight.size, weight.sha256});
  total_bytes_ = weight.size;
  if (!projectors.empty()) {
    const Candidate projector = choose(projectors);
    files_.push_back({base::Uuid::GenerateRandomV4().AsLowercaseString(),
                      "projector", projector.filename, projector.size,
                      projector.sha256});
    total_bytes_ += projector.size;
  }
  model_id_ = ModelIdFor(repository_, revision_, variant_);
  preparing_ = true;
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE, {base::MayBlock(), base::TaskPriority::USER_VISIBLE},
      base::BindOnce(&PrepareInstallDirectory, repository_, revision_, variant_),
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

void ArkModelManager::DeleteModel(std::string model_id,
                                  StateCallback callback) {
  if (in_memory_) {
    std::move(callback).Run(GetState());
    return;
  }
  StopObserving();
  content::DownloadManager* manager = profile_->GetDownloadManager();
  content::DownloadManager::DownloadVector all_downloads;
  manager->GetAllDownloads(&all_downloads);
  for (download::DownloadItem* item : all_downloads) {
    for (const LocalModelFile& file : files_) {
      if (item->GetTargetFilePath() ==
              GetTargetPath(repository_, revision_, variant_, file) ||
          item->GetURL() == GetDownloadUrl(repository_, revision_, file) ||
          item->GetGuid() == file.guid) {
        item->Cancel(/*user_cancel=*/true);
        item->Remove();
        break;
      }
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
      base::BindOnce(&DeleteInstalledModel, std::move(model_id)),
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

  const std::string deleted_name = display_name_;
  ResetToPreparedModel();
  LocalModelState state;
  state.model_id = model_id_;
  state.display_name = display_name_;
  state.variant = variant_;
  state.state = "available";
  state.detail = success ? base::StrCat({deleted_name, " deleted from local storage."})
                         : "Ark could not find that installed model.";
  state.bytes_downloaded = 0;
  state.bytes_total = total_bytes_;
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
      current_file_index_ >= files_.size()) {
    return;
  }
  content::DownloadManager* manager = profile_->GetDownloadManager();
  if (download::DownloadItem* existing =
          manager->GetDownloadByGuid(files_[current_file_index_].guid)) {
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

  const LocalModelFile& file = files_[current_file_index_];
  const GURL url = GetDownloadUrl(repository_, revision_, file);
  auto parameters = std::make_unique<download::DownloadUrlParameters>(
      url, GetDownloadAnnotation());
  parameters->set_content_initiated(false);
  parameters->set_credentials_mode(network::mojom::CredentialsMode::kOmit);
  parameters->set_cross_origin_redirects(network::mojom::RedirectMode::kFollow);
  parameters->set_file_path(
      GetTargetPath(repository_, revision_, variant_, file));
  parameters->set_guid(file.guid);
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
  for (size_t index = 0; index < files_.size(); ++index) {
    download::DownloadItem* item =
        manager->GetDownloadByGuid(files_[index].guid);
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
      download->GetReceivedBytes() != files_.at(file_index).size) {
    return false;
  }
  if (!download->GetHash().empty()) {
    if (files_.at(file_index).sha256.empty()) {
      return true;
    }
    return base::EqualsCaseInsensitiveASCII(
        base::HexEncode(download->GetHash()), files_.at(file_index).sha256);
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
    if (current_file_index_ < files_.size()) {
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
      base::BindOnce(&WriteManifest, repository_, revision_, variant_,
                     model_id_, display_name_, license_,
                     runtime_backend_, runtime_compatibility_, files_),
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
