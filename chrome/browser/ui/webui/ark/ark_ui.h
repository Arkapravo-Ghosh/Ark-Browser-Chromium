// Copyright 2026 Arkapravo Ghosh
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_UI_WEBUI_ARK_ARK_UI_H_
#define CHROME_BROWSER_UI_WEBUI_ARK_ARK_UI_H_

#include "base/memory/weak_ptr.h"
#include "chrome/browser/ark/ark_conversation_store.h"
#include "chrome/browser/ui/webui/ark/ark.mojom.h"
#include "content/public/browser/webui_config.h"
#include "mojo/public/cpp/bindings/receiver.h"
#include "ui/webui/mojo_web_ui_controller.h"

namespace network {
class SimpleURLLoader;
}

class ArkUI;

class ArkUIConfig : public content::DefaultWebUIConfig<ArkUI> {
 public:
  ArkUIConfig();
  bool IsWebUIEnabled(content::BrowserContext* browser_context) override;
};

class ArkUI : public ui::MojoWebUIController, public ark::mojom::PageHandler {
 public:
  explicit ArkUI(content::WebUI* web_ui);
  ~ArkUI() override;
  void BindInterface(mojo::PendingReceiver<ark::mojom::PageHandler> receiver);

 private:
  void Navigate(const std::string& text, NavigateCallback callback) override;
  void GetSearchSuggestions(const std::string& query,
                            GetSearchSuggestionsCallback callback) override;
  void OnSuggestionsLoaded(std::optional<std::string> response_body);
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
  std::unique_ptr<network::SimpleURLLoader> suggestions_loader_;
  GetSearchSuggestionsCallback pending_suggestions_callback_;
  mojo::Receiver<ark::mojom::PageHandler> receiver_{this};
  base::WeakPtrFactory<ArkUI> weak_ptr_factory_{this};
  WEB_UI_CONTROLLER_TYPE_DECL();
};

#endif  // CHROME_BROWSER_UI_WEBUI_ARK_ARK_UI_H_
