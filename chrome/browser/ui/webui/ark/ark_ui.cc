// Copyright 2026 Arkapravo Ghosh
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/ui/webui/ark/ark_ui.h"

#include <utility>

#include "base/feature_list.h"
#include "base/functional/bind.h"
#include "base/json/json_reader.h"
#include "base/strings/escape.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "base/values.h"
#include "chrome/browser/ark/ark_ai_service.h"
#include "chrome/browser/ark/ark_ai_service_factory.h"
#include "chrome/browser/ark/ark_features.h"
#include "chrome/browser/autocomplete/autocomplete_classifier_factory.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/search_engines/template_url_service_factory.h"
#include "chrome/browser/ui/browser_window/public/global_browser_collection.h"
#include "chrome/browser/ui/side_panel/side_panel_entry_key.h"
#include "chrome/browser/ui/side_panel/side_panel_ui.h"
#include "chrome/common/ark_url_constants.h"
#include "chrome/grit/ark_resources.h"
#include "chrome/grit/ark_resources_map.h"
#include "components/omnibox/browser/autocomplete_classifier.h"
#include "components/omnibox/browser/autocomplete_match.h"
#include "components/omnibox/browser/search_suggestion_parser.h"
#include "components/search_engines/template_url_service.h"
#include "content/public/browser/navigation_controller.h"
#include "content/public/browser/web_contents.h"
#include "content/public/browser/web_ui.h"
#include "content/public/browser/web_ui_data_source.h"
#include "content/public/common/url_constants.h"
#include "net/base/load_flags.h"
#include "net/cookies/site_for_cookies.h"
#include "net/traffic_annotation/network_traffic_annotation.h"
#include "services/network/public/cpp/resource_request.h"
#include "services/network/public/cpp/shared_url_loader_factory.h"
#include "services/network/public/cpp/simple_url_loader.h"
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

std::vector<std::string> ParseGoogleSuggestions(const std::string& json_str) {
  std::vector<std::string> suggestions;
  std::optional<base::ListValue> root =
      SearchSuggestionParser::DeserializeJsonData(json_str);
  if (!root || root->size() < 2) {
    return suggestions;
  }
  const auto& second_elem = (*root)[1];
  if (!second_elem.is_list()) {
    return suggestions;
  }
  const base::ListValue& items = second_elem.GetList();
  for (const auto& item : items) {
    if (item.is_string()) {
      suggestions.push_back(item.GetString());
    }
  }
  return suggestions;
}

}  // namespace

ArkUIConfig::ArkUIConfig()
    : DefaultWebUIConfig(content::kChromeUIScheme, ark::kArkChatHost) {}

bool ArkUIConfig::IsWebUIEnabled(content::BrowserContext* browser_context) {
  return base::FeatureList::IsEnabled(ark::kArkUI);
}

ArkUI::ArkUI(content::WebUI* web_ui) : ui::MojoWebUIController(web_ui) {
  auto* source = content::WebUIDataSource::CreateAndAdd(
      web_ui->GetWebContents()->GetBrowserContext(), ark::kArkChatHost);
  webui::SetupWebUIDataSource(source, kArkResources, IDR_ARK_ARK_HTML);
  source->DisableTrustedTypesCSP();
  source->OverrideContentSecurityPolicy(
      network::mojom::CSPDirectiveName::ConnectSrc,
      "connect-src 'self' https://generativelanguage.googleapis.com "
      "https://huggingface.co http://127.0.0.1:* http://localhost:*;");
}

ArkUI::~ArkUI() {
  if (pending_suggestions_callback_) {
    std::move(pending_suggestions_callback_).Run({});
  }
}

void ArkUI::BindInterface(
    mojo::PendingReceiver<ark::mojom::PageHandler> receiver) {
  receivers_.Add(this, std::move(receiver));
}

void ArkUI::Navigate(const std::string& input, NavigateCallback callback) {
  if (input.size() > 16384) {
    std::move(callback).Run(false);
    return;
  }
  const std::u16string text(
      base::TrimWhitespace(base::UTF8ToUTF16(input), base::TRIM_ALL));
  if (text.empty() || text.size() > 4096) {
    std::move(callback).Run(false);
    return;
  }
  auto* classifier = AutocompleteClassifierFactory::GetForProfile(
      Profile::FromWebUI(web_ui()));
  if (!classifier) {
    std::move(callback).Run(false);
    return;
  }
  AutocompleteMatch match;
  classifier->Classify(text, /*in_keyword_mode=*/false,
                       /*allow_exact_keyword_match=*/false,
                       metrics::OmniboxEventProto::NTP, &match, nullptr);
  // Never execute script/data URLs, open local files, or dispatch external
  // protocols from this renderer-originated input.
  const bool is_allowed_scheme =
      match.destination_url.SchemeIsHTTPOrHTTPS() ||
      match.destination_url.SchemeIs(ark::kUIScheme) ||
      match.destination_url.SchemeIs(content::kChromeUIScheme);
  if (!match.destination_url.is_valid() || !is_allowed_scheme) {
    std::move(callback).Run(false);
    return;
  }
  std::move(callback).Run(true);
  content::NavigationController::LoadURLParams params(match.destination_url);
  params.transition_type = match.transition;
  web_ui()->GetWebContents()->GetController().LoadURLWithParams(params);
}

