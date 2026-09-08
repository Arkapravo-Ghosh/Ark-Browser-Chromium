// Copyright 2016 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/ui/webui/signin/signin_utils_desktop.h"

#include "base/command_line.h"
#include "base/strings/utf_string_conversions.h"
#include "chrome/browser/browser_process.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/profiles/profile_attributes_entry.h"
#include "chrome/browser/profiles/profile_attributes_storage.h"
#include "chrome/browser/profiles/profile_manager.h"
#include "chrome/browser/signin/chrome_signin_client.h"
#include "chrome/browser/signin/identity_manager_factory.h"
#include "chrome/browser/signin/signin_util.h"
#include "chrome/browser/ui/webui/signin/signin_ui_error.h"
#include "chrome/common/chrome_switches.h"
#include "components/prefs/pref_service.h"
#include "components/signin/public/base/signin_pref_names.h"
#include "components/signin/public/identity_manager/identity_manager.h"
#include "components/signin/public/identity_manager/identity_utils.h"
#include "components/sync/base/features.h"
#include "google_apis/gaia/gaia_auth_util.h"
#include "google_apis/gaia/gaia_id.h"

SigninUIError CanOfferSignin(Profile* profile,
                             const GaiaId& gaia_id,
                             std::string_view email,
                             bool allow_account_from_other_profile,
                             bool ignore_reauth_error) {
  // Disabled in Ark Browser: browser sign-in is not offered.
  return SigninUIError::SigninDisallowed(email);
}

bool IsCrossAccountError(Profile* profile, const GaiaId& gaia_id) {
  DCHECK(!gaia_id.empty());
  const GaiaId last_gaia_id(
      profile->GetPrefs()->GetString(prefs::kGoogleServicesLastSyncingGaiaId));
  return !last_gaia_id.empty() && gaia_id != last_gaia_id;
}
