// Copyright 2026 Arkapravo Ghosh
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/ark/ark_inference_service.h"

#include <utility>

#include "base/base_paths.h"
#include "base/command_line.h"
#include "base/files/file_enumerator.h"
#include "base/files/file_util.h"
#include "base/functional/bind.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/path_service.h"
#include "base/process/launch.h"
#include "base/strings/strcat.h"
#include "base/strings/string_number_conversions.h"
#include "base/task/thread_pool.h"
#include "base/time/time.h"
#include "base/values.h"
#include "chrome/browser/ark/ark_paths.h"
#include "chrome/browser/profiles/profile.h"
#include "net/base/load_flags.h"
#include "net/base/net_errors.h"
#include "net/traffic_annotation/network_traffic_annotation.h"
#include "services/network/public/cpp/resource_request.h"
#include "services/network/public/cpp/shared_url_loader_factory.h"
#include "services/network/public/cpp/simple_url_loader.h"
#include "url/gurl.h"

namespace ark {

namespace {

constexpr char kVerifiedCompatibility[] = "verified_compatible";
constexpr char kBundledRuntimeCompatibility[] =
    "supported_by_bundled_llama_cpp_0_4_0";
constexpr char kUnsupportedVisionRepository[] =
    "leafspark/Llama-3.2-11B-Vision-Instruct-GGUF";
constexpr size_t kMaximumQueuedPrompts = 8;

net::NetworkTrafficAnnotationTag GetInferenceAnnotation() {
  return net::DefineNetworkTrafficAnnotation("ark_local_model_inference", R"(
    semantics {
      sender: "Ark Browser Local Model Inference Service"
      description:
        "Sends chat prompts to the bundled on-device llama-server process "
        "running locally on Apple Silicon Metal."
      trigger: "The user sends a message in Chat or Ask AI."
      data: "The conversation messages and optional image attachments."
      destination: LOCAL
      internal { contacts { email: "ark@arkbrowser.internal" } }
      user_data { type: NONE }
      last_reviewed: "2026-09-14"
    }
    policy {
      cookies_allowed: NO
      setting: "Runs entirely on localhost without external network traffic."
      policy_exception_justification: "Localhost process communication only."
    })");
}

// These helpers perform filesystem I/O and must only run on a MayBlock worker.
base::FilePath GetRuntimeExecutablePath() {
  base::FilePath exe_dir;
  if (base::PathService::Get(base::DIR_EXE, &exe_dir)) {
    const base::FilePath bundled =
        exe_dir.Append(FILE_PATH_LITERAL("llama-server"));
    if (base::PathExists(bundled)) {
      return bundled;
    }
  }
  const base::FilePath homebrew(
      FILE_PATH_LITERAL("/opt/homebrew/bin/llama-server"));
  return base::PathExists(homebrew) ? homebrew : base::FilePath();
}

InstalledModelFiles FindInstalledModelFiles() {
  base::FilePath models_dir;
  InstalledModelFiles missing;
  if (!ArkPaths::GetModelsDirectory(&models_dir)) {
    missing.error = "Ark could not resolve its private model directory.";
    return missing;
  }

  const base::FilePath installed_dir =
      models_dir.Append(FILE_PATH_LITERAL("installed"));
  if (!base::PathExists(installed_dir)) {
    missing.error = "No local model is installed.";
    return missing;
  }

  // An exact FileEnumerator pattern is applied at the traversal root on macOS
  // and can prevent descent into nested owner/repository directories. Walk all
  // files and select manifests by basename instead.
  base::FileEnumerator manifests(installed_dir, /*recursive=*/true,
                                 base::FileEnumerator::FILES);
  bool unsupported_model_found = false;
  for (base::FilePath manifest_path = manifests.Next(); !manifest_path.empty();
       manifest_path = manifests.Next()) {
    if (manifest_path.BaseName() !=
        base::FilePath(FILE_PATH_LITERAL("manifest.json"))) {
      continue;
    }
    std::string json;
    if (!base::ReadFileToString(manifest_path, &json)) {
      continue;
    }
    std::optional<base::DictValue> manifest =
        base::JSONReader::ReadDict(json, base::JSON_PARSE_RFC);
    if (!manifest) {
      continue;
    }

    InstalledModelFiles result;
    if (const std::string* compatibility =
            manifest->FindString("runtime_compatibility")) {
      result.runtime_compatibility = *compatibility;
    }
    const std::string* repository = manifest->FindString("repository");
    const bool supported =
        result.runtime_compatibility == kVerifiedCompatibility ||
        result.runtime_compatibility == kBundledRuntimeCompatibility;
    if ((repository && *repository == kUnsupportedVisionRepository) ||
        !supported) {
      unsupported_model_found = true;
      continue;
    }

    const base::ListValue* files = manifest->FindList("files");
    if (!files) {
      continue;
    }
    for (const base::Value& value : *files) {
      const base::DictValue* file = value.GetIfDict();
      const std::string* role = file ? file->FindString("role") : nullptr;
      const std::string* filename =
          file ? file->FindString("filename") : nullptr;
      if (!role || !filename) {
        continue;
      }
      const base::FilePath relative = base::FilePath::FromUTF8Unsafe(*filename);
      if (relative.IsAbsolute() || relative.ReferencesParent() ||
          relative != relative.BaseName()) {
        result.error = "The installed model manifest contains an unsafe path.";
        return result;
      }
      const base::FilePath path = manifest_path.DirName().Append(relative);
      if (!base::PathExists(path)) {
        result.error = "An installed model file is missing.";
        return result;
      }
      if (*role == "weights") {
        result.model_path = path;
      } else if (*role == "projector") {
        result.mmproj_path = path;
      }
    }
    if (!result.model_path.empty()) {
      return result;
    }
  }

  missing.error = unsupported_model_found
                      ? "Llama 3.2 Vision 11B is downloaded, but its mllama "
                        "architecture is unsupported. Download Ark's prepared "
                        "Qwen2.5-VL model to use local chat safely."
                      : "No verified, runtime-compatible local model "
                        "installation was found.";
  return missing;
}

}  // namespace

ArkInferenceService::PendingPrompt::PendingPrompt() = default;
ArkInferenceService::PendingPrompt::PendingPrompt(PendingPrompt&&) = default;
ArkInferenceService::PendingPrompt&
ArkInferenceService::PendingPrompt::operator=(PendingPrompt&&) = default;
ArkInferenceService::PendingPrompt::~PendingPrompt() = default;

ArkInferenceService::ServerLaunchResult::ServerLaunchResult() = default;
ArkInferenceService::ServerLaunchResult::ServerLaunchResult(
    ServerLaunchResult&&) = default;
ArkInferenceService::ServerLaunchResult&
ArkInferenceService::ServerLaunchResult::operator=(ServerLaunchResult&&) =
    default;
ArkInferenceService::ServerLaunchResult::~ServerLaunchResult() = default;

ArkInferenceService::ArkInferenceService(Profile* profile)
    : profile_(profile) {}

ArkInferenceService::~ArkInferenceService() {
  StopServer();
}

// static
ArkInferenceService::ServerLaunchResult
ArkInferenceService::PrepareAndLaunchServer(int port) {
  ServerLaunchResult result;
  const base::FilePath server_path = GetRuntimeExecutablePath();
  if (server_path.empty()) {
    result.error = "Ark's bundled local inference runtime is unavailable.";
    return result;
  }

  const InstalledModelFiles files = FindInstalledModelFiles();
  if (!files.error.empty()) {
    result.error = files.error;
    return result;
  }

  base::CommandLine command(server_path);
  command.AppendArg("-m");
  command.AppendArgPath(files.model_path);
  if (!files.mmproj_path.empty()) {
    command.AppendArg("--mmproj");
    command.AppendArgPath(files.mmproj_path);
    command.AppendArg("--mmproj-offload");
    // Qwen2.5-VL grounding tasks require at least 1024 visual tokens. Keep the
    // bundled runtime's multimodal path correct before image attachments are
    // exposed by the chat UI.
    command.AppendArg("--image-min-tokens");
    command.AppendArg("1024");
  }
  command.AppendArg("-ngl");
  command.AppendArg("99");
  command.AppendArg("-c");
  command.AppendArg("16384");
  command.AppendArg("-np");
  command.AppendArg("1");
  command.AppendArg("--temp");
  command.AppendArg("0.7");
  command.AppendArg("--host");
  command.AppendArg("127.0.0.1");
  command.AppendArg("--port");
  command.AppendArg(base::NumberToString(port));

  base::LaunchOptions options;
  options.current_directory = server_path.DirName();
  result.process = base::LaunchProcess(command, options);
  if (!result.process.IsValid()) {
    result.error = "Ark could not start its isolated local model runtime.";
  }
  return result;
}

// static
void ArkInferenceService::TerminateServerProcess(base::Process process) {
  if (process.IsValid()) {
    process.Terminate(0, /*wait=*/false);
    process.Close();
  }
}

void ArkInferenceService::SendPrompt(
    const std::string& conversation_id,
    const std::string& prompt,
    const std::optional<std::string>& image_data,
    const std::vector<ChatMessage>& history,
    PromptCallback callback) {
  if (pending_prompts_.size() >= kMaximumQueuedPrompts) {
    std::move(callback).Run(
        "Local inference is busy. Wait for a queued response or cancel it "
        "before sending another message.",
        false);
    return;
  }

  PendingPrompt pending;
  pending.conversation_id = conversation_id;
  pending.prompt = prompt;
  pending.image_data = image_data;
  pending.history = history;
  pending.callback = std::move(callback);
  pending_prompts_.push_back(std::move(pending));

  if (runtime_state_ == RuntimeState::kStopped) {
    StartServer();
  } else if (runtime_state_ == RuntimeState::kReady) {
    DispatchNextPrompt();
  }
}

void ArkInferenceService::StartServer() {
  runtime_state_ = RuntimeState::kStarting;
  startup_deadline_ = base::TimeTicks::Now() + base::Minutes(5);
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE, {base::MayBlock(), base::TaskPriority::USER_VISIBLE},
      base::BindOnce(&ArkInferenceService::PrepareAndLaunchServer, port_),
      base::BindOnce(&ArkInferenceService::OnServerPrepared,
                     weak_ptr_factory_.GetWeakPtr()));
}

