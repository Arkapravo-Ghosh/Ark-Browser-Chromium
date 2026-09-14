// Copyright 2026 Arkapravo Ghosh
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_ARK_ARK_CONVERSATION_STORE_H_
#define CHROME_BROWSER_ARK_ARK_CONVERSATION_STORE_H_

#include <cstdint>
#include <string>
#include <vector>

#include "base/files/file_path.h"
#include "base/sequence_checker.h"
#include "sql/database.h"

namespace ark {

struct ConversationState {
  std::string id;
  std::string title;
  std::string draft;
  std::string model_name;
};

struct ChatMessage {
  int64_t id = 0;
  std::string conversation_id;
  std::string role;
  std::string content;
  int64_t created_at = 0;
  std::string model_name;
};

// Owns Ark's local conversation database and runs only on its DB sequence.
class ConversationStore {
 public:
  ConversationStore();
  ~ConversationStore();

  bool Init(const base::FilePath& path,
            const base::FilePath& fallback_path,
            bool in_memory);
  ConversationState GetOrCreateCurrent();
  std::vector<ConversationState> GetConversations();
  ConversationState CreateConversation(const std::string& model_name = "");
  ConversationState GetConversation(const std::string& id);
  ConversationState SwitchConversation(const std::string& id);
  bool DeleteConversation(const std::string& id);
  bool SaveDraft(const std::string& id, const std::string& draft);
  bool UpdateConversationTitle(const std::string& id, const std::string& title);
  bool UpdateConversationModel(const std::string& id,
                               const std::string& model_name);
  bool AddMessage(const std::string& conversation_id,
                  const std::string& role,
                  const std::string& content,
                  const std::string& model_name = "");
  std::vector<ChatMessage> GetMessages(const std::string& conversation_id);

 private:
  ConversationState InsertConversation(const std::string& model_name = "");

  sql::Database db_{sql::DatabaseOptions().set_wal_mode(true),
                    sql::Database::Tag("ArkAI")};
  SEQUENCE_CHECKER(sequence_checker_);
};

}  // namespace ark

#endif  // CHROME_BROWSER_ARK_ARK_CONVERSATION_STORE_H_
