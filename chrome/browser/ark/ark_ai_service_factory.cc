// Copyright 2026 Arkapravo Ghosh
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/ark/ark_ai_service_factory.h"

#include "base/no_destructor.h"
#include "chrome/browser/ark/ark_ai_service.h"
#include "chrome/browser/profiles/profile.h"

namespace ark {

ArkAIService* ArkAIServiceFactory::GetForProfile(Profile* profile) {
  return static_cast<ArkAIService*>(
      GetInstance()->GetServiceForBrowserContext(profile, true));
}

ArkAIServiceFactory* ArkAIServiceFactory::GetInstance() {
  static base::NoDestructor<ArkAIServiceFactory> instance;
  return instance.get();
}

ArkAIServiceFactory::ArkAIServiceFactory()
    : ProfileKeyedServiceFactory(
          "ArkAIService",
          ProfileSelections::BuildForRegularAndIncognito()) {}

ArkAIServiceFactory::~ArkAIServiceFactory() = default;

std::unique_ptr<KeyedService>
ArkAIServiceFactory::BuildServiceInstanceForBrowserContext(
    content::BrowserContext* context) const {
  Profile* profile = Profile::FromBrowserContext(context);
  return std::make_unique<ArkAIService>(profile, profile->IsOffTheRecord());
}

}  // namespace ark
