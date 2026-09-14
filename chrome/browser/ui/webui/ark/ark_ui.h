// Copyright 2026 Arkapravo Ghosh
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_UI_WEBUI_ARK_ARK_UI_H_
#define CHROME_BROWSER_UI_WEBUI_ARK_ARK_UI_H_

#include "base/memory/weak_ptr.h"
#include "chrome/browser/ark/ark_conversation_store.h"
#include "chrome/browser/ui/webui/ark/ark.mojom.h"
#include "content/public/browser/webui_config.h"
#include "mojo/public/cpp/bindings/receiver_set.h"
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
  std::unique_ptr<network::SimpleURLLoader> suggestions_loader_;
  GetSearchSuggestionsCallback pending_suggestions_callback_;
  mojo::ReceiverSet<ark::mojom::PageHandler> receivers_;
  base::WeakPtrFactory<ArkUI> weak_ptr_factory_{this};
  WEB_UI_CONTROLLER_TYPE_DECL();
};

#endif  // CHROME_BROWSER_UI_WEBUI_ARK_ARK_UI_H_
