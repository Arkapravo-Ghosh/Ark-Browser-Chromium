// Copyright 2026 Arkapravo Ghosh
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_ARK_ARK_PATHS_H_
#define CHROME_BROWSER_ARK_ARK_PATHS_H_

#include "base/files/file_path.h"

namespace ark {

// The sole resolver for Ark-owned AI data outside Chromium's browsing profile.
// Callers receive paths only; potentially blocking directory creation belongs
// on the caller's background sequence.
class ArkPaths {
 public:
  ArkPaths() = delete;

  static bool GetRoot(base::FilePath* path);
  static bool GetConfigDirectory(base::FilePath* path);
  static bool GetDataDirectory(base::FilePath* path);
  static bool GetModelsDirectory(base::FilePath* path);
  static bool GetCacheDirectory(base::FilePath* path);
  static bool GetArtifactsDirectory(base::FilePath* path);
  static bool GetLogsDirectory(base::FilePath* path);
  static bool GetRuntimeDirectory(base::FilePath* path);
  static bool GetConversationsDatabase(base::FilePath* path);
  static bool GetModelsDatabase(base::FilePath* path);
};

}  // namespace ark

#endif  // CHROME_BROWSER_ARK_ARK_PATHS_H_
