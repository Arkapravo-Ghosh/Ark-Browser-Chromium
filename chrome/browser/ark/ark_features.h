// Copyright 2026 Arkapravo Ghosh
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_ARK_ARK_FEATURES_H_
#define CHROME_BROWSER_ARK_ARK_FEATURES_H_

#include "base/feature_list.h"

namespace ark {
BASE_DECLARE_FEATURE(kArkUI);
inline constexpr char kArkChatHost[] = "ark-chat";
inline constexpr char kArkChatURL[] = "chrome://ark-chat/";
inline constexpr char kArkNewTabURL[] = "chrome://ark-chat/#home";
inline constexpr char kArkSidePanelHost[] = "ark-side-panel";
inline constexpr char kArkSidePanelURL[] = "chrome://ark-side-panel/#chat";
}  // namespace ark

#endif  // CHROME_BROWSER_ARK_ARK_FEATURES_H_
