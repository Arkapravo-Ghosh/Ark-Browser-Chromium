// Copyright 2026 Arkapravo Ghosh
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_COMMON_ARK_VERSION_CONSTANTS_H_
#define CHROME_COMMON_ARK_VERSION_CONSTANTS_H_

namespace ark {

// Current Ark Browser release version tag.
inline constexpr char kArkVersion[] = "155.0.8049.0-alpha.0.0.14";

// Current Ark Browser channel (e.g. "stable", "beta", "canary").
inline constexpr char kArkChannel[] = "stable";

// Default remote manifest URL for checking updates.
// Can be overridden via command-line switch --ark-update-manifest-url.
inline constexpr char kArkDefaultUpdateManifestURL[] =
    "https://raw.githubusercontent.com/Arkapravo-Ghosh/Ark-Browser/main/release/version.json";

}  // namespace ark

#endif  // CHROME_COMMON_ARK_VERSION_CONSTANTS_H_