void ArkUI::GetSearchSuggestions(const std::string& input,
                                 GetSearchSuggestionsCallback callback) {
  if (suggestions_loader_) {
    suggestions_loader_.reset();
  }
  if (pending_suggestions_callback_) {
    std::move(pending_suggestions_callback_).Run({});
  }

  std::string trimmed_query = base::UTF16ToUTF8(
      base::TrimWhitespace(base::UTF8ToUTF16(input), base::TRIM_ALL));
  if (trimmed_query.empty() || trimmed_query.size() > 1024) {
    std::move(callback).Run({});
    return;
  }

  pending_suggestions_callback_ = std::move(callback);

  GURL suggest_url;
  auto* profile = Profile::FromWebUI(web_ui());
  auto* turl_service = TemplateURLServiceFactory::GetForProfile(profile);
  const TemplateURL* default_provider =
      turl_service ? turl_service->GetDefaultSearchProvider() : nullptr;

  if (default_provider && turl_service) {
    if (default_provider->GetEngineType(turl_service->search_terms_data()) ==
        SearchEngineType::SEARCH_ENGINE_GOOGLE) {
      suggest_url = GURL(
          "https://suggestqueries.google.com/complete/search?client=chrome&q=" +
          base::EscapeQueryParamValue(trimmed_query, true));
    } else {
      TemplateURLRef::SearchTermsArgs args(base::UTF8ToUTF16(trimmed_query));
      args.page_classification = metrics::OmniboxEventProto::NTP_REALBOX;
      std::string url_str =
          default_provider->suggestions_url_ref().ReplaceSearchTerms(
              args, turl_service->search_terms_data());
      if (!url_str.empty()) {
        suggest_url = GURL(url_str);
      }
    }
  }

  if (!suggest_url.is_valid() || suggest_url.is_empty()) {
    suggest_url = GURL(
        "https://suggestqueries.google.com/complete/search?client=chrome&q=" +
        base::EscapeQueryParamValue(trimmed_query, true));
  }

  net::NetworkTrafficAnnotationTag traffic_annotation =
      net::DefineNetworkTrafficAnnotation("ark_search_suggestions", R"(
        semantics {
          sender: "Ark Browser New Tab Search"
          description: "Fetches search suggestions for user query on new tab."
          trigger: "User types into the search box on the Ark New Tab Page."
          data: "Search query text."
          destination: GOOGLE_OWNED_SERVICE
          internal {
            contacts {
              email: "ark@arkbrowser.internal"
            }
          }
          user_data {
            type: SEARCH_TERMS
          }
          last_reviewed: "2026-09-08"
        }
        policy {
          cookies_allowed: NO
          setting: "Users can change default search engine."
          policy_exception_justification: "Not implemented."
        })");

  auto request = std::make_unique<network::ResourceRequest>();
  request->url = suggest_url;
  request->method = "GET";
  request->load_flags = net::LOAD_DO_NOT_SAVE_COOKIES;
  request->site_for_cookies = net::SiteForCookies::FromUrl(suggest_url);

  suggestions_loader_ =
      network::SimpleURLLoader::Create(std::move(request), traffic_annotation);
  constexpr size_t kMaxDownloadBytes = 64 * 1024;
  suggestions_loader_->DownloadToString(
      profile->GetURLLoaderFactory().get(),
      base::BindOnce(&ArkUI::OnSuggestionsLoaded,
                     weak_ptr_factory_.GetWeakPtr()),
      kMaxDownloadBytes);
}

void ArkUI::OnSuggestionsLoaded(std::optional<std::string> response_body) {
  if (!pending_suggestions_callback_) {
    suggestions_loader_.reset();
    return;
  }

  auto callback = std::move(pending_suggestions_callback_);
  std::vector<std::string> results;
  if (response_body) {
    results = ParseGoogleSuggestions(*response_body);
  }
  std::move(callback).Run(std::move(results));
  suggestions_loader_.reset();
}

void ArkUI::GetChatState(GetChatStateCallback callback) {
  ark::ArkAIServiceFactory::GetForProfile(Profile::FromWebUI(web_ui()))
      ->GetChatState(base::BindOnce(
          [](GetChatStateCallback callback, ark::ConversationState state) {
            std::move(callback).Run(ToMojom(std::move(state)));
          },
          std::move(callback)));
}

void ArkUI::GetConversations(GetConversationsCallback callback) {
  ark::ArkAIServiceFactory::GetForProfile(Profile::FromWebUI(web_ui()))
      ->GetConversations(base::BindOnce(
          [](GetConversationsCallback callback,
             std::vector<ark::ConversationState> conversations) {
            std::move(callback).Run(ToMojom(std::move(conversations)));
          },
          std::move(callback)));
}

