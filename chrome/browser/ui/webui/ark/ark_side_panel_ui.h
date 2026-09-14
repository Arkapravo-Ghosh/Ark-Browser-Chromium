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
#include "mojo/public/cpp/bindings/receiver_set.h"

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
  void GetConversations(GetConversationsCallback callback) override;
  void CreateConversation(const std::string& model_name,
                          CreateConversationCallback callback) override;
  void SwitchConversation(const std::string& conversation_id,
                          SwitchConversationCallback callback) override;
  void DeleteConversation(const std::string& conversation_id,
                          DeleteConversationCallback callback) override;
  void GetMessages(const std::string& conversation_id,
                   GetMessagesCallback callback) override;
  void AddMessage(const std::string& conversation_id,
                  const std::string& role,
                  const std::string& content,
                  const std::string& model_name,
                  AddMessageCallback callback) override;
  void UpdateConversationTitle(
      const std::string& conversation_id,
      const std::string& title,
      UpdateConversationTitleCallback callback) override;
  void UpdateConversationModel(
      const std::string& conversation_id,
      const std::string& model_name,
      UpdateConversationModelCallback callback) override;
  void SaveDraft(const std::string& conversation_id,
                 const std::string& draft,
                 SaveDraftCallback callback) override;
  void SearchLocalModels(const std::string& query,
                         SearchLocalModelsCallback callback) override;
  void GetLocalModelState(GetLocalModelStateCallback callback) override;
  void StartLocalModelDownload(
      bool license_accepted,
      StartLocalModelDownloadCallback callback) override;
  void PauseLocalModelDownload(
      PauseLocalModelDownloadCallback callback) override;
  void ResumeLocalModelDownload(
      ResumeLocalModelDownloadCallback callback) override;
  void DeleteLocalModel(DeleteLocalModelCallback callback) override;
  void SendChatPrompt(const std::string& conversation_id,
                      const std::string& message,
                      const std::optional<std::string>& image_data,
                      SendChatPromptCallback callback) override;
  void OpenSidebarWithDraft(const std::string& draft,
                            OpenSidebarWithDraftCallback callback) override;
  void OnCurrentConversationForSidebar(std::string draft,
                                       OpenSidebarWithDraftCallback callback,
                                       ark::ConversationState state);
  void OnDraftSavedForSidebar(OpenSidebarWithDraftCallback callback,
                              bool success);

  mojo::ReceiverSet<ark::mojom::PageHandler> receivers_;
  base::WeakPtrFactory<ArkSidePanelUI> weak_ptr_factory_{this};
  WEB_UI_CONTROLLER_TYPE_DECL();
};

#endif  // CHROME_BROWSER_UI_WEBUI_ARK_ARK_SIDE_PANEL_UI_H_
