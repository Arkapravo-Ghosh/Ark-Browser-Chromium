// Copyright 2026 Arkapravo Ghosh
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_ARK_ARK_INFERENCE_SERVICE_H_
#define CHROME_BROWSER_ARK_ARK_INFERENCE_SERVICE_H_

#include <deque>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "base/files/file_path.h"
#include "base/functional/callback_forward.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/process/process.h"
#include "base/time/time.h"
#include "base/timer/timer.h"
#include "chrome/browser/ark/ark_conversation_store.h"

class Profile;

namespace network {
class SimpleURLLoader;
}

namespace ark {

struct InstalledModelFiles {
  base::FilePath install_directory;
  base::FilePath model_path;
  base::FilePath mmproj_path;
  std::string runtime_backend;
  std::string runtime_compatibility;
  std::string error;
};

class ArkInferenceService {
 public:
  using PromptCallback =
      base::OnceCallback<void(const std::string& response, bool success)>;

  explicit ArkInferenceService(Profile* profile);
  ~ArkInferenceService();

  void SendPrompt(const std::string& conversation_id,
                  const std::string& model_id,
                  const std::string& prompt,
                  const std::optional<std::string>& image_data,
                  const std::vector<ChatMessage>& history,
                  PromptCallback callback);

  void StopServer();

 private:
  enum class RuntimeState { kStopped, kStarting, kReady };

  struct PendingPrompt {
    PendingPrompt();
    PendingPrompt(PendingPrompt&&);
    PendingPrompt& operator=(PendingPrompt&&);
    ~PendingPrompt();

    std::string conversation_id;
    std::string model_id;
    std::string prompt;
    std::optional<std::string> image_data;
    std::vector<ChatMessage> history;
    PromptCallback callback;
  };

  struct ProcessRunResult {
    std::string response;
    std::string error;
    bool success = false;
  };

  static ProcessRunResult RunPromptInProcess(std::string model_id,
                                              std::string prompt,
                                              std::optional<std::string> image_data,
                                              std::vector<ChatMessage> history);

  void StartServer();
  void DispatchNextPrompt();
  void OnProcessResponse(ProcessRunResult result);
  void FailPendingPrompts(const std::string& error);
  void StopServerWithError(const std::string& error);

  const raw_ptr<Profile> profile_;
  RuntimeState runtime_state_ = RuntimeState::kStopped;
  std::deque<PendingPrompt> pending_prompts_;
  PromptCallback active_callback_;
  bool request_in_flight_ = false;
  std::string active_model_id_;
  base::WeakPtrFactory<ArkInferenceService> weak_ptr_factory_{this};
};

}  // namespace ark

#endif  // CHROME_BROWSER_ARK_ARK_INFERENCE_SERVICE_H_
