// Copyright (c) 2016 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "extensions/browser/api/clipboard/clipboard_api.h"

#include <utility>

#include "base/lazy_instance.h"
#include "base/memory/ptr_util.h"
#include "base/values.h"
#include "extensions/browser/api/extensions_api_client.h"
#include "extensions/browser/event_router.h"
#include "ui/base/clipboard/clipboard_monitor.h"

namespace extensions {

namespace clipboard = api::clipboard;

static base::LazyInstance<BrowserContextKeyedAPIFactory<ClipboardAPI>>
    g_factory = LAZY_INSTANCE_INITIALIZER;

// static
BrowserContextKeyedAPIFactory<ClipboardAPI>*
ClipboardAPI::GetFactoryInstance() {
  return g_factory.Pointer();
}

ClipboardAPI::ClipboardAPI(content::BrowserContext* context)
    : browser_context_(context) {
  ui::ClipboardMonitor::GetInstance()->AddObserver(this);
}

ClipboardAPI::~ClipboardAPI() {
  ui::ClipboardMonitor::GetInstance()->RemoveObserver(this);
}

void ClipboardAPI::OnClipboardDataChanged() {
  EventRouter* router = EventRouter::Get(browser_context_);
  if (router &&
      router->HasEventListener(clipboard::OnClipboardDataChanged::kEventName)) {
    std::unique_ptr<Event> event(
        new Event(events::CLIPBOARD_ON_CLIPBOARD_DATA_CHANGED,
                  clipboard::OnClipboardDataChanged::kEventName,
                  base::MakeUnique<base::ListValue>()));
    router->BroadcastEvent(std::move(event));
  }
}

ClipboardSetImageDataFunction::~ClipboardSetImageDataFunction() {}

ExtensionFunction::ResponseAction ClipboardSetImageDataFunction::Run() {
  std::unique_ptr<clipboard::SetImageData::Params> params(
      clipboard::SetImageData::Params::Create(*args_));
  EXTENSION_FUNCTION_VALIDATE(params);
  ExtensionsAPIClient::Get()->SaveImageDataToClipboard(
      params->image_data, params->type,
      base::Bind(&ClipboardSetImageDataFunction::OnSaveImageDataSuccess, this),
      base::Bind(&ClipboardSetImageDataFunction::OnSaveImageDataError, this));
  return RespondLater();
}

void ClipboardSetImageDataFunction::OnSaveImageDataSuccess() {
  Respond(NoArguments());
}

void ClipboardSetImageDataFunction::OnSaveImageDataError(
    const std::string& error) {
  Respond(Error(error));
}

}  // namespace extensions
