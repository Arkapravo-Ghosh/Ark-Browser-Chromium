// Copyright 2026 Arkapravo Ghosh
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/ui/webui/ark/ark_side_panel_ui.h"

#include <utility>

#include "base/feature_list.h"
#include "base/functional/bind.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "chrome/browser/ark/ark_ai_service.h"
#include "chrome/browser/ark/ark_ai_service_factory.h"
#include "chrome/browser/ark/ark_features.h"
#include "chrome/browser/autocomplete/autocomplete_classifier_factory.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/browser_window/public/global_browser_collection.h"
#include "chrome/browser/ui/side_panel/side_panel_entry_key.h"
#include "chrome/browser/ui/side_panel/side_panel_ui.h"
#include "chrome/grit/ark_resources.h"
#include "chrome/grit/ark_resources_map.h"
#include "components/omnibox/browser/autocomplete_classifier.h"
#include "components/omnibox/browser/autocomplete_match.h"
#include "content/public/browser/navigation_controller.h"
#include "content/public/browser/web_contents.h"
#include "content/public/browser/web_ui.h"
#include "content/public/browser/web_ui_data_source.h"
#include "content/public/common/url_constants.h"
#include "services/network/public/mojom/content_security_policy.mojom.h"
#include "ui/base/page_transition_types.h"
#include "ui/webui/webui_util.h"

namespace {

ark::mojom::ConversationStatePtr ToMojom(ark::ConversationState state) {
  return ark::mojom::ConversationState::New(state.id, state.title, state.draft,
                                            state.model_name);
}

std::vector<ark::mojom::ConversationStatePtr> ToMojom(
    std::vector<ark::ConversationState> states) {
  std::vector<ark::mojom::ConversationStatePtr> converted;
  converted.reserve(states.size());
  for (auto& state : states) {
    converted.push_back(ToMojom(std::move(state)));
  }
  return converted;
}

ark::mojom::ChatMessagePtr ToMojom(ark::ChatMessage message) {
  return ark::mojom::ChatMessage::New(message.id, message.conversation_id,
                                      message.role, message.content,
                                      message.created_at, message.model_name);
}

std::vector<ark::mojom::ChatMessagePtr> ToMojom(
    std::vector<ark::ChatMessage> messages) {
  std::vector<ark::mojom::ChatMessagePtr> converted;
  converted.reserve(messages.size());
  for (auto& message : messages) {
    converted.push_back(ToMojom(std::move(message)));
  }
  return converted;
}

ark::mojom::LocalModelStatePtr ToMojom(ark::LocalModelState state) {
  return ark::mojom::LocalModelState::New(
      state.model_id, state.display_name, state.variant,
      state.runtime_backend, state.state,
      state.detail, state.bytes_downloaded, state.bytes_total, state.can_start,
      state.can_pause, state.can_resume, state.installed,
      state.runtime_compatible);
}

std::vector<ark::mojom::ModelSearchResultPtr> ToMojom(
    std::vector<ark::ModelSearchResult> results) {
  std::vector<ark::mojom::ModelSearchResultPtr> converted;
  converted.reserve(results.size());
  for (auto& result : results) {
    converted.push_back(ark::mojom::ModelSearchResult::New(
        std::move(result.id), std::move(result.runtime_backend),
        result.downloads, result.gated, result.prepared));
  }
  return converted;
}

std::vector<ark::mojom::InstalledLocalModelPtr> ToMojom(
    std::vector<ark::InstalledLocalModel> models) {
  std::vector<ark::mojom::InstalledLocalModelPtr> converted;
  converted.reserve(models.size());
  for (auto& model : models) {
    converted.push_back(ark::mojom::InstalledLocalModel::New(
        std::move(model.model_id), std::move(model.display_name),
        std::move(model.repository), std::move(model.revision),
        std::move(model.variant), std::move(model.runtime_backend),
        model.bytes_total,
        model.runtime_compatible));
  }
  return converted;
}

}  // namespace

ArkSidePanelUIConfig::ArkSidePanelUIConfig()
    : DefaultTopChromeWebUIConfig(content::kChromeUIScheme,
                                  ark::kArkSidePanelHost) {}

bool ArkSidePanelUIConfig::IsWebUIEnabled(
    content::BrowserContext* browser_context) {
  return base::FeatureList::IsEnabled(ark::kArkUI);
}

