// Copyright 2026 Arkapravo Ghosh
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/ark/ark_inference_service.h"

#include <utility>

#include "base/base64.h"
#include "base/base_paths.h"
#include "base/command_line.h"
#include "base/files/file_enumerator.h"
#include "base/files/file_util.h"
#include "base/files/scoped_temp_dir.h"
#include "base/functional/bind.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/path_service.h"
#include "base/process/launch.h"
#include "base/process/process_handle.h"
#include "base/strings/strcat.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "base/task/thread_pool.h"
#include "base/time/time.h"
#include "base/values.h"
#include "chrome/browser/ark/ark_paths.h"
#include "chrome/browser/profiles/profile.h"
#include "url/gurl.h"

namespace ark {

namespace {

constexpr char kVerifiedCompatibility[] = "verified_compatible";
constexpr char kBundledRuntimeCompatibility[] =
    "supported_by_bundled_llama_cpp_0_4_0";
constexpr char kDownloadedUnverifiedCompatibility[] = "downloaded_unverified";
constexpr char kMlxCompatibility[] = "supported_by_bundled_mlx_vlm_0_5_0";
constexpr char kMlxUnverifiedCompatibility[] = "downloaded_unverified_mlx";
constexpr size_t kMaximumQueuedPrompts = 8;

// These helpers perform filesystem I/O and must only run on a MayBlock worker.
base::FilePath GetRuntimeCliPath() {
  base::FilePath exe_dir;
  if (base::PathService::Get(base::DIR_EXE, &exe_dir)) {
    const base::FilePath bundled =
        exe_dir.Append(FILE_PATH_LITERAL("llama-cli"));
    if (base::PathExists(bundled)) {
      return bundled;
    }
  }
  const base::FilePath homebrew(
      FILE_PATH_LITERAL("/opt/homebrew/bin/llama-cli"));
  return base::PathExists(homebrew) ? homebrew : base::FilePath();
}

base::FilePath GetMlxPythonPath() {
  base::FilePath exe_dir;
  if (!base::PathService::Get(base::DIR_EXE, &exe_dir)) {
    return {};
  }
  const base::FilePath bundled = exe_dir.DirName()
                                     .Append(FILE_PATH_LITERAL("Resources"))
                                     .Append(FILE_PATH_LITERAL("ark-mlx"))
                                     .Append(FILE_PATH_LITERAL("python"))
                                     .Append(FILE_PATH_LITERAL("bin"))
                                     .Append(FILE_PATH_LITERAL("python3"));
  return base::PathExists(bundled) ? bundled : base::FilePath();
}

std::string ModelIdForManifest(const base::DictValue& manifest) {
  if (const std::string* stored = manifest.FindString("model_id")) {
    return *stored;
  }
  const std::string* repository = manifest.FindString("repository");
  const std::string* revision = manifest.FindString("resolved_revision");
  const std::string* variant = manifest.FindString("variant");
  if (!repository || !revision || !variant) {
    return {};
  }
  if (*repository == "ggml-org/Qwen2.5-VL-7B-Instruct-GGUF") {
    return "local:qwen2.5-vl-7b-instruct";
  }
  return base::StrCat(
      {"local:huggingface:", *repository, "@", *revision, ":", *variant});
}

InstalledModelFiles FindInstalledModelFiles(const std::string& model_id) {
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
    if (ModelIdForManifest(*manifest) != model_id) {
      continue;
    }

    InstalledModelFiles result;
    if (const std::string* compatibility =
            manifest->FindString("runtime_compatibility")) {
      result.runtime_compatibility = *compatibility;
    }
    const std::string* backend = manifest->FindString("runtime_backend");
    result.runtime_backend = backend ? *backend : "llama.cpp";
    const bool supported =
        result.runtime_compatibility == kVerifiedCompatibility ||
        result.runtime_compatibility == kBundledRuntimeCompatibility ||
        result.runtime_compatibility == kDownloadedUnverifiedCompatibility ||
        result.runtime_compatibility == kMlxCompatibility ||
        result.runtime_compatibility == kMlxUnverifiedCompatibility;
    if (!supported) {
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
    result.install_directory = manifest_path.DirName();
    if (result.runtime_backend == "mlx-vlm" || !result.model_path.empty()) {
      return result;
    }
  }

  missing.error = "No runnable local model installation was found.";
  return missing;
}

}  // namespace

ArkInferenceService::PendingPrompt::PendingPrompt() = default;
ArkInferenceService::PendingPrompt::PendingPrompt(PendingPrompt&&) = default;
ArkInferenceService::PendingPrompt&
ArkInferenceService::PendingPrompt::operator=(PendingPrompt&&) = default;
ArkInferenceService::PendingPrompt::~PendingPrompt() = default;

ArkInferenceService::ArkInferenceService(Profile* profile)
    : profile_(profile) {}

ArkInferenceService::~ArkInferenceService() {
  StopServer();
}

// static
ArkInferenceService::ProcessRunResult ArkInferenceService::RunPromptInProcess(
    std::string model_id,
    std::string prompt,
    std::optional<std::string> image_data,
    std::vector<ChatMessage> history) {
  ProcessRunResult result;
  const InstalledModelFiles files = FindInstalledModelFiles(model_id);
  if (!files.error.empty()) {
    result.error = files.error;
    return result;
  }

  base::ScopedTempDir temp_dir;
  if (!temp_dir.CreateUniqueTempDir()) {
    result.error = "Ark could not create an isolated inference workspace.";
    return result;
  }
  const base::FilePath prompt_path =
      temp_dir.GetPath().AppendASCII("prompt.txt");
  std::string full_prompt =
      "You are a helpful assistant. If you include code, use a fenced Markdown "
      "block with the language immediately after the opening backticks, for "
      "example ```csharp.\n\n";
  for (const ChatMessage& message : history) {
    full_prompt.append(message.role);
    full_prompt.append(": ");
    full_prompt.append(message.content);
    full_prompt.append("\n\n");
  }
  full_prompt.append("user: ");
  full_prompt.append(prompt);
  if (!base::WriteFile(prompt_path, full_prompt)) {
    result.error = "Ark could not prepare the isolated inference request.";
    return result;
  }

  base::FilePath image_path;
  if (image_data && base::StartsWith(*image_data, "data:",
                                     base::CompareCase::INSENSITIVE_ASCII)) {
    const size_t comma = image_data->find(',');
    if (comma != std::string::npos &&
        image_data->find(";base64", comma > 0 ? comma - 1 : 0) !=
            std::string::npos) {
      const std::optional<std::vector<uint8_t>> decoded =
          base::Base64Decode(image_data->substr(comma + 1));
      if (decoded) {
        const std::string header = image_data->substr(5, comma - 5);
        const std::string extension =
            base::StartsWith(header, "image/png",
                             base::CompareCase::INSENSITIVE_ASCII)
                ? ".png"
            : base::StartsWith(header, "image/webp",
                               base::CompareCase::INSENSITIVE_ASCII)
                ? ".webp"
                : ".jpg";
        image_path = temp_dir.GetPath().AppendASCII("attachment" + extension);
        base::WriteFile(image_path, base::span<const uint8_t>(*decoded));
      }
    }
  }

  base::CommandLine command(files.runtime_backend == "mlx-vlm"
                                ? GetMlxPythonPath()
                                : GetRuntimeCliPath());
  const bool is_mlx_backend = files.runtime_backend == "mlx-vlm";
  if (command.GetProgram().empty()) {
    result.error = files.runtime_backend == "mlx-vlm"
                       ? "Ark's bundled MLX runtime is unavailable."
                       : "Ark's bundled llama.cpp runtime is unavailable.";
    return result;
  }
  base::FilePath llama_output_path;
  if (is_mlx_backend) {
    base::FilePath runner =
        command.GetProgram().DirName().DirName().DirName().AppendASCII(
            "ark_mlx_runner.py");
    command.AppendArgPath(runner);
    command.AppendArg("--model");
    command.AppendArgPath(files.install_directory);
    command.AppendArg("--prompt-file");
    command.AppendArgPath(prompt_path);
    if (!image_path.empty()) {
      command.AppendArg("--image");
      command.AppendArgPath(image_path);
    }
  } else {
    llama_output_path = temp_dir.GetPath().AppendASCII("llama-completion.txt");
    command.AppendArg("-m");
    command.AppendArgPath(files.model_path);
    // In this pinned llama-cli build, -f alone enters the interactive
    // conversation UI even with --single-turn. A harmless predefined prompt
    // makes the request non-interactive while the real prompt remains in the
    // owner-only temporary file instead of being exposed in the process argv.
    command.AppendArg("--prompt");
    command.AppendArg("\n");
    command.AppendArg("-f");
    command.AppendArgPath(prompt_path);
    command.AppendArg("-n");
    command.AppendArg("1024");
    command.AppendArg("--temp");
    command.AppendArg("0.7");
    command.AppendArg("--no-display-prompt");
    command.AppendArg("--no-show-timings");
    command.AppendArg("--log-disable");
    command.AppendArg("--simple-io");
    // Never enter llama.cpp's interactive stdin loop. Each Ark request must
    // be a bounded, one-turn child process so it can exit cleanly after the
    // captured completion and never block the browser's request queue.
    command.AppendArg("--single-turn");
    command.AppendArg("--reasoning");
    command.AppendArg("off");
    // llama-cli writes its logo, model metadata, command help, and prompt to
    // stdout. Its output file has a deterministic transcript envelope, which
    // Ark validates below before extracting only the assistant completion.
    command.AppendArg("--output");
    command.AppendArgPath(llama_output_path);
    if (!files.mmproj_path.empty()) {
      command.AppendArg("--mmproj");
      command.AppendArgPath(files.mmproj_path);
      command.AppendArg("--mmproj-offload");
      command.AppendArg("--image-min-tokens");
      command.AppendArg("1024");
      if (!image_path.empty()) {
        command.AppendArg("--image");
        command.AppendArgPath(image_path);
      }
    }
  }

  // A request is one isolated child process. GetAppOutput owns the child,
  // captures stdout, waits off the UI sequence, and tears it down before the
  // callback returns. No localhost listener or model state crosses requests.
  int exit_code = -1;
  std::string output;
  if (!base::GetAppOutputWithExitCode(command, &output, &exit_code) ||
      exit_code != 0) {
    result.error =
        "The isolated local model process failed. Ark preserved "
        "the browser and chat history.";
    return result;
  }
  if (is_mlx_backend) {
    result.response = std::move(output);
  } else {
    std::string transcript;
    if (!base::ReadFileToString(llama_output_path, &transcript)) {
      result.error = "The isolated local model returned no completion file.";
      return result;
    }
    const std::string transcript_prefix =
        base::StrCat({"User:\n", full_prompt, "\n\nAssistant:\n"});
    if (!base::StartsWith(transcript, transcript_prefix,
                          base::CompareCase::SENSITIVE)) {
      result.error =
          "The isolated local model returned an invalid completion "
          "envelope.";
      return result;
    }
    result.response = transcript.substr(transcript_prefix.size());
  }
  base::TrimWhitespaceASCII(result.response, base::TRIM_ALL, &result.response);
  result.success = !result.response.empty();
  if (!result.success) {
    result.error = "The isolated local model returned no completion text.";
  }
  return result;
}

void ArkInferenceService::SendPrompt(
    const std::string& conversation_id,
    const std::string& model_id,
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
  pending.model_id = model_id;
  pending.prompt = prompt;
  pending.image_data = image_data;
  pending.history = history;
  pending.callback = std::move(callback);
  pending_prompts_.push_back(std::move(pending));

  if (!request_in_flight_) {
    DispatchNextPrompt();
  }
}

void ArkInferenceService::StartServer() {
  DispatchNextPrompt();
}

void ArkInferenceService::DispatchNextPrompt() {
  if (request_in_flight_ || pending_prompts_.empty()) {
    return;
  }
  PendingPrompt pending = std::move(pending_prompts_.front());
  pending_prompts_.pop_front();
  active_model_id_ = pending.model_id;
  active_callback_ = std::move(pending.callback);
  request_in_flight_ = true;
  runtime_state_ = RuntimeState::kStarting;
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE, {base::MayBlock(), base::TaskPriority::USER_VISIBLE},
      base::BindOnce(&ArkInferenceService::RunPromptInProcess, pending.model_id,
                     pending.prompt, std::move(pending.image_data),
                     std::move(pending.history)),
      base::BindOnce(&ArkInferenceService::OnProcessResponse,
                     weak_ptr_factory_.GetWeakPtr()));
}

void ArkInferenceService::OnProcessResponse(ProcessRunResult result) {
  request_in_flight_ = false;
  runtime_state_ = RuntimeState::kStopped;
  PromptCallback callback = std::move(active_callback_);
  if (!result.success) {
    if (callback) {
      std::move(callback).Run(result.error, false);
    }
  } else if (callback) {
    std::move(callback).Run(result.response, true);
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
  request_in_flight_ = false;
  if (active_callback_) {
    std::move(active_callback_).Run(error, false);
  }
  FailPendingPrompts(error);
}

void ArkInferenceService::StopServer() {
  StopServerWithError("The local model runtime was stopped.");
}

}  // namespace ark
