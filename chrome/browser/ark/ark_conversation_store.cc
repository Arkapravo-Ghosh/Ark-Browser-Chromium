// Copyright 2026 Arkapravo Ghosh
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/ark/ark_conversation_store.h"

#include "base/files/file_util.h"
#include "base/time/time.h"
#include "base/uuid.h"
#include "sql/statement.h"

namespace ark {

namespace {

constexpr char kCreateConversationsSql[] =
    "CREATE TABLE IF NOT EXISTS conversations("
    "id TEXT PRIMARY KEY NOT NULL,"
    "title TEXT NOT NULL,"
    "draft TEXT NOT NULL DEFAULT '',"
    "created_at INTEGER NOT NULL,"
    "updated_at INTEGER NOT NULL)";

constexpr char kCreateMessagesSql[] =
    "CREATE TABLE IF NOT EXISTS messages("
    "id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "conversation_id TEXT NOT NULL REFERENCES conversations(id) ON DELETE "
    "CASCADE,"
    "role TEXT NOT NULL CHECK(role IN ('user','assistant','tool','system')),"
    "content TEXT NOT NULL,"
    "created_at INTEGER NOT NULL)";

}  // namespace

ConversationStore::ConversationStore() = default;
ConversationStore::~ConversationStore() = default;

bool ConversationStore::Init(const base::FilePath& path, bool in_memory) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (!in_memory && !base::CreateDirectory(path.DirName())) {
    return false;
  }
  if (!(in_memory ? db_.OpenInMemory() : db_.Open(path))) {
    return false;
  }
  return db_.Execute("PRAGMA foreign_keys=ON") &&
         db_.Execute(kCreateConversationsSql) &&
         db_.Execute(kCreateMessagesSql);
}

ConversationState ConversationStore::GetOrCreateCurrent() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  sql::Statement statement(db_.GetUniqueStatement(
      "SELECT id,title,draft FROM conversations ORDER BY updated_at DESC, "
      "rowid DESC "
      "LIMIT 1"));
  if (statement.Step()) {
    return {.id = statement.ColumnString(0),
            .title = statement.ColumnString(1),
            .draft = statement.ColumnString(2)};
  }
  return InsertConversation();
}

ConversationState ConversationStore::CreateConversation() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  return InsertConversation();
}

bool ConversationStore::SaveDraft(const std::string& id,
                                  const std::string& draft) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  sql::Statement statement(db_.GetUniqueStatement(
      "UPDATE conversations SET draft=?,updated_at=? WHERE id=?"));
  statement.BindString(0, draft);
  statement.BindInt64(1, base::Time::Now().InMillisecondsSinceUnixEpoch());
  statement.BindString(2, id);
  return statement.Run() && db_.GetLastChangeCount() == 1;
}

ConversationState ConversationStore::InsertConversation() {
  ConversationState result{
      .id = base::Uuid::GenerateRandomV4().AsLowercaseString(),
      .title = "New conversation",
      .draft = "",
  };
  const int64_t now = base::Time::Now().InMillisecondsSinceUnixEpoch();
  sql::Statement statement(db_.GetUniqueStatement(
      "INSERT INTO conversations(id,title,draft,created_at,updated_at) "
      "VALUES(?,?,?,?,?)"));
  statement.BindString(0, result.id);
  statement.BindString(1, result.title);
  statement.BindString(2, result.draft);
  statement.BindInt64(3, now);
  statement.BindInt64(4, now);
  return statement.Run() ? result : ConversationState();
}

}  // namespace ark
