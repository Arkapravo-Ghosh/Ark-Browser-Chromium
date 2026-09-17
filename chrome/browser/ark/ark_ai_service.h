// Copyright 2026 Arkapravo Ghosh
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_ARK_ARK_AI_SERVICE_H_
#define CHROME_BROWSER_ARK_ARK_AI_SERVICE_H_

#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "base/files/file_path.h"
#include "base/functional/callback_forward.h"
#include "base/memory/weak_ptr.h"
#include "base/threading/sequence_bound.h"
#include "chrome/browser/ark/ark_conversation_store.h"
#include "chrome/browser/ark/ark_model_manager.h"
#include "components/keyed_service/core/keyed_service.h"

class Profile;

namespace ark {
class ArkCloudInferenceService;
class ArkInferenceService;

class ArkAIService : public KeyedService {
 public:
  using StateCallback = base::OnceCallback<void(ConversationState)>;
  using ConversationsCallback =
      base::OnceCallback<void(std::vector<ConversationState>)>;
  using MessagesCallback = base::OnceCallback<void(std::vector<ChatMessage>)>;
  using ResultCallback = base::OnceCallback<void(bool)>;
  using PromptCallback =
      base::OnceCallback<void(const std::string& response, bool success)>;

  ArkAIService(Profile* profile, bool in_memory);
  ~ArkAIService() override;

  void GetChatState(StateCallback callback);
  void GetConversations(ConversationsCallback callback);
  void CreateConversation(std::string model_name, StateCallback callback);
  void SwitchConversation(std::string id, StateCallback callback);
  void DeleteConversation(std::string id, ResultCallback callback);
  void GetMessages(std::string conversation_id, MessagesCallback callback);
  void AddMessage(std::string conversation_id,
                  std::string role,
                  std::string content,
                  std::string model_name,
                  ResultCallback callback);
  void UpdateConversationTitle(std::string id,
                               std::string title,
                               ResultCallback callback);
  void UpdateConversationModel(std::string id,
                               std::string model_name,
                               ResultCallback callback);
  void SaveDraft(std::string id, std::string draft, ResultCallback callback);
  void SendChatPrompt(std::string conversation_id,
                      std::string message,
                      std::optional<std::string> image_data,
                      std::optional<std::string> provider_credential,
                      PromptCallback callback);
  void GenerateConversationTitle(std::string conversation_id,
                                 std::string user_message,
                                 std::optional<std::string> provider_credential,
                                 PromptCallback callback);
  void SearchLocalModels(std::string query,
                         ArkModelManager::SearchCallback callback);
  void GetLocalModelState(ArkModelManager::StateCallback callback);
  void GetInstalledLocalModels(ArkModelManager::InstalledCallback callback);
  void StartLocalModelDownload(std::string repository,
                               bool license_accepted,
                               ArkModelManager::StateCallback callback);
  void PauseLocalModelDownload(ArkModelManager::StateCallback callback);
  void ResumeLocalModelDownload(ArkModelManager::StateCallback callback);
  void DeleteLocalModel(std::string model_id,
                        ArkModelManager::StateCallback callback);

 private:
  void BeginChatPrompt(std::string conversation_id,
                       std::string message,
                       std::optional<std::string> image_data,
                       std::optional<std::string> provider_credential,
                       PromptCallback callback);
  void OnConversationReady(std::string conversation_id,
                           std::string message,
                           std::optional<std::string> image_data,
                           std::optional<std::string> provider_credential,
                           PromptCallback callback,
                           ConversationState conversation);
  bool DispatchPrompt(std::string request_key,
                      std::string model_name,
                      std::string prompt,
                      std::optional<std::string> image_data,
                      std::optional<std::string> provider_credential,
                      std::vector<ChatMessage> history,
                      PromptCallback callback);
  void OnInitialized(bool success);
  void OnPromptCompleted(std::string conversation_id,
                         std::string model_name,
                         PromptCallback callback,
                         const std::string& response,
                         bool success);
  void RunWhenReady(base::OnceClosure operation, base::OnceClosure failure);

  base::SequenceBound<ConversationStore> store_;
  std::unique_ptr<ArkModelManager> model_manager_;
  std::unique_ptr<ArkInferenceService> inference_service_;
  std::unique_ptr<ArkCloudInferenceService> cloud_inference_service_;
  std::set<std::string> active_chat_conversation_ids_;
  bool initializing_ = true;
  bool ready_ = false;
  std::vector<base::OnceClosure> pending_operations_;
  base::WeakPtrFactory<ArkAIService> weak_ptr_factory_{this};
};

}  // namespace ark

#endif  // CHROME_BROWSER_ARK_ARK_AI_SERVICE_H_
