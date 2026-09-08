// Copyright 2026 Arkapravo Ghosh
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_UI_VIEWS_SIDE_PANEL_ARK_ARK_SIDE_PANEL_COORDINATOR_H_
#define CHROME_BROWSER_UI_VIEWS_SIDE_PANEL_ARK_ARK_SIDE_PANEL_COORDINATOR_H_

#include <memory>

#include "ui/base/unowned_user_data/scoped_unowned_user_data.h"

class BrowserWindowInterface;
class SidePanelEntryScope;
class SidePanelRegistry;

namespace views {
class View;
}

class ArkSidePanelCoordinator {
 public:
  explicit ArkSidePanelCoordinator(BrowserWindowInterface& browser);
  ~ArkSidePanelCoordinator();

  static ArkSidePanelCoordinator* From(BrowserWindowInterface* browser);
  DECLARE_USER_DATA(ArkSidePanelCoordinator);

  void CreateAndRegisterEntry(SidePanelRegistry* registry);

 private:
  std::unique_ptr<views::View> CreateWebView(SidePanelEntryScope& scope);

  ui::ScopedUnownedUserData<ArkSidePanelCoordinator> scoped_user_data_;
};

#endif  // CHROME_BROWSER_UI_VIEWS_SIDE_PANEL_ARK_ARK_SIDE_PANEL_COORDINATOR_H_