ArkSidePanelUI::ArkSidePanelUI(content::WebUI* web_ui)
    : TopChromeWebUIController(web_ui) {
  auto* source = content::WebUIDataSource::CreateAndAdd(
      web_ui->GetWebContents()->GetBrowserContext(), ark::kArkSidePanelHost);
  webui::SetupWebUIDataSource(source, kArkResources, IDR_ARK_ARK_HTML);
  source->DisableTrustedTypesCSP();
  source->OverrideContentSecurityPolicy(
      network::mojom::CSPDirectiveName::ConnectSrc,
      "connect-src 'self' https://generativelanguage.googleapis.com "
      "https://huggingface.co http://127.0.0.1:* http://localhost:*;");
}

ArkSidePanelUI::~ArkSidePanelUI() = default;

void ArkSidePanelUI::BindInterface(
    mojo::PendingReceiver<ark::mojom::PageHandler> receiver) {
  receivers_.Add(this, std::move(receiver));
  if (embedder()) {
    embedder()->ShowUI();
  }
}

void ArkSidePanelUI::WebUIPrimaryPageChanged(content::Page& page) {
  TopChromeWebUIController::WebUIPrimaryPageChanged(page);
  if (embedder()) {
    embedder()->ShowUI();
  }
}

void ArkSidePanelUI::Navigate(const std::string& input,
                              NavigateCallback callback) {
  if (input.size() > 16384) {
    std::move(callback).Run(false);
    return;
  }
  const std::u16string text(
      base::TrimWhitespace(base::UTF8ToUTF16(input), base::TRIM_ALL));
  auto* classifier = AutocompleteClassifierFactory::GetForProfile(
      Profile::FromWebUI(web_ui()));
  if (text.empty() || text.size() > 4096 || !classifier) {
    std::move(callback).Run(false);
    return;
  }
  AutocompleteMatch match;
  classifier->Classify(text, false, false, metrics::OmniboxEventProto::NTP,
                       &match, nullptr);
  if (!match.destination_url.is_valid() ||
      !match.destination_url.SchemeIsHTTPOrHTTPS()) {
    std::move(callback).Run(false);
    return;
  }
  std::move(callback).Run(true);
  content::NavigationController::LoadURLParams params(match.destination_url);
  params.transition_type = match.transition;
  web_ui()->GetWebContents()->GetController().LoadURLWithParams(params);
}

void ArkSidePanelUI::GetSearchSuggestions(
    const std::string& query,
    GetSearchSuggestionsCallback callback) {
  std::move(callback).Run({});
}

void ArkSidePanelUI::GetChatState(GetChatStateCallback callback) {
  ark::ArkAIServiceFactory::GetForProfile(Profile::FromWebUI(web_ui()))
      ->GetChatState(base::BindOnce(
          [](GetChatStateCallback callback, ark::ConversationState state) {
            std::move(callback).Run(ToMojom(std::move(state)));
          },
          std::move(callback)));
}

void ArkSidePanelUI::GetConversations(GetConversationsCallback callback) {
  ark::ArkAIServiceFactory::GetForProfile(Profile::FromWebUI(web_ui()))
      ->GetConversations(base::BindOnce(
          [](GetConversationsCallback callback,
             std::vector<ark::ConversationState> conversations) {
            std::move(callback).Run(ToMojom(std::move(conversations)));
          },
          std::move(callback)));
}

void ArkSidePanelUI::CreateConversation(const std::string& model_name,
                                         CreateConversationCallback callback) {
  ark::ArkAIServiceFactory::GetForProfile(Profile::FromWebUI(web_ui()))
      ->CreateConversation(
          model_name,
          base::BindOnce(
              [](CreateConversationCallback callback,
                 ark::ConversationState state) {
                std::move(callback).Run(ToMojom(std::move(state)));
              },
              std::move(callback)));
}

void ArkSidePanelUI::SwitchConversation(const std::string& conversation_id,
                                        SwitchConversationCallback callback) {
  if (conversation_id.empty()) {
    std::move(callback).Run(ark::mojom::ConversationState::New());
    return;
  }
  ark::ArkAIServiceFactory::GetForProfile(Profile::FromWebUI(web_ui()))
      ->SwitchConversation(
          conversation_id,
          base::BindOnce(
              [](SwitchConversationCallback callback,
                 ark::ConversationState state) {
                std::move(callback).Run(ToMojom(std::move(state)));
              },
              std::move(callback)));
}

