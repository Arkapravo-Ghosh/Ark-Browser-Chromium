// Copyright 2026 Arkapravo Ghosh
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_ARK_ARK_CONVERSATION_STORE_H_
#define CHROME_BROWSER_ARK_ARK_CONVERSATION_STORE_H_

#include <string>

#include "base/files/file_path.h"
#include "base/sequence_checker.h"
#include "sql/database.h"

namespace ark {

struct ConversationState {
  std::string id;
  std::string title;
  std::string draft;
};

// Owns Ark's local conversation database and runs only on its DB sequence.
class ConversationStore {
 public:
  ConversationStore();
  ~ConversationStore();

  bool Init(const base::FilePath& path, bool in_memory);
  ConversationState GetOrCreateCurrent();
  ConversationState CreateConversation();
  bool SaveDraft(const std::string& id, const std::string& draft);

 private:
  ConversationState InsertConversation();

  sql::Database db_{sql::DatabaseOptions().set_wal_mode(true),
                    sql::Database::Tag("ArkAI")};
  SEQUENCE_CHECKER(sequence_checker_);
};

}  // namespace ark

#endif  // CHROME_BROWSER_ARK_ARK_CONVERSATION_STORE_H_
