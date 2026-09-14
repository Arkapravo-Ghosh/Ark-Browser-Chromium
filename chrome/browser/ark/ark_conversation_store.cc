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
    "model_name TEXT NOT NULL DEFAULT '',"
    "created_at INTEGER NOT NULL,"
    "updated_at INTEGER NOT NULL)";

constexpr char kCreateMessagesSql[] =
    "CREATE TABLE IF NOT EXISTS messages("
    "id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "conversation_id TEXT NOT NULL REFERENCES conversations(id) ON DELETE "
    "CASCADE,"
    "role TEXT NOT NULL CHECK(role IN ('user','assistant','tool','system')),"
    "content TEXT NOT NULL,"
    "model_name TEXT NOT NULL DEFAULT '',"
    "created_at INTEGER NOT NULL)";

}  // namespace

ConversationStore::ConversationStore() = default;
ConversationStore::~ConversationStore() = default;

bool ConversationStore::Init(const base::FilePath& path,
                             const base::FilePath& fallback_path,
                             bool in_memory) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (in_memory) {
    db_.Close();
    if (!db_.OpenInMemory()) {
      return false;
    }
  } else {
    bool opened = false;
    if (!path.empty() && base::CreateDirectory(path.DirName())) {
      opened = db_.Open(path);
    }
    if (!opened) {
      db_.Close();
      if (!fallback_path.empty() &&
          base::CreateDirectory(fallback_path.DirName())) {
        opened = db_.Open(fallback_path);
      }
    }
    if (!opened) {
      db_.Close();
      opened = db_.OpenInMemory();
    }
    if (!opened) {
      return false;
    }
  }
  if (!db_.Execute("PRAGMA foreign_keys=ON") ||
      !db_.Execute(kCreateConversationsSql) ||
      !db_.Execute(kCreateMessagesSql)) {
    return false;
  }
  if (!db_.DoesColumnExist("conversations", "model_name")) {
    if (!db_.Execute(
            "ALTER TABLE conversations ADD COLUMN model_name TEXT NOT NULL "
            "DEFAULT ''")) {
      return false;
    }
  }
  if (!db_.DoesColumnExist("messages", "model_name")) {
    if (!db_.Execute(
            "ALTER TABLE messages ADD COLUMN model_name TEXT NOT NULL "
            "DEFAULT ''")) {
      return false;
    }
  }
  return true;
}

ConversationState ConversationStore::GetOrCreateCurrent() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  sql::Statement statement(db_.GetUniqueStatement(
      "SELECT id,title,draft,model_name FROM conversations ORDER BY updated_at DESC, "
      "rowid DESC "
      "LIMIT 1"));
  if (statement.Step()) {
    return {.id = statement.ColumnString(0),
            .title = statement.ColumnString(1),
            .draft = statement.ColumnString(2),
            .model_name = statement.ColumnString(3)};
  }
  return InsertConversation();
}

std::vector<ConversationState> ConversationStore::GetConversations() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  std::vector<ConversationState> conversations;
  sql::Statement statement(db_.GetUniqueStatement(
      "SELECT id,title,draft,model_name FROM conversations ORDER BY updated_at DESC, "
      "rowid DESC"));
  while (statement.Step()) {
    conversations.push_back({
        .id = statement.ColumnString(0),
        .title = statement.ColumnString(1),
        .draft = statement.ColumnString(2),
        .model_name = statement.ColumnString(3),
    });
  }
  return conversations;
}

ConversationState ConversationStore::CreateConversation(
    const std::string& model_name) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  return InsertConversation(model_name);
}

ConversationState ConversationStore::GetConversation(const std::string& id) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  sql::Statement statement(db_.GetUniqueStatement(
      "SELECT id,title,draft,model_name FROM conversations WHERE id=? LIMIT 1"));
  statement.BindString(0, id);
  if (statement.Step()) {
    return {.id = statement.ColumnString(0),
            .title = statement.ColumnString(1),
            .draft = statement.ColumnString(2),
            .model_name = statement.ColumnString(3)};
  }
  return ConversationState();
}

ConversationState ConversationStore::SwitchConversation(const std::string& id) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  sql::Statement update_stmt(db_.GetUniqueStatement(
      "UPDATE conversations SET updated_at=? WHERE id=?"));
  update_stmt.BindInt64(0, base::Time::Now().InMillisecondsSinceUnixEpoch());
  update_stmt.BindString(1, id);
  update_stmt.Run();
  return GetConversation(id);
}

