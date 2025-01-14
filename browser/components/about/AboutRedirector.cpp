/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// See also: docshell/base/nsAboutRedirector.cpp

#include "AboutRedirector.h"
#include "nsNetUtil.h"
#include "nsIChannel.h"
#include "nsIURI.h"
#include "nsIScriptSecurityManager.h"
#include "mozilla/ArrayUtils.h"
#include "nsDOMString.h"


namespace mozilla {
namespace browser {

NS_IMPL_ISUPPORTS(AboutRedirector, nsIAboutModule)

struct RedirEntry {
  const char* id;
  const char* url;
  uint32_t flags;
  const char* idbOriginPostfix;
};

/*
  Entries which do not have URI_SAFE_FOR_UNTRUSTED_CONTENT will run with chrome
  privileges. This is potentially dangerous. Please use
  URI_SAFE_FOR_UNTRUSTED_CONTENT in the third argument to each map item below
  unless your about: page really needs chrome privileges. Security review is
  required before adding new map entries without
  URI_SAFE_FOR_UNTRUSTED_CONTENT.
*/
static RedirEntry kRedirMap[] = {
  { "certerror", "chrome://browser/content/certerror/aboutCertError.xhtml",
    nsIAboutModule::URI_SAFE_FOR_UNTRUSTED_CONTENT |
    nsIAboutModule::URI_CAN_LOAD_IN_CHILD |
    nsIAboutModule::ALLOW_SCRIPT |
    nsIAboutModule::HIDE_FROM_ABOUTABOUT },
  { "tabcrashed", "chrome://browser/content/aboutTabCrashed.xhtml",
    nsIAboutModule::URI_SAFE_FOR_UNTRUSTED_CONTENT |
    nsIAboutModule::ALLOW_SCRIPT |
    nsIAboutModule::HIDE_FROM_ABOUTABOUT },
  { "feeds", "chrome://browser/content/feeds/subscribe.xhtml",
    nsIAboutModule::URI_SAFE_FOR_UNTRUSTED_CONTENT |
    nsIAboutModule::URI_MUST_LOAD_IN_CHILD |
    nsIAboutModule::ALLOW_SCRIPT |
    nsIAboutModule::HIDE_FROM_ABOUTABOUT },
  { "privatebrowsing", "chrome://browser/content/aboutPrivateBrowsing.xhtml",
    nsIAboutModule::URI_MUST_LOAD_IN_CHILD |
    nsIAboutModule::ALLOW_SCRIPT },
  { "rights",
#ifdef MOZ_OFFICIAL_BRANDING
    "chrome://global/content/aboutRights.xhtml",
#else
    "chrome://global/content/aboutRights-unbranded.xhtml",
#endif
    nsIAboutModule::URI_SAFE_FOR_UNTRUSTED_CONTENT |
    nsIAboutModule::MAKE_LINKABLE |
    nsIAboutModule::ALLOW_SCRIPT },
  { "palemoon", "chrome://global/content/palemoon.xhtml",
    nsIAboutModule::URI_SAFE_FOR_UNTRUSTED_CONTENT |
    nsIAboutModule::HIDE_FROM_ABOUTABOUT },
  { "logopage", "chrome://global/content/logopage.xhtml",
    nsIAboutModule::URI_SAFE_FOR_UNTRUSTED_CONTENT |
    nsIAboutModule::HIDE_FROM_ABOUTABOUT },
  { "robots", "chrome://browser/content/aboutRobots.xhtml",
    nsIAboutModule::URI_SAFE_FOR_UNTRUSTED_CONTENT |
    nsIAboutModule::ALLOW_SCRIPT },
  { "sessionrestore", "chrome://browser/content/aboutSessionRestore.xhtml",
    nsIAboutModule::ALLOW_SCRIPT },
  { "welcomeback", "chrome://browser/content/aboutWelcomeBack.xhtml",
    nsIAboutModule::ALLOW_SCRIPT },
#ifdef MOZ_SERVICES_SYNC
  { "sync-tabs", "chrome://browser/content/sync/aboutSyncTabs.xul",
    nsIAboutModule::ALLOW_SCRIPT },
#endif
  { "home", "chrome://browser/content/abouthome/aboutHome.xhtml",
    nsIAboutModule::URI_SAFE_FOR_UNTRUSTED_CONTENT |
    nsIAboutModule::MAKE_LINKABLE |
    nsIAboutModule::ALLOW_SCRIPT },
  { "newtab", "chrome://browser/content/newtab/newTab.xul",
    nsIAboutModule::ALLOW_SCRIPT },
  { "permissions", "chrome://browser/content/preferences/aboutPermissions.xul",
    nsIAboutModule::ALLOW_SCRIPT },
  { "downloads", "chrome://browser/content/downloads/contentAreaDownloadsView.xul",
    nsIAboutModule::ALLOW_SCRIPT },
};
static const int kRedirTotal = ArrayLength(kRedirMap);

static nsAutoCString
GetAboutModuleName(nsIURI *aURI)
{
  nsAutoCString path;
  aURI->GetPath(path);

  int32_t f = path.FindChar('#');
  if (f >= 0)
    path.SetLength(f);

  f = path.FindChar('?');
  if (f >= 0)
    path.SetLength(f);

  ToLowerCase(path);
  return path;
}

NS_IMETHODIMP
AboutRedirector::NewChannel(nsIURI* aURI,
                            nsILoadInfo* aLoadInfo,
                            nsIChannel** result)
{
  NS_ENSURE_ARG_POINTER(aURI);
  NS_ASSERTION(result, "must not be null");

  nsAutoCString path = GetAboutModuleName(aURI);

  nsresult rv;
  nsCOMPtr<nsIIOService> ioService = do_GetIOService(&rv);
  NS_ENSURE_SUCCESS(rv, rv);

  for (int i = 0; i < kRedirTotal; i++) {
    if (!strcmp(path.get(), kRedirMap[i].id)) {
      nsCOMPtr<nsIChannel> tempChannel;
      rv = ioService->NewChannel(nsDependentCString(kRedirMap[i].url),
                                 nullptr, nullptr, getter_AddRefs(tempChannel));
      NS_ENSURE_SUCCESS(rv, rv);

      tempChannel->SetOriginalURI(aURI);

      NS_ADDREF(*result = tempChannel);
      return rv;
    }
  }

  return NS_ERROR_ILLEGAL_VALUE;
}

NS_IMETHODIMP
AboutRedirector::GetURIFlags(nsIURI *aURI, uint32_t *result)
{
  NS_ENSURE_ARG_POINTER(aURI);

  nsAutoCString name = GetAboutModuleName(aURI);

  for (int i = 0; i < kRedirTotal; i++) {
    if (name.Equals(kRedirMap[i].id)) {
      *result = kRedirMap[i].flags;
      return NS_OK;
    }
  }

  return NS_ERROR_ILLEGAL_VALUE;
}

NS_IMETHODIMP
AboutRedirector::GetIndexedDBOriginPostfix(nsIURI *aURI, nsAString &result)
{
  NS_ENSURE_ARG_POINTER(aURI);

  nsAutoCString name = GetAboutModuleName(aURI);

  for (int i = 0; i < kRedirTotal; i++) {
    if (name.Equals(kRedirMap[i].id)) {
      const char* postfix = kRedirMap[i].idbOriginPostfix;
      if (!postfix) {
        break;
      }

      result.AssignASCII(postfix);
      return NS_OK;
    }
  }

  SetDOMStringToNull(result);
  return NS_ERROR_ILLEGAL_VALUE;
}


nsresult
AboutRedirector::Create(nsISupports *aOuter, REFNSIID aIID, void **result)
{
  AboutRedirector* about = new AboutRedirector();
  if (about == nullptr)
    return NS_ERROR_OUT_OF_MEMORY;
  NS_ADDREF(about);
  nsresult rv = about->QueryInterface(aIID, result);
  NS_RELEASE(about);
  return rv;
}

} // namespace browser
} // namespace mozilla
