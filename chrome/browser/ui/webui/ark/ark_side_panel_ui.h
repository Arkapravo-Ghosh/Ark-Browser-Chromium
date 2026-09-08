// Copyright 2026 Arkapravo Ghosh
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_UI_WEBUI_ARK_ARK_SIDE_PANEL_UI_H_
#define CHROME_BROWSER_UI_WEBUI_ARK_ARK_SIDE_PANEL_UI_H_

#include "base/memory/weak_ptr.h"
#include "chrome/browser/ark/ark_conversation_store.h"
#include "chrome/browser/ui/webui/ark/ark.mojom.h"
#include "chrome/browser/ui/webui/top_chrome/top_chrome_web_ui_controller.h"
#include "chrome/browser/ui/webui/top_chrome/top_chrome_webui_config.h"
#include "mojo/public/cpp/bindings/receiver.h"

class ArkSidePanelUI;

class ArkSidePanelUIConfig
    : public DefaultTopChromeWebUIConfig<ArkSidePanelUI> {
 public:
  ArkSidePanelUIConfig();
  bool IsWebUIEnabled(content::BrowserContext* browser_context) override;
};

class ArkSidePanelUI : public TopChromeWebUIController,
                       public ark::mojom::PageHandler {
 public:
  explicit ArkSidePanelUI(content::WebUI* web_ui);
  ~ArkSidePanelUI() override;

  static constexpr std::string_view GetWebUIName() { return "ArkSidePanel"; }
  void BindInterface(mojo::PendingReceiver<ark::mojom::PageHandler> receiver);

  // TopChromeWebUIController:
  void WebUIPrimaryPageChanged(content::Page& page) override;

 private:
  void Navigate(const std::string& text, NavigateCallback callback) override;
  void GetSearchSuggestions(const std::string& query,
                            GetSearchSuggestionsCallback callback) override;
  void GetChatState(GetChatStateCallback callback) override;
  void CreateConversation(CreateConversationCallback callback) override;
  void SaveDraft(const std::string& conversation_id,
                 const std::string& draft,
                 SaveDraftCallback callback) override;
  void OpenSidebarWithDraft(const std::string& draft,
                            OpenSidebarWithDraftCallback callback) override;
  void OnCurrentConversationForSidebar(std::string draft,
                                       OpenSidebarWithDraftCallback callback,
                                       ark::ConversationState state);
  void OnDraftSavedForSidebar(OpenSidebarWithDraftCallback callback,
                              bool success);

  mojo::Receiver<ark::mojom::PageHandler> receiver_{this};
  base::WeakPtrFactory<ArkSidePanelUI> weak_ptr_factory_{this};
  WEB_UI_CONTROLLER_TYPE_DECL();
};

#endif  // CHROME_BROWSER_UI_WEBUI_ARK_ARK_SIDE_PANEL_UI_H_
