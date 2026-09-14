// Copyright 2026 Arkapravo Ghosh
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/ark/ark_paths.h"

#include "base/base_paths.h"
#include "base/path_service.h"

namespace ark {

namespace {

bool GetChild(const base::FilePath::CharType* child, base::FilePath* path) {
  if (!path || !ArkPaths::GetRoot(path)) {
    return false;
  }
  *path = path->Append(child);
  return true;
}

}  // namespace

// static
bool ArkPaths::GetRoot(base::FilePath* path) {
  if (!path || !base::PathService::Get(base::DIR_HOME, path) || path->empty()) {
    return false;
  }
  *path = path->Append(FILE_PATH_LITERAL(".arkbrowser"));
  return true;
}

// static
bool ArkPaths::GetConfigDirectory(base::FilePath* path) {
  return GetChild(FILE_PATH_LITERAL("config"), path);
}

// static
bool ArkPaths::GetDataDirectory(base::FilePath* path) {
  return GetChild(FILE_PATH_LITERAL("data"), path);
}

// static
bool ArkPaths::GetModelsDirectory(base::FilePath* path) {
  return GetChild(FILE_PATH_LITERAL("models"), path);
}

// static
bool ArkPaths::GetCacheDirectory(base::FilePath* path) {
  return GetChild(FILE_PATH_LITERAL("cache"), path);
}

// static
bool ArkPaths::GetArtifactsDirectory(base::FilePath* path) {
  return GetChild(FILE_PATH_LITERAL("artifacts"), path);
}

// static
bool ArkPaths::GetLogsDirectory(base::FilePath* path) {
  return GetChild(FILE_PATH_LITERAL("logs"), path);
}

// static
bool ArkPaths::GetRuntimeDirectory(base::FilePath* path) {
  return GetChild(FILE_PATH_LITERAL("runtime"), path);
}

// static
bool ArkPaths::GetConversationsDatabase(base::FilePath* path) {
  if (!GetDataDirectory(path)) {
    return false;
  }
  *path = path->Append(FILE_PATH_LITERAL("conversations.sqlite3"));
  return true;
}

// static
bool ArkPaths::GetModelsDatabase(base::FilePath* path) {
  if (!GetDataDirectory(path)) {
    return false;
  }
  *path = path->Append(FILE_PATH_LITERAL("models.sqlite3"));
  return true;
}

}  // namespace ark
