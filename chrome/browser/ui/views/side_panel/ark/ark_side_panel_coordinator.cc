// Copyright 2026 Arkapravo Ghosh
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/ui/views/side_panel/ark/ark_side_panel_coordinator.h"

#include <memory>

#include "base/functional/bind.h"
#include "chrome/browser/ark/ark_features.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/side_panel/side_panel_entry.h"
#include "chrome/browser/ui/side_panel/side_panel_registry.h"
#include "chrome/browser/ui/views/side_panel/side_panel_web_ui_view.h"
#include "chrome/browser/ui/webui/ark/ark_side_panel_ui.h"
#include "chrome/grit/generated_resources.h"
#include "ui/base/metadata/metadata_impl_macros.h"

using SidePanelWebUIViewT_ArkSidePanelUI = SidePanelWebUIViewT<ArkSidePanelUI>;
BEGIN_TEMPLATE_METADATA(SidePanelWebUIViewT_ArkSidePanelUI, SidePanelWebUIViewT)
END_METADATA

DEFINE_USER_DATA(ArkSidePanelCoordinator);

ArkSidePanelCoordinator* ArkSidePanelCoordinator::From(
    BrowserWindowInterface* browser) {
  return browser ? Get(browser->GetUnownedUserDataHost()) : nullptr;
}

ArkSidePanelCoordinator::ArkSidePanelCoordinator(
    BrowserWindowInterface& browser)
    : scoped_user_data_(browser.GetUnownedUserDataHost(), *this) {}

ArkSidePanelCoordinator::~ArkSidePanelCoordinator() = default;

void ArkSidePanelCoordinator::CreateAndRegisterEntry(
    SidePanelRegistry* registry) {
  auto entry = std::make_unique<SidePanelEntry>(
      SidePanelEntry::Key(SidePanelEntry::Id::kArkAi),
      base::BindRepeating(&ArkSidePanelCoordinator::CreateWebView,
                          base::Unretained(this)),
      base::BindRepeating([] { return GURL(ark::kArkChatURL); }),
      base::NullCallback(), base::BindRepeating([] { return 420; }));
  entry->set_should_show_header(false);
  entry->set_should_show_ephemerally_in_toolbar(false);
  registry->Register(std::move(entry));
}

std::unique_ptr<views::View> ArkSidePanelCoordinator::CreateWebView(
    SidePanelEntryScope& scope) {
  auto view = std::make_unique<SidePanelWebUIViewT<ArkSidePanelUI>>(
      scope, base::RepeatingClosure(), base::RepeatingClosure(),
      std::make_unique<WebUIContentsWrapperT<ArkSidePanelUI>>(
          GURL(ark::kArkSidePanelURL),
          scope.GetBrowserWindowInterface().GetProfile(), IDS_ARK_AI_TITLE,
          /*esc_closes_ui=*/false));
  view->ShowUI();
  return view;
}