void ArkSidePanelUI::DeleteConversation(const std::string& conversation_id,
                                        DeleteConversationCallback callback) {
  if (conversation_id.empty()) {
    std::move(callback).Run(false);
    return;
  }
  ark::ArkAIServiceFactory::GetForProfile(Profile::FromWebUI(web_ui()))
      ->DeleteConversation(conversation_id, std::move(callback));
}

void ArkSidePanelUI::GetMessages(const std::string& conversation_id,
                                 GetMessagesCallback callback) {
  if (conversation_id.empty()) {
    std::move(callback).Run({});
    return;
  }
  ark::ArkAIServiceFactory::GetForProfile(Profile::FromWebUI(web_ui()))
      ->GetMessages(conversation_id,
                    base::BindOnce(
                        [](GetMessagesCallback callback,
                           std::vector<ark::ChatMessage> messages) {
                          std::move(callback).Run(ToMojom(std::move(messages)));
                        },
                        std::move(callback)));
}

void ArkSidePanelUI::AddMessage(const std::string& conversation_id,
                                const std::string& role,
                                const std::string& content,
                                const std::string& model_name,
                                AddMessageCallback callback) {
  if (conversation_id.empty() || role.empty() || content.empty() ||
      content.size() > 65536) {
    std::move(callback).Run(false);
    return;
  }
  ark::ArkAIServiceFactory::GetForProfile(Profile::FromWebUI(web_ui()))
      ->AddMessage(conversation_id, role, content, model_name,
                   std::move(callback));
}

void ArkSidePanelUI::UpdateConversationTitle(
    const std::string& conversation_id,
    const std::string& title,
    UpdateConversationTitleCallback callback) {
  if (conversation_id.empty() || title.size() > 500) {
    std::move(callback).Run(false);
    return;
  }
  ark::ArkAIServiceFactory::GetForProfile(Profile::FromWebUI(web_ui()))
      ->UpdateConversationTitle(conversation_id, title, std::move(callback));
}

void ArkSidePanelUI::UpdateConversationModel(
    const std::string& conversation_id,
    const std::string& model_name,
    UpdateConversationModelCallback callback) {
  if (conversation_id.empty()) {
    std::move(callback).Run(false);
    return;
  }
  ark::ArkAIServiceFactory::GetForProfile(Profile::FromWebUI(web_ui()))
      ->UpdateConversationModel(conversation_id, model_name,
                                std::move(callback));
}

void ArkSidePanelUI::SaveDraft(const std::string& conversation_id,
                               const std::string& draft,
                               SaveDraftCallback callback) {
  if (conversation_id.empty() || draft.size() > 32000) {
    std::move(callback).Run(false);
    return;
  }
  ark::ArkAIServiceFactory::GetForProfile(Profile::FromWebUI(web_ui()))
      ->SaveDraft(conversation_id, draft, std::move(callback));
}

void ArkSidePanelUI::SearchLocalModels(const std::string& query,
                                       SearchLocalModelsCallback callback) {
  ark::ArkAIServiceFactory::GetForProfile(Profile::FromWebUI(web_ui()))
      ->SearchLocalModels(query,
                          base::BindOnce(
                              [](SearchLocalModelsCallback callback,
                                 std::vector<ark::ModelSearchResult> results,
                                 std::string error) {
                                std::move(callback).Run(
                                    ToMojom(std::move(results)), error);
                              },
                              std::move(callback)));
}

void ArkSidePanelUI::GetLocalModelState(GetLocalModelStateCallback callback) {
  ark::ArkAIServiceFactory::GetForProfile(Profile::FromWebUI(web_ui()))
      ->GetLocalModelState(base::BindOnce(
          [](GetLocalModelStateCallback callback, ark::LocalModelState state) {
            std::move(callback).Run(ToMojom(std::move(state)));
          },
          std::move(callback)));
}

void ArkSidePanelUI::GetInstalledLocalModels(
    GetInstalledLocalModelsCallback callback) {
  ark::ArkAIServiceFactory::GetForProfile(Profile::FromWebUI(web_ui()))
      ->GetInstalledLocalModels(base::BindOnce(
          [](GetInstalledLocalModelsCallback callback,
             std::vector<ark::InstalledLocalModel> models) {
            std::move(callback).Run(ToMojom(std::move(models)));
          },
          std::move(callback)));
}