void ArkInferenceService::OnServerPrepared(ServerLaunchResult result) {
  if (runtime_state_ != RuntimeState::kStarting) {
    if (result.process.IsValid()) {
      base::ThreadPool::PostTask(
          FROM_HERE,
          {base::MayBlock(), base::TaskPriority::USER_VISIBLE,
           base::TaskShutdownBehavior::BLOCK_SHUTDOWN},
          base::BindOnce(&ArkInferenceService::TerminateServerProcess,
                         std::move(result.process)));
    }
    return;
  }
  if (!result.process.IsValid()) {
    runtime_state_ = RuntimeState::kStopped;
    FailPendingPrompts(result.error.empty()
                           ? "Ark could not start the local model runtime."
                           : result.error);
    return;
  }
  server_process_ = std::move(result.process);
  CheckServerHealth();
}

void ArkInferenceService::CheckServerHealth() {
  if (runtime_state_ != RuntimeState::kStarting) {
    return;
  }
  auto request = std::make_unique<network::ResourceRequest>();
  request->url = GURL(base::StrCat(
      {"http://127.0.0.1:", base::NumberToString(port_), "/health"}));
  request->method = "GET";
  request->credentials_mode = network::mojom::CredentialsMode::kOmit;
  request->load_flags = net::LOAD_DISABLE_CACHE;
  health_loader_ = network::SimpleURLLoader::Create(std::move(request),
                                                    GetInferenceAnnotation());
  health_loader_->DownloadToString(
      profile_->GetURLLoaderFactory().get(),
      base::BindOnce(&ArkInferenceService::OnServerHealthChecked,
                     weak_ptr_factory_.GetWeakPtr()),
      64 * 1024);
}

