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
  base::FilePath model_path;
  base::FilePath mmproj_path;
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
    std::string prompt;
    std::optional<std::string> image_data;
    std::vector<ChatMessage> history;
    PromptCallback callback;
  };

  struct ServerLaunchResult {
    ServerLaunchResult();
    ServerLaunchResult(ServerLaunchResult&&);
    ServerLaunchResult& operator=(ServerLaunchResult&&);
    ~ServerLaunchResult();

    base::Process process;
    std::string error;
  };

  static ServerLaunchResult PrepareAndLaunchServer(int port);
  static void TerminateServerProcess(base::Process process);

  void StartServer();
  void OnServerPrepared(ServerLaunchResult result);
  void CheckServerHealth();
  void OnServerHealthChecked(std::optional<std::string> response_body);
  void DispatchNextPrompt();
  void OnServerResponse(std::optional<std::string> response_body);
  void FailPendingPrompts(const std::string& error);
  void StopServerWithError(const std::string& error);

  const raw_ptr<Profile> profile_;
  base::Process server_process_;
  RuntimeState runtime_state_ = RuntimeState::kStopped;
  std::deque<PendingPrompt> pending_prompts_;
  PromptCallback active_callback_;
  std::unique_ptr<network::SimpleURLLoader> health_loader_;
  std::unique_ptr<network::SimpleURLLoader> prompt_loader_;
  base::OneShotTimer health_retry_timer_;
  base::TimeTicks startup_deadline_;
  bool request_in_flight_ = false;
  int port_ = 8088;
  base::WeakPtrFactory<ArkInferenceService> weak_ptr_factory_{this};
};

}  // namespace ark

#endif  // CHROME_BROWSER_ARK_ARK_INFERENCE_SERVICE_H_
