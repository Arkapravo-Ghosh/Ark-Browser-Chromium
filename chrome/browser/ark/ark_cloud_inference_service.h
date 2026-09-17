// Copyright 2026 Arkapravo Ghosh
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_ARK_ARK_CLOUD_INFERENCE_SERVICE_H_
#define CHROME_BROWSER_ARK_ARK_CLOUD_INFERENCE_SERVICE_H_

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "base/functional/callback_forward.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "chrome/browser/ark/ark_conversation_store.h"

class Profile;

namespace network {
class SimpleURLLoader;
}

namespace ark {

// Owns cloud-provider HTTP details. ArkAIService remains responsible for
// conversation state, persistence, and selecting this adapter.
class ArkCloudInferenceService {
 public:
  using PromptCallback =
      base::OnceCallback<void(const std::string& response, bool success)>;

  explicit ArkCloudInferenceService(Profile* profile);
  ~ArkCloudInferenceService();

  bool SendPrompt(const std::string& model_id,
                  const std::string& prompt,
                  const std::optional<std::string>& image_data,
                  const std::vector<ChatMessage>& history,
                  const std::optional<std::string>& credential,
                  PromptCallback callback);

 private:
  struct ActiveRequest {
    std::unique_ptr<network::SimpleURLLoader> loader;
    PromptCallback callback;
  };

  void OnGeminiResponse(uint64_t request_id,
                        std::optional<std::string> response_body);

  const raw_ptr<Profile> profile_;
  std::map<uint64_t, ActiveRequest> active_requests_;
  uint64_t next_request_id_ = 1;
  base::WeakPtrFactory<ArkCloudInferenceService> weak_ptr_factory_{this};
};

}  // namespace ark

#endif  // CHROME_BROWSER_ARK_ARK_CLOUD_INFERENCE_SERVICE_H_