bool ConversationStore::DeleteConversation(const std::string& id) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  sql::Statement del_msgs(db_.GetUniqueStatement(
      "DELETE FROM messages WHERE conversation_id=?"));
  del_msgs.BindString(0, id);
  del_msgs.Run();

  sql::Statement del_conv(db_.GetUniqueStatement(
      "DELETE FROM conversations WHERE id=?"));
  del_conv.BindString(0, id);
  return del_conv.Run();
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

bool ConversationStore::UpdateConversationTitle(const std::string& id,
                                                const std::string& title) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  sql::Statement statement(db_.GetUniqueStatement(
      "UPDATE conversations SET title=?,updated_at=? WHERE id=?"));
  statement.BindString(0, title);
  statement.BindInt64(1, base::Time::Now().InMillisecondsSinceUnixEpoch());
  statement.BindString(2, id);
  return statement.Run() && db_.GetLastChangeCount() == 1;
}

bool ConversationStore::UpdateConversationModel(const std::string& id,
                                                const std::string& model_name) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  sql::Statement statement(db_.GetUniqueStatement(
      "UPDATE conversations SET model_name=?,updated_at=? WHERE id=?"));
  statement.BindString(0, model_name);
  statement.BindInt64(1, base::Time::Now().InMillisecondsSinceUnixEpoch());
  statement.BindString(2, id);
  return statement.Run() && db_.GetLastChangeCount() == 1;
}

bool ConversationStore::AddMessage(const std::string& conversation_id,
                                   const std::string& role,
                                   const std::string& content,
                                   const std::string& model_name) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  sql::Statement statement(db_.GetUniqueStatement(
      "INSERT INTO messages(conversation_id,role,content,model_name,created_at) "
      "VALUES(?,?,?,?,?)"));
  statement.BindString(0, conversation_id);
  statement.BindString(1, role);
  statement.BindString(2, content);
  statement.BindString(3, model_name);
  statement.BindInt64(4, base::Time::Now().InMillisecondsSinceUnixEpoch());
  if (!statement.Run()) {
    return false;
  }

  const int64_t now = base::Time::Now().InMillisecondsSinceUnixEpoch();
  if (role == "user") {
    std::string snippet = content.substr(0, 60);
    if (content.size() > 60) {
      snippet += "...";
    }
    sql::Statement update_stmt(db_.GetUniqueStatement(
        "UPDATE conversations SET title=?,updated_at=? WHERE id=? AND "
        "(title='New conversation' OR title='')"));
    update_stmt.BindString(0, snippet);
    update_stmt.BindInt64(1, now);
    update_stmt.BindString(2, conversation_id);
    update_stmt.Run();
  } else {
    sql::Statement update_stmt(db_.GetUniqueStatement(
        "UPDATE conversations SET updated_at=? WHERE id=?"));
    update_stmt.BindInt64(0, now);
    update_stmt.BindString(1, conversation_id);
    update_stmt.Run();
  }

  return true;
}

std::vector<ChatMessage> ConversationStore::GetMessages(
    const std::string& conversation_id) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  std::vector<ChatMessage> messages;
  sql::Statement statement(db_.GetUniqueStatement(
      "SELECT id,conversation_id,role,content,created_at,model_name FROM messages "
      "WHERE conversation_id=? ORDER BY id ASC"));
  statement.BindString(0, conversation_id);
  while (statement.Step()) {
    messages.push_back({
        .id = statement.ColumnInt64(0),
        .conversation_id = statement.ColumnString(1),
        .role = statement.ColumnString(2),
        .content = statement.ColumnString(3),
        .created_at = statement.ColumnInt64(4),
        .model_name = statement.ColumnString(5),
    });
  }
  return messages;
}

ConversationState ConversationStore::InsertConversation(
    const std::string& model_name) {
  ConversationState result{
      .id = base::Uuid::GenerateRandomV4().AsLowercaseString(),
      .title = "New conversation",
      .draft = "",
      .model_name = model_name,
  };
  const int64_t now = base::Time::Now().InMillisecondsSinceUnixEpoch();
  sql::Statement statement(db_.GetUniqueStatement(
      "INSERT INTO conversations(id,title,draft,model_name,created_at,updated_at) "
      "VALUES(?,?,?,?,?,?)"));
  statement.BindString(0, result.id);
  statement.BindString(1, result.title);
  statement.BindString(2, result.draft);
  statement.BindString(3, result.model_name);
  statement.BindInt64(4, now);
  statement.BindInt64(5, now);
  return statement.Run() ? result : ConversationState();
}

}  // namespace ark
