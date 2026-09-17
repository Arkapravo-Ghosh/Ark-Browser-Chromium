// Copyright 2026 Arkapravo Ghosh
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/ark/ark_ai_service.h"

#include <utility>

#include "base/functional/bind.h"
#include "base/functional/callback_helpers.h"
#include "base/strings/string_util.h"
#include "base/task/thread_pool.h"
#include "chrome/browser/ark/ark_cloud_inference_service.h"
#include "chrome/browser/ark/ark_inference_service.h"
#include "chrome/browser/ark/ark_paths.h"
#include "chrome/browser/profiles/profile.h"

namespace ark {

namespace {
base::FilePath GetDatabasePath(const base::FilePath& profile_path,
                               bool in_memory) {
  return profile_path.Append(FILE_PATH_LITERAL("ArkAI"))
      .Append(FILE_PATH_LITERAL("conversations.sqlite3"));
}

base::FilePath GetFallbackDatabasePath() {
  base::FilePath path;
  if (!ArkPaths::GetConversationsDatabase(&path)) {
    return base::FilePath();
  }
  return path;
}
}  // namespace

ArkAIService::ArkAIService(Profile* profile, bool in_memory)
    : store_(base::ThreadPool::CreateSequencedTaskRunnerForResource(
          {base::MayBlock(), base::TaskPriority::USER_VISIBLE,
           base::TaskShutdownBehavior::BLOCK_SHUTDOWN},
          GetDatabasePath(profile->GetPath(), in_memory))),
      model_manager_(std::make_unique<ArkModelManager>(profile, in_memory)),
      inference_service_(std::make_unique<ArkInferenceService>(profile)),
      cloud_inference_service_(
          std::make_unique<ArkCloudInferenceService>(profile)) {
  store_.AsyncCall(&ConversationStore::Init)
      .WithArgs(GetDatabasePath(profile->GetPath(), in_memory),
                GetFallbackDatabasePath(), in_memory)
      .Then(base::BindOnce(&ArkAIService::OnInitialized,
                           weak_ptr_factory_.GetWeakPtr()));
}

ArkAIService::~ArkAIService() = default;

void ArkAIService::GetChatState(StateCallback callback) {
  auto [success_callback, failure_callback] =
      base::SplitOnceCallback(std::move(callback));
  RunWhenReady(
      base::BindOnce(
          [](base::SequenceBound<ConversationStore>* store,
             StateCallback callback) {
            store->AsyncCall(&ConversationStore::GetOrCreateCurrent)
                .Then(std::move(callback));
          },
          &store_, std::move(success_callback)),
      base::BindOnce(std::move(failure_callback), ConversationState()));
}

void ArkAIService::GetConversations(ConversationsCallback callback) {
  auto [success_callback, failure_callback] =
      base::SplitOnceCallback(std::move(callback));
  RunWhenReady(base::BindOnce(
                   [](base::SequenceBound<ConversationStore>* store,
                      ConversationsCallback callback) {
                     store->AsyncCall(&ConversationStore::GetConversations)
                         .Then(std::move(callback));
                   },
                   &store_, std::move(success_callback)),
               base::BindOnce(std::move(failure_callback),
                              std::vector<ConversationState>()));
}

void ArkAIService::CreateConversation(std::string model_name,
                                      StateCallback callback) {
  auto [success_callback, failure_callback] =
      base::SplitOnceCallback(std::move(callback));
  RunWhenReady(
      base::BindOnce(
          [](base::SequenceBound<ConversationStore>* store,
             std::string model_name, StateCallback callback) {
            store->AsyncCall(&ConversationStore::CreateConversation)
                .WithArgs(std::move(model_name))
                .Then(std::move(callback));
          },
          &store_, std::move(model_name), std::move(success_callback)),
      base::BindOnce(std::move(failure_callback), ConversationState()));
}

void ArkAIService::SwitchConversation(std::string id, StateCallback callback) {
  auto [success_callback, failure_callback] =
      base::SplitOnceCallback(std::move(callback));
  RunWhenReady(
      base::BindOnce(
          [](base::SequenceBound<ConversationStore>* store, std::string id,
             StateCallback callback) {
            store->AsyncCall(&ConversationStore::SwitchConversation)
                .WithArgs(std::move(id))
                .Then(std::move(callback));
          },
          &store_, std::move(id), std::move(success_callback)),
      base::BindOnce(std::move(failure_callback), ConversationState()));
}

void ArkAIService::DeleteConversation(std::string id, ResultCallback callback) {
  auto [success_callback, failure_callback] =
      base::SplitOnceCallback(std::move(callback));
  RunWhenReady(base::BindOnce(
                   [](base::SequenceBound<ConversationStore>* store,
                      std::string id, ResultCallback callback) {
                     store->AsyncCall(&ConversationStore::DeleteConversation)
                         .WithArgs(std::move(id))
                         .Then(std::move(callback));
                   },
                   &store_, std::move(id), std::move(success_callback)),
               base::BindOnce(std::move(failure_callback), false));
}

void ArkAIService::GetMessages(std::string conversation_id,
                               MessagesCallback callback) {
  auto [success_callback, failure_callback] =
      base::SplitOnceCallback(std::move(callback));
  RunWhenReady(
      base::BindOnce(
          [](base::SequenceBound<ConversationStore>* store,
             std::string conversation_id, MessagesCallback callback) {
            store->AsyncCall(&ConversationStore::GetMessages)
                .WithArgs(std::move(conversation_id))
                .Then(std::move(callback));
          },
          &store_, std::move(conversation_id), std::move(success_callback)),
      base::BindOnce(std::move(failure_callback), std::vector<ChatMessage>()));
}

void ArkAIService::AddMessage(std::string conversation_id,
                              std::string role,
                              std::string content,
                              std::string model_name,
                              ResultCallback callback) {
  auto [success_callback, failure_callback] =
      base::SplitOnceCallback(std::move(callback));
  RunWhenReady(
      base::BindOnce(
          [](base::SequenceBound<ConversationStore>* store,
             std::string conversation_id, std::string role, std::string content,
             std::string model_name, ResultCallback callback) {
            store->AsyncCall(&ConversationStore::AddMessage)
                .WithArgs(std::move(conversation_id), std::move(role),
                          std::move(content), std::move(model_name))
                .Then(std::move(callback));
          },
          &store_, std::move(conversation_id), std::move(role),
          std::move(content), std::move(model_name),
          std::move(success_callback)),
      base::BindOnce(std::move(failure_callback), false));
}

void ArkAIService::UpdateConversationTitle(std::string id,
                                           std::string title,
                                           ResultCallback callback) {
  auto [success_callback, failure_callback] =
      base::SplitOnceCallback(std::move(callback));
  RunWhenReady(
      base::BindOnce(
          [](base::SequenceBound<ConversationStore>* store, std::string id,
             std::string title, ResultCallback callback) {
            store->AsyncCall(&ConversationStore::UpdateConversationTitle)
                .WithArgs(std::move(id), std::move(title))
                .Then(std::move(callback));
          },
          &store_, std::move(id), std::move(title),
          std::move(success_callback)),
      base::BindOnce(std::move(failure_callback), false));
}

void ArkAIService::UpdateConversationModel(std::string id,
                                           std::string model_name,
                                           ResultCallback callback) {
  auto [success_callback, failure_callback] =
      base::SplitOnceCallback(std::move(callback));
  RunWhenReady(
      base::BindOnce(
          [](base::SequenceBound<ConversationStore>* store, std::string id,
             std::string model_name, ResultCallback callback) {
            store->AsyncCall(&ConversationStore::UpdateConversationModel)
                .WithArgs(std::move(id), std::move(model_name))
                .Then(std::move(callback));
          },
          &store_, std::move(id), std::move(model_name),
          std::move(success_callback)),
      base::BindOnce(std::move(failure_callback), false));
}

void ArkAIService::SaveDraft(std::string id,
                             std::string draft,
                             ResultCallback callback) {
  auto [success_callback, failure_callback] =
      base::SplitOnceCallback(std::move(callback));
  RunWhenReady(
      base::BindOnce(
          [](base::SequenceBound<ConversationStore>* store, std::string id,
             std::string draft, ResultCallback callback) {
            store->AsyncCall(&ConversationStore::SaveDraft)
                .WithArgs(std::move(id), std::move(draft))
                .Then(std::move(callback));
          },
          &store_, std::move(id), std::move(draft),
          std::move(success_callback)),
      base::BindOnce(std::move(failure_callback), false));
}

void ArkAIService::SendChatPrompt(
    std::string conversation_id,
    std::string message,
    std::optional<std::string> image_data,
    std::optional<std::string> provider_credential,
    PromptCallback callback) {
  auto [success_callback, failure_callback] =
      base::SplitOnceCallback(std::move(callback));
  RunWhenReady(
      base::BindOnce(
          &ArkAIService::BeginChatPrompt, weak_ptr_factory_.GetWeakPtr(),
          std::move(conversation_id), std::move(message), std::move(image_data),
          std::move(provider_credential), std::move(success_callback)),
      base::BindOnce(std::move(failure_callback),
                     "Chat service failed to initialize.", false));
}

void ArkAIService::BeginChatPrompt(
    std::string conversation_id,
    std::string message,
    std::optional<std::string> image_data,
    std::optional<std::string> provider_credential,
    PromptCallback callback) {
  if (active_chat_conversation_ids_.contains(conversation_id)) {
    std::move(callback).Run(
        "This conversation is already generating a response.", false);
    return;
  }
  active_chat_conversation_ids_.insert(conversation_id);
  store_.AsyncCall(&ConversationStore::GetConversation)
      .WithArgs(conversation_id)
      .Then(base::BindOnce(
          &ArkAIService::OnConversationReady, weak_ptr_factory_.GetWeakPtr(),
          std::move(conversation_id), std::move(message), std::move(image_data),
          std::move(provider_credential), std::move(callback)));
}

void ArkAIService::OnConversationReady(
    std::string conversation_id,
    std::string message,
    std::optional<std::string> image_data,
    std::optional<std::string> provider_credential,
    PromptCallback callback,
    ConversationState conversation) {
  if (conversation.id.empty()) {
    active_chat_conversation_ids_.erase(conversation_id);
    std::move(callback).Run("Conversation not found.", false);
    return;
  }
  const std::string model_name = conversation.model_name.empty()
                                     ? "local:mlx:llama-3.2-11b-vision-instruct"
                                     : conversation.model_name;
  store_.AsyncCall(&ConversationStore::GetMessages)
      .WithArgs(conversation_id)
      .Then(base::BindOnce(
          [](base::WeakPtr<ArkAIService> self, std::string conversation_id,
             std::string message, std::optional<std::string> image_data,
             std::optional<std::string> provider_credential,
             std::string model_name, PromptCallback callback,
             std::vector<ChatMessage> history) {
            if (!self) {
              return;
            }
            const bool accepted = self->DispatchPrompt(
                conversation_id, model_name, message, std::move(image_data),
                std::move(provider_credential), std::move(history),
                base::BindOnce(&ArkAIService::OnPromptCompleted, self,
                               conversation_id, model_name,
                               std::move(callback)));
            if (accepted) {
              self->store_.AsyncCall(&ConversationStore::AddMessage)
                  .WithArgs(conversation_id, "user", message, model_name)
                  .Then(base::BindOnce([](bool) {}));
            }
          },
          weak_ptr_factory_.GetWeakPtr(), std::move(conversation_id),
          std::move(message), std::move(image_data),
          std::move(provider_credential), model_name, std::move(callback)));
}

bool ArkAIService::DispatchPrompt(
    std::string request_key,
    std::string model_name,
    std::string prompt,
    std::optional<std::string> image_data,
    std::optional<std::string> provider_credential,
    std::vector<ChatMessage> history,
    PromptCallback callback) {
  if (base::StartsWith(model_name, "cloud:gemini")) {
    if (!cloud_inference_service_) {
      std::move(callback).Run("Cloud inference service unavailable.", false);
      return false;
    }
    return cloud_inference_service_->SendPrompt(model_name, prompt, image_data,
                                                history, provider_credential,
                                                std::move(callback));
  }
  if (!inference_service_) {
    std::move(callback).Run("Local inference service unavailable.", false);
    return false;
  }
  return inference_service_->SendPrompt(request_key, model_name, prompt,
                                        image_data, history,
                                        std::move(callback));
}

void ArkAIService::GenerateConversationTitle(
    std::string conversation_id,
    std::string user_message,
    std::optional<std::string> provider_credential,
    PromptCallback callback) {
  auto [success_callback, failure_callback] =
      base::SplitOnceCallback(std::move(callback));
  RunWhenReady(
      base::BindOnce(
          [](ArkAIService* self, std::string conversation_id,
             std::string user_message,
             std::optional<std::string> provider_credential,
             PromptCallback callback) {
            self->store_.AsyncCall(&ConversationStore::GetConversation)
                .WithArgs(conversation_id)
                .Then(base::BindOnce(
                    [](ArkAIService* ai, std::string cid, std::string message,
                       std::optional<std::string> provider_credential,
                       PromptCallback title_callback,
                       ConversationState conversation) {
                      const std::string model_name =
                          conversation.model_name.empty()
                              ? "local:mlx:llama-3.2-11b-vision-instruct"
                              : conversation.model_name;
                      const std::string prompt =
                          "Create a short chat title based only on the user's "
                          "message below. Return only 2 to 6 words, with no "
                          "quotes, punctuation, or explanation.\n\nUser "
                          "message:\n" +
                          message;
                      ai->DispatchPrompt(cid + ":title", model_name, prompt,
                                         std::nullopt,
                                         std::move(provider_credential), {},
                                         std::move(title_callback));
                    },
                    self, std::move(conversation_id), std::move(user_message),
                    std::move(provider_credential), std::move(callback)));
          },
          this, std::move(conversation_id), std::move(user_message),
          std::move(provider_credential), std::move(success_callback)),
      base::BindOnce(std::move(failure_callback),
                     "Chat service failed to initialize.", false));
}

void ArkAIService::OnPromptCompleted(std::string conversation_id,
                                     std::string model_name,
                                     PromptCallback callback,
                                     const std::string& response,
                                     bool success) {
  if (success && !response.empty()) {
    store_.AsyncCall(&ConversationStore::AddMessage)
        .WithArgs(conversation_id, "assistant", response, std::move(model_name))
        .Then(base::BindOnce(
            [](base::WeakPtr<ArkAIService> self, std::string conversation_id,
               PromptCallback callback, std::string response, bool stored) {
              if (!self) {
                return;
              }
              self->active_chat_conversation_ids_.erase(conversation_id);
              if (!stored) {
                std::move(callback).Run(
                    "Ark generated a response but could not save it.", false);
                return;
              }
              std::move(callback).Run(response, true);
            },
            weak_ptr_factory_.GetWeakPtr(), std::move(conversation_id),
            std::move(callback), response));
    return;
  }
  active_chat_conversation_ids_.erase(conversation_id);
  std::move(callback).Run(response, success);
}

void ArkAIService::SearchLocalModels(std::string query,
                                     ArkModelManager::SearchCallback callback) {
  model_manager_->Search(std::move(query), std::move(callback));
}

void ArkAIService::GetLocalModelState(ArkModelManager::StateCallback callback) {
  model_manager_->GetLocalModelState(std::move(callback));
}

void ArkAIService::GetInstalledLocalModels(
    ArkModelManager::InstalledCallback callback) {
  model_manager_->GetInstalledModels(std::move(callback));
}

void ArkAIService::StartLocalModelDownload(
    std::string repository,
    bool license_accepted,
    ArkModelManager::StateCallback callback) {
  model_manager_->StartDownload(std::move(repository), license_accepted,
                                std::move(callback));
}

void ArkAIService::PauseLocalModelDownload(
    ArkModelManager::StateCallback callback) {
  model_manager_->PauseDownload(std::move(callback));
}

void ArkAIService::ResumeLocalModelDownload(
    ArkModelManager::StateCallback callback) {
  model_manager_->ResumeDownload(std::move(callback));
}

void ArkAIService::DeleteLocalModel(std::string model_id,
                                    ArkModelManager::StateCallback callback) {
  if (inference_service_) {
    inference_service_->StopServer();
  }
  model_manager_->DeleteModel(std::move(model_id), std::move(callback));
}

void ArkAIService::OnInitialized(bool success) {
  if (!success) {
    store_.AsyncCall(&ConversationStore::Init)
        .WithArgs(base::FilePath(), base::FilePath(), /*in_memory=*/true)
        .Then(base::BindOnce(
            [](base::WeakPtr<ArkAIService> self, bool in_mem_success) {
              if (!self) {
                return;
              }
              self->initializing_ = false;
              self->ready_ = in_mem_success;
              auto operations = std::move(self->pending_operations_);
              for (auto& operation : operations) {
                std::move(operation).Run();
              }
            },
            weak_ptr_factory_.GetWeakPtr()));
    return;
  }
  initializing_ = false;
  ready_ = success;
  auto operations = std::move(pending_operations_);
  for (auto& operation : operations) {
    std::move(operation).Run();
  }
}

void ArkAIService::RunWhenReady(base::OnceClosure operation,
                                base::OnceClosure failure) {
  if (initializing_) {
    pending_operations_.push_back(base::BindOnce(
        &ArkAIService::RunWhenReady, weak_ptr_factory_.GetWeakPtr(),
        std::move(operation), std::move(failure)));
  } else if (ready_) {
    std::move(operation).Run();
  } else {
    std::move(failure).Run();
  }
}

}  // namespace ark
