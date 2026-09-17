// Copyright 2026 Arkapravo Ghosh
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_ARK_ARK_INFERENCE_SERVICE_H_
#define CHROME_BROWSER_ARK_ARK_INFERENCE_SERVICE_H_

#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <optional>
#include <set>
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

  bool SendPrompt(const std::string& conversation_id,
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

  struct ActivePrompt {
    std::string conversation_id;
    PromptCallback callback;
  };

  static ProcessRunResult RunPromptInProcess(
      std::string model_id,
      std::string prompt,
      std::optional<std::string> image_data,
      std::vector<ChatMessage> history);

  void DispatchPrompts();
  void OnProcessResponse(uint64_t request_id, ProcessRunResult result);
  void FailPendingPrompts(const std::string& error);
  void StopServerWithError(const std::string& error);

  const raw_ptr<Profile> profile_;
  RuntimeState runtime_state_ = RuntimeState::kStopped;
  std::deque<PendingPrompt> pending_prompts_;
  std::map<uint64_t, ActivePrompt> active_prompts_;
  std::set<std::string> busy_conversation_ids_;
  uint64_t next_request_id_ = 1;
  base::WeakPtrFactory<ArkInferenceService> weak_ptr_factory_{this};
};

}  // namespace ark

#endif  // CHROME_BROWSER_ARK_ARK_INFERENCE_SERVICE_H_
