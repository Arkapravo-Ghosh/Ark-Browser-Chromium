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
#include "ui/base/page_transition_types.h"
#include "ui/webui/webui_util.h"

namespace {

ark::mojom::ConversationStatePtr ToMojom(ark::ConversationState state) {
  return ark::mojom::ConversationState::New(state.id, state.title, state.draft);
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
}

ArkSidePanelUI::~ArkSidePanelUI() = default;

void ArkSidePanelUI::BindInterface(
    mojo::PendingReceiver<ark::mojom::PageHandler> receiver) {
  receiver_.reset();
  receiver_.Bind(std::move(receiver));
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

void ArkSidePanelUI::CreateConversation(CreateConversationCallback callback) {
  ark::ArkAIServiceFactory::GetForProfile(Profile::FromWebUI(web_ui()))
      ->CreateConversation(base::BindOnce(
          [](CreateConversationCallback callback,
             ark::ConversationState state) {
            std::move(callback).Run(ToMojom(std::move(state)));
          },
          std::move(callback)));
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