void ArkUI::CreateConversation(const std::string& model_name,
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

void ArkUI::SwitchConversation(const std::string& conversation_id,
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

void ArkUI::DeleteConversation(const std::string& conversation_id,
                               DeleteConversationCallback callback) {
  if (conversation_id.empty()) {
    std::move(callback).Run(false);
    return;
  }
  ark::ArkAIServiceFactory::GetForProfile(Profile::FromWebUI(web_ui()))
      ->DeleteConversation(conversation_id, std::move(callback));
}

void ArkUI::GetMessages(const std::string& conversation_id,
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

void ArkUI::AddMessage(const std::string& conversation_id,
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

void ArkUI::UpdateConversationTitle(
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

void ArkUI::UpdateConversationModel(
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

void ArkUI::SaveDraft(const std::string& conversation_id,
                      const std::string& draft,
                      SaveDraftCallback callback) {
  if (conversation_id.empty() || draft.size() > 32000) {
    std::move(callback).Run(false);
    return;
  }
  ark::ArkAIServiceFactory::GetForProfile(Profile::FromWebUI(web_ui()))
      ->SaveDraft(conversation_id, draft, std::move(callback));
}

void ArkUI::SearchLocalModels(const std::string& query,
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

void ArkUI::GetLocalModelState(GetLocalModelStateCallback callback) {
  ark::ArkAIServiceFactory::GetForProfile(Profile::FromWebUI(web_ui()))
      ->GetLocalModelState(base::BindOnce(
          [](GetLocalModelStateCallback callback, ark::LocalModelState state) {
            std::move(callback).Run(ToMojom(std::move(state)));
          },
          std::move(callback)));
}

void ArkUI::GetInstalledLocalModels(
    GetInstalledLocalModelsCallback callback) {
  ark::ArkAIServiceFactory::GetForProfile(Profile::FromWebUI(web_ui()))
      ->GetInstalledLocalModels(base::BindOnce(
          [](GetInstalledLocalModelsCallback callback,
             std::vector<ark::InstalledLocalModel> models) {
            std::move(callback).Run(ToMojom(std::move(models)));
          },
          std::move(callback)));
}

void ArkUI::StartLocalModelDownload(const std::string& repository,
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

void ArkUI::PauseLocalModelDownload(PauseLocalModelDownloadCallback callback) {
  ark::ArkAIServiceFactory::GetForProfile(Profile::FromWebUI(web_ui()))
      ->PauseLocalModelDownload(base::BindOnce(
          [](PauseLocalModelDownloadCallback callback,
             ark::LocalModelState state) {
            std::move(callback).Run(ToMojom(std::move(state)));
          },
          std::move(callback)));
}

void ArkUI::ResumeLocalModelDownload(
    ResumeLocalModelDownloadCallback callback) {
  ark::ArkAIServiceFactory::GetForProfile(Profile::FromWebUI(web_ui()))
      ->ResumeLocalModelDownload(base::BindOnce(
          [](ResumeLocalModelDownloadCallback callback,
             ark::LocalModelState state) {
            std::move(callback).Run(ToMojom(std::move(state)));
          },
          std::move(callback)));
}

void ArkUI::DeleteLocalModel(const std::string& model_id,
                             DeleteLocalModelCallback callback) {
  ark::ArkAIServiceFactory::GetForProfile(Profile::FromWebUI(web_ui()))
      ->DeleteLocalModel(model_id, base::BindOnce(
          [](DeleteLocalModelCallback callback, ark::LocalModelState state) {
            std::move(callback).Run(ToMojom(std::move(state)));
          },
          std::move(callback)));
}

void ArkUI::SendChatPrompt(const std::string& conversation_id,
                           const std::string& message,
                           const std::optional<std::string>& image_data,
                           SendChatPromptCallback callback) {
  ark::ArkAIServiceFactory::GetForProfile(Profile::FromWebUI(web_ui()))
      ->SendChatPrompt(conversation_id, message, image_data,
                       std::move(callback));
}

void ArkUI::GenerateConversationTitle(
    const std::string& conversation_id,
    const std::string& user_message,
    GenerateConversationTitleCallback callback) {
  ark::ArkAIServiceFactory::GetForProfile(Profile::FromWebUI(web_ui()))
      ->GenerateConversationTitle(conversation_id, user_message,
                                   std::move(callback));
}

void ArkUI::OpenSidebarWithDraft(const std::string& draft,
                                 OpenSidebarWithDraftCallback callback) {
  if (draft.empty() || draft.size() > 32000) {
    std::move(callback).Run(false);
    return;
  }
  ark::ArkAIServiceFactory::GetForProfile(Profile::FromWebUI(web_ui()))
      ->GetChatState(base::BindOnce(&ArkUI::OnCurrentConversationForSidebar,
                                    weak_ptr_factory_.GetWeakPtr(), draft,
                                    std::move(callback)));
}

void ArkUI::OnCurrentConversationForSidebar(
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
          base::BindOnce(&ArkUI::OnDraftSavedForSidebar,
                         weak_ptr_factory_.GetWeakPtr(), std::move(callback)));
}

void ArkUI::OnDraftSavedForSidebar(OpenSidebarWithDraftCallback callback,
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

WEB_UI_CONTROLLER_TYPE_IMPL(ArkUI)