void ArkSidePanelUI::StartLocalModelDownload(
    const std::string& repository,
    bool license_accepted,
    StartLocalModelDownloadCallback callback) {
  ark::ArkAIServiceFactory::GetForProfile(Profile::FromWebUI(web_ui()))
      ->StartLocalModelDownload(
          repository, license_accepted,
          base::BindOnce(
              [](StartLocalModelDownloadCallback callback,
                 ark::LocalModelState state) {
                std::move(callback).Run(ToMojom(std::move(state)));
              },
              std::move(callback)));
}

void ArkSidePanelUI::PauseLocalModelDownload(
    PauseLocalModelDownloadCallback callback) {
  ark::ArkAIServiceFactory::GetForProfile(Profile::FromWebUI(web_ui()))
      ->PauseLocalModelDownload(base::BindOnce(
          [](PauseLocalModelDownloadCallback callback,
             ark::LocalModelState state) {
            std::move(callback).Run(ToMojom(std::move(state)));
          },
          std::move(callback)));
}

void ArkSidePanelUI::ResumeLocalModelDownload(
    ResumeLocalModelDownloadCallback callback) {
  ark::ArkAIServiceFactory::GetForProfile(Profile::FromWebUI(web_ui()))
      ->ResumeLocalModelDownload(base::BindOnce(
          [](ResumeLocalModelDownloadCallback callback,
             ark::LocalModelState state) {
            std::move(callback).Run(ToMojom(std::move(state)));
          },
          std::move(callback)));
}

void ArkSidePanelUI::DeleteLocalModel(
    const std::string& model_id,
    DeleteLocalModelCallback callback) {
  ark::ArkAIServiceFactory::GetForProfile(Profile::FromWebUI(web_ui()))
      ->DeleteLocalModel(model_id, base::BindOnce(
          [](DeleteLocalModelCallback callback, ark::LocalModelState state) {
            std::move(callback).Run(ToMojom(std::move(state)));
          },
          std::move(callback)));
}

void ArkSidePanelUI::SendChatPrompt(
    const std::string& conversation_id,
    const std::string& message,
    const std::optional<std::string>& image_data,
    SendChatPromptCallback callback) {
  ark::ArkAIServiceFactory::GetForProfile(Profile::FromWebUI(web_ui()))
      ->SendChatPrompt(conversation_id, message, image_data,
                       std::move(callback));
}

void ArkSidePanelUI::GenerateConversationTitle(
    const std::string& conversation_id,
    const std::string& user_message,
    GenerateConversationTitleCallback callback) {
  ark::ArkAIServiceFactory::GetForProfile(Profile::FromWebUI(web_ui()))
      ->GenerateConversationTitle(conversation_id, user_message,
                                  std::move(callback));
}

void ArkSidePanelUI::OpenSidebarWithDraft(
    const std::string& draft,
    OpenSidebarWithDraftCallback callback) {
  if (draft.empty() || draft.size() > 32000) {
    std::move(callback).Run(false);
    return;
  }
  ark::ArkAIServiceFactory::GetForProfile(Profile::FromWebUI(web_ui()))
      ->GetChatState(base::BindOnce(
          &ArkSidePanelUI::OnCurrentConversationForSidebar,
          weak_ptr_factory_.GetWeakPtr(), draft, std::move(callback)));
}

void ArkSidePanelUI::OnCurrentConversationForSidebar(
    std::string draft,
    OpenSidebarWithDraftCallback callback,
    ark::ConversationState state) {
  if (state.id.empty()) {
    std::move(callback).Run(false);
    return;
  }
  ark::ArkAIServiceFactory::GetForProfile(Profile::FromWebUI(web_ui()))
      ->SaveDraft(
          state.id, std::move(draft),
          base::BindOnce(&ArkSidePanelUI::OnDraftSavedForSidebar,
                         weak_ptr_factory_.GetWeakPtr(), std::move(callback)));
}

void ArkSidePanelUI::OnDraftSavedForSidebar(
    OpenSidebarWithDraftCallback callback,
    bool success) {
  if (success) {
    if (auto* browser =
            GlobalBrowserCollection::GetInstance()->FindBrowserWithTab(
                web_ui()->GetWebContents())) {
      if (auto* side_panel = SidePanelUI::From(browser)) {
        side_panel->Show(SidePanelEntryId::kArkAi,
                         SidePanelOpenTrigger::kNewTabPage);
      }
    }
  }
  std::move(callback).Run(success);
}

WEB_UI_CONTROLLER_TYPE_IMPL(ArkSidePanelUI)
