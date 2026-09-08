// Copyright 2026 Arkapravo Ghosh
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_ARK_ARK_AI_SERVICE_H_
#define CHROME_BROWSER_ARK_ARK_AI_SERVICE_H_

#include <string>
#include <vector>

#include "base/files/file_path.h"
#include "base/functional/callback_forward.h"
#include "base/memory/weak_ptr.h"
#include "base/threading/sequence_bound.h"
#include "chrome/browser/ark/ark_conversation_store.h"
#include "components/keyed_service/core/keyed_service.h"

namespace ark {

class ArkAIService : public KeyedService {
 public:
  using StateCallback = base::OnceCallback<void(ConversationState)>;
  using ResultCallback = base::OnceCallback<void(bool)>;

  ArkAIService(const base::FilePath& profile_path, bool in_memory);
  ~ArkAIService() override;

  void GetChatState(StateCallback callback);
  void CreateConversation(StateCallback callback);
  void SaveDraft(std::string id, std::string draft, ResultCallback callback);

 private:
  void OnInitialized(bool success);
  void RunWhenReady(base::OnceClosure operation, base::OnceClosure failure);

  base::SequenceBound<ConversationStore> store_;
  bool initializing_ = true;
  bool ready_ = false;
  std::vector<base::OnceClosure> pending_operations_;
  base::WeakPtrFactory<ArkAIService> weak_ptr_factory_{this};
};

}  // namespace ark

#endif  // CHROME_BROWSER_ARK_ARK_AI_SERVICE_H_
