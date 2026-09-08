// Copyright 2026 Arkapravo Ghosh
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_ARK_ARK_AI_SERVICE_FACTORY_H_
#define CHROME_BROWSER_ARK_ARK_AI_SERVICE_FACTORY_H_

#include "chrome/browser/profiles/profile_keyed_service_factory.h"

class Profile;

namespace base {
template <typename T>
class NoDestructor;
}

namespace ark {

class ArkAIService;

class ArkAIServiceFactory : public ProfileKeyedServiceFactory {
 public:
  static ArkAIService* GetForProfile(Profile* profile);
  static ArkAIServiceFactory* GetInstance();

 private:
  friend base::NoDestructor<ArkAIServiceFactory>;
  ArkAIServiceFactory();
  ~ArkAIServiceFactory() override;

  std::unique_ptr<KeyedService> BuildServiceInstanceForBrowserContext(
      content::BrowserContext* context) const override;
};

}  // namespace ark

#endif  // CHROME_BROWSER_ARK_ARK_AI_SERVICE_FACTORY_H_
