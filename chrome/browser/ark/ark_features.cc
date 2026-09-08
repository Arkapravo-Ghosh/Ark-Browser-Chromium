// Copyright 2026 Arkapravo Ghosh
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/ark/ark_features.h"

namespace ark {
// Keep the UI foundation independently reversible during upstream development.
BASE_FEATURE(kArkUI, base::FEATURE_ENABLED_BY_DEFAULT);
}  // namespace ark