void ArkInferenceService::OnServerHealthChecked(
    std::optional<std::string> response_body) {
  const bool healthy =
      health_loader_ && health_loader_->NetError() == net::OK && response_body;
  health_loader_.reset();
  if (runtime_state_ != RuntimeState::kStarting) {
    return;
  }
  if (healthy) {
    runtime_state_ = RuntimeState::kReady;
    DispatchNextPrompt();
    return;
  }
  if (base::TimeTicks::Now() >= startup_deadline_) {
    StopServerWithError(
        "The local model runtime did not become ready. Ark stopped it without "
        "affecting your browser or chat history.");
    return;
  }
  health_retry_timer_.Start(
      FROM_HERE, base::Milliseconds(500),
      base::BindOnce(&ArkInferenceService::CheckServerHealth,
                     weak_ptr_factory_.GetWeakPtr()));
}

void ArkInferenceService::DispatchNextPrompt() {
  if (runtime_state_ != RuntimeState::kReady || request_in_flight_ ||
      pending_prompts_.empty()) {
    return;
  }

  PendingPrompt pending = std::move(pending_prompts_.front());
  pending_prompts_.pop_front();
  base::DictValue root;
  root.Set("model", "local");
  root.Set("temperature", 0.7);
  root.Set("stream", false);
  base::ListValue messages;

  base::DictValue system_message;
  system_message.Set("role", "system");
  system_message.Set(
      "content",
      "You are Ark AI, a smart, capable, and helpful personal AI assistant "
      "running locally inside Ark Browser. Provide direct, natural, and "
      "accurate answers without unnecessary preamble.");
  messages.Append(std::move(system_message));

  size_t history_count = pending.history.size();
  if (history_count > 0 && pending.history.back().role == "user" &&
      pending.history.back().content == pending.prompt) {
    --history_count;
  }
  // Ensure the running agent only receives role and content from history.
  // Model metadata is strictly kept private to the UI and database, and is
  // never retrieved or exposed to the running agent.
  for (size_t i = 0; i < history_count; ++i) {
    base::DictValue message;
    message.Set("role", pending.history[i].role);
    message.Set("content", pending.history[i].content);
    messages.Append(std::move(message));
  }

  base::DictValue user_message;
  user_message.Set("role", "user");
  if (pending.image_data && !pending.image_data->empty()) {
    base::ListValue content;
    base::DictValue text_part;
    text_part.Set("type", "text");
    text_part.Set("text", pending.prompt);
    content.Append(std::move(text_part));
    base::DictValue image_part;
    image_part.Set("type", "image_url");
    base::DictValue image_url;
    image_url.Set("url", *pending.image_data);
    image_part.Set("image_url", std::move(image_url));
    content.Append(std::move(image_part));
    user_message.Set("content", std::move(content));
  } else {
    user_message.Set("content", pending.prompt);
  }
  messages.Append(std::move(user_message));
  root.Set("messages", std::move(messages));

  std::optional<std::string> payload =
      base::WriteJsonWithOptions(root, base::OPTIONS_PRETTY_PRINT);
  if (!payload) {
    std::move(pending.callback)
        .Run("Failed to construct the local inference request.", false);
    DispatchNextPrompt();
    return;
  }

  auto request = std::make_unique<network::ResourceRequest>();
  request->url =
      GURL(base::StrCat({"http://127.0.0.1:", base::NumberToString(port_),
                         "/v1/chat/completions"}));
  request->method = "POST";
  request->credentials_mode = network::mojom::CredentialsMode::kOmit;
  request->load_flags = net::LOAD_DISABLE_CACHE;

  active_callback_ = std::move(pending.callback);
  request_in_flight_ = true;
  prompt_loader_ = network::SimpleURLLoader::Create(std::move(request),
                                                    GetInferenceAnnotation());
  prompt_loader_->AttachStringForUpload(*payload, "application/json");
  prompt_loader_->DownloadToString(
      profile_->GetURLLoaderFactory().get(),
      base::BindOnce(&ArkInferenceService::OnServerResponse,
                     weak_ptr_factory_.GetWeakPtr()),
      5 * 1024 * 1024);
}

