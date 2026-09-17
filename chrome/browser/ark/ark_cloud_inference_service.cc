// Copyright 2026 Arkapravo Ghosh
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/ark/ark_cloud_inference_service.h"

#include <utility>

#include "base/functional/bind.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/strings/strcat.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "base/values.h"
#include "chrome/browser/profiles/profile.h"
#include "net/base/load_flags.h"
#include "net/http/http_response_headers.h"
#include "net/traffic_annotation/network_traffic_annotation.h"
#include "services/network/public/cpp/resource_request.h"
#include "services/network/public/cpp/shared_url_loader_factory.h"
#include "services/network/public/cpp/simple_url_loader.h"
#include "services/network/public/mojom/url_response_head.mojom.h"
#include "url/gurl.h"

namespace ark {

namespace {

constexpr size_t kMaximumConcurrentRequests = 4;
constexpr size_t kMaximumResponseBytes = 4 * 1024 * 1024;
constexpr char kAssistantInstruction[] =
    "You are a helpful assistant. If you include code, use a fenced Markdown "
    "block and put the language immediately after the opening backticks.";

std::optional<std::string> GeminiModelName(const std::string& model_id) {
  if (model_id == "cloud:gemini-2.5-flash") {
    return "gemini-2.5-flash";
  }
  if (model_id == "cloud:gemini-2.5-pro") {
    return "gemini-2.5-pro";
  }
  return std::nullopt;
}

base::DictValue MessageContent(const std::string& role,
                               const std::string& text) {
  base::ListValue parts;
  parts.Append(base::DictValue().Set("text", text));
  return base::DictValue().Set("role", role).Set("parts", std::move(parts));
}

bool AddImagePart(const std::string& image_data, base::ListValue* parts) {
  if (!base::StartsWith(image_data,
                        "data:", base::CompareCase::INSENSITIVE_ASCII)) {
    return false;
  }
  const size_t separator = image_data.find(',');
  const size_t mime_end = image_data.find(';');
  if (separator == std::string::npos || mime_end == std::string::npos ||
      mime_end >= separator ||
      image_data.substr(mime_end, separator - mime_end) != ";base64") {
    return false;
  }
  const std::string mime_type = image_data.substr(5, mime_end - 5);
  if (!base::StartsWith(mime_type, "image/",
                        base::CompareCase::INSENSITIVE_ASCII)) {
    return false;
  }
  parts->Append(base::DictValue().Set(
      "inlineData", base::DictValue()
                        .Set("mimeType", mime_type)
                        .Set("data", image_data.substr(separator + 1))));
  return true;
}

std::optional<std::string> ParseGeminiResponse(const std::string& body) {
  std::optional<base::DictValue> root =
      base::JSONReader::ReadDict(body, base::JSON_PARSE_RFC);
  if (!root) {
    return std::nullopt;
  }
  const base::ListValue* candidates = root->FindList("candidates");
  const base::DictValue* candidate = candidates && !candidates->empty()
                                         ? (*candidates)[0].GetIfDict()
                                         : nullptr;
  const base::DictValue* content =
      candidate ? candidate->FindDict("content") : nullptr;
  const base::ListValue* parts = content ? content->FindList("parts") : nullptr;
  if (!parts) {
    return std::nullopt;
  }
  std::string response;
  for (const base::Value& value : *parts) {
    const base::DictValue* part = value.GetIfDict();
    const std::string* text = part ? part->FindString("text") : nullptr;
    if (text) {
      response.append(*text);
    }
  }
  base::TrimWhitespaceASCII(response, base::TRIM_ALL, &response);
  return response.empty() ? std::nullopt
                          : std::optional<std::string>(std::move(response));
}

}  // namespace

ArkCloudInferenceService::ArkCloudInferenceService(Profile* profile)
    : profile_(profile) {}

ArkCloudInferenceService::~ArkCloudInferenceService() = default;

bool ArkCloudInferenceService::SendPrompt(
    const std::string& model_id,
    const std::string& prompt,
    const std::optional<std::string>& image_data,
    const std::vector<ChatMessage>& history,
    const std::optional<std::string>& credential,
    PromptCallback callback) {
  const std::optional<std::string> model_name = GeminiModelName(model_id);
  if (!model_name) {
    std::move(callback).Run("Unsupported cloud model.", false);
    return false;
  }
  if (!credential || credential->empty()) {
    std::move(callback).Run("Gemini API key is missing.", false);
    return false;
  }
  if (active_requests_.size() >= kMaximumConcurrentRequests) {
    std::move(callback).Run(
        "Cloud inference is busy. Wait for a response before trying again.",
        false);
    return false;
  }

  base::ListValue contents;
  for (const ChatMessage& message : history) {
    contents.Append(MessageContent(
        message.role == "assistant" ? "model" : "user", message.content));
  }
  base::ListValue prompt_parts;
  prompt_parts.Append(base::DictValue().Set("text", prompt));
  if (image_data && !AddImagePart(*image_data, &prompt_parts)) {
    std::move(callback).Run("The attached image data is invalid.", false);
    return false;
  }
  contents.Append(base::DictValue()
                      .Set("role", "user")
                      .Set("parts", std::move(prompt_parts)));

  base::ListValue system_parts;
  system_parts.Append(base::DictValue().Set("text", kAssistantInstruction));
  base::DictValue request_body;
  request_body.Set("systemInstruction",
                   base::DictValue().Set("parts", std::move(system_parts)));
  request_body.Set("contents", std::move(contents));
  std::string body;
  if (!base::JSONWriter::Write(request_body, &body)) {
    std::move(callback).Run("Ark could not prepare the Gemini request.", false);
    return false;
  }

  net::NetworkTrafficAnnotationTag traffic_annotation =
      net::DefineNetworkTrafficAnnotation("ark_gemini_inference", R"(
        semantics {
          sender: "Ark AI Gemini Provider"
          description:
            "Sends chat content to Gemini when the user selects a Gemini "
            "cloud model."
          trigger: "The user sends a message with a Gemini model selected."
          data: "Chat messages, optional image data, and the user's Gemini API key."
          destination: GOOGLE_OWNED_SERVICE
          internal {
            contacts { email: "ark@arkbrowser.internal" }
          }
          user_data { type: USER_CONTENT }
          last_reviewed: "2026-09-17"
        }
        policy {
          cookies_allowed: NO
          setting:
            "The request is sent only after the user configures a Gemini API "
            "key and selects a Gemini model."
          policy_exception_justification: "Not implemented."
        })");

  auto request = std::make_unique<network::ResourceRequest>();
  request->url = GURL(
      base::StrCat({"https://generativelanguage.googleapis.com/v1beta/models/",
                    *model_name, ":generateContent"}));
  request->method = "POST";
  request->load_flags = net::LOAD_DO_NOT_SAVE_COOKIES;
  request->credentials_mode = network::mojom::CredentialsMode::kOmit;
  request->headers.SetHeader("X-Goog-Api-Key", *credential);

  std::unique_ptr<network::SimpleURLLoader> loader =
      network::SimpleURLLoader::Create(std::move(request), traffic_annotation);
  loader->AttachStringForUpload(body, "application/json");
  const uint64_t request_id = next_request_id_++;
  network::SimpleURLLoader* loader_ptr = loader.get();
  active_requests_.emplace(request_id,
                           ActiveRequest{.loader = std::move(loader),
                                         .callback = std::move(callback)});
  loader_ptr->DownloadToString(
      profile_->GetURLLoaderFactory().get(),
      base::BindOnce(&ArkCloudInferenceService::OnGeminiResponse,
                     weak_ptr_factory_.GetWeakPtr(), request_id),
      kMaximumResponseBytes);
  return true;
}

void ArkCloudInferenceService::OnGeminiResponse(
    uint64_t request_id,
    std::optional<std::string> response_body) {
  auto active = active_requests_.find(request_id);
  if (active == active_requests_.end()) {
    return;
  }
  PromptCallback callback = std::move(active->second.callback);
  const int response_code =
      active->second.loader->ResponseInfo() &&
              active->second.loader->ResponseInfo()->headers
          ? active->second.loader->ResponseInfo()->headers->response_code()
          : 0;
  active_requests_.erase(active);

  if (!response_body || response_code < 200 || response_code >= 300) {
    std::move(callback).Run(
        response_code
            ? base::StrCat({"Gemini request failed (",
                            base::NumberToString(response_code), ")."})
            : "Gemini request failed.",
        false);
    return;
  }
  std::optional<std::string> response = ParseGeminiResponse(*response_body);
  if (!response) {
    std::move(callback).Run("Gemini returned an empty response.", false);
    return;
  }
  std::move(callback).Run(*response, true);
}

}  // namespace ark
