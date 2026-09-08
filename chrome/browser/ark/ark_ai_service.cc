// Copyright 2026 Arkapravo Ghosh
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/ark/ark_ai_service.h"

#include <utility>

#include "base/functional/bind.h"
#include "base/functional/callback_helpers.h"
#include "base/task/thread_pool.h"

namespace ark {

namespace {
constexpr base::FilePath::CharType kDatabasePath[] =
    FILE_PATH_LITERAL("ArkAI/conversations.sqlite3");
}  // namespace

ArkAIService::ArkAIService(const base::FilePath& profile_path, bool in_memory)
    : store_(base::ThreadPool::CreateSequencedTaskRunnerForResource(
          {base::MayBlock(), base::TaskPriority::USER_VISIBLE,
           base::TaskShutdownBehavior::BLOCK_SHUTDOWN},
          profile_path.Append(kDatabasePath))) {
  store_.AsyncCall(&ConversationStore::Init)
      .WithArgs(profile_path.Append(kDatabasePath), in_memory)
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

void ArkAIService::CreateConversation(StateCallback callback) {
  auto [success_callback, failure_callback] =
      base::SplitOnceCallback(std::move(callback));
  RunWhenReady(
      base::BindOnce(
          [](base::SequenceBound<ConversationStore>* store,
             StateCallback callback) {
            store->AsyncCall(&ConversationStore::CreateConversation)
                .Then(std::move(callback));
          },
          &store_, std::move(success_callback)),
      base::BindOnce(std::move(failure_callback), ConversationState()));
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

void ArkAIService::OnInitialized(bool success) {
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