void ArkInferenceService::OnServerResponse(
    std::optional<std::string> response_body) {
  const int net_error =
      prompt_loader_ ? prompt_loader_->NetError() : net::ERR_ABORTED;
  prompt_loader_.reset();
  request_in_flight_ = false;
  PromptCallback callback = std::move(active_callback_);
  if (net_error != net::OK || !response_body || response_body->empty()) {
    if (callback) {
      std::move(callback).Run(
          "The isolated local model runtime stopped responding. Ark preserved "
          "the browser and chat history.",
          false);
    }
    StopServerWithError(
        "A queued request was cancelled because the local runtime stopped.");
    return;
  }

  std::optional<base::DictValue> parsed =
      base::JSONReader::ReadDict(*response_body, base::JSON_PARSE_RFC);
  const base::ListValue* choices =
      parsed ? parsed->FindList("choices") : nullptr;
  const base::DictValue* choice =
      choices && !choices->empty() ? (*choices)[0].GetIfDict() : nullptr;
  const base::DictValue* message =
      choice ? choice->FindDict("message") : nullptr;
  const std::string* content =
      message ? message->FindString("content") : nullptr;
  if (!content) {
    if (callback) {
      std::move(callback).Run(
          parsed ? "The local model response contained no completion text."
                 : "Received invalid JSON from the local model.",
          false);
    }
    DispatchNextPrompt();
    return;
  }
  if (callback) {
    std::move(callback).Run(*content, true);
  }
  DispatchNextPrompt();
}

void ArkInferenceService::FailPendingPrompts(const std::string& error) {
  while (!pending_prompts_.empty()) {
    PromptCallback callback = std::move(pending_prompts_.front().callback);
    pending_prompts_.pop_front();
    if (callback) {
      std::move(callback).Run(error, false);
    }
  }
}

void ArkInferenceService::StopServerWithError(const std::string& error) {
  runtime_state_ = RuntimeState::kStopped;
  health_retry_timer_.Stop();
  health_loader_.reset();
  prompt_loader_.reset();
  request_in_flight_ = false;
  if (active_callback_) {
    std::move(active_callback_).Run(error, false);
  }
  FailPendingPrompts(error);
  if (server_process_.IsValid()) {
    base::Process process = std::move(server_process_);
    base::ThreadPool::PostTask(
        FROM_HERE,
        {base::MayBlock(), base::TaskPriority::USER_VISIBLE,
         base::TaskShutdownBehavior::BLOCK_SHUTDOWN},
        base::BindOnce(&ArkInferenceService::TerminateServerProcess,
                       std::move(process)));
  }
}

void ArkInferenceService::StopServer() {
  StopServerWithError("The local model runtime was stopped.");
}

}  // namespace ark
