// Copyright 2026 Arkapravo Ghosh
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_COMMON_ARK_URL_CONSTANTS_H_
#define CHROME_COMMON_ARK_URL_CONSTANTS_H_

namespace ark {
// User-facing alias. Chromium's canonical WebUI origins and resource schemes
// stay unchanged so existing controllers, CSP, and privilege checks still apply.
inline constexpr char kUIScheme[] = "ark";
}  // namespace ark

#endif  // CHROME_COMMON_ARK_URL_CONSTANTS_H_
