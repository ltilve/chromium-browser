// Copyright 2016 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef EXTENSIONS_TEST_EXTENSION_TEST_NOTIFICATION_OBSERVER_H_
#define EXTENSIONS_TEST_EXTENSION_TEST_NOTIFICATION_OBSERVER_H_

#include <memory>
#include <string>

#include "base/callback.h"
#include "base/callback_list.h"
#include "base/macros.h"
#include "base/scoped_observer.h"
#include "content/public/browser/notification_observer.h"
#include "content/public/browser/notification_registrar.h"
#include "extensions/browser/process_manager_observer.h"

namespace content {
class BrowserContext;
class NotificationDetails;
class WindowedNotificationObserver;
}

namespace extensions {
class ProcessManager;

// Test helper class for observing extension-related events.
class ExtensionTestNotificationObserver : public content::NotificationObserver {
 public:
  explicit ExtensionTestNotificationObserver(content::BrowserContext* context);
  ~ExtensionTestNotificationObserver() override;

  // Wait for an extension install error to be raised. Returns true if an
  // error was raised.
  bool WaitForExtensionInstallError();

  // Waits until an extension is loaded.
  void WaitForExtensionLoad();

  // Waits for an extension load error. Returns true if the error really
  // happened.
  bool WaitForExtensionLoadError();

  // Wait for the specified extension to crash. Returns true if it really
  // crashed.
  bool WaitForExtensionCrash(const std::string& extension_id);

  // Wait for the crx installer to be done. Returns true if it really is done.
  bool WaitForCrxInstallerDone();

  // Watch for the given event type from the given source.
  // After calling this method, call Wait() to ensure that RunMessageLoop() is
  // called appropriately and cleanup is performed.
  void Watch(int type, const content::NotificationSource& source);

  // After registering one or more event types with Watch(), call
  // this method to run the message loop and perform cleanup.
  void Wait();

  const std::string& last_loaded_extension_id() {
    return last_loaded_extension_id_;
  }
  void set_last_loaded_extension_id(
      const std::string& last_loaded_extension_id) {
    last_loaded_extension_id_ = last_loaded_extension_id;
  }

  // content::NotificationObserver
  void Observe(int type,
               const content::NotificationSource& source,
               const content::NotificationDetails& details) override;

 protected:
  class NotificationSet : public content::NotificationObserver,
                          public extensions::ProcessManagerObserver {
   public:
    NotificationSet();
    ~NotificationSet() override;

    void Add(int type, const content::NotificationSource& source);
    void Add(int type);
    void AddExtensionFrameUnregistration(extensions::ProcessManager* manager);

    // Notified any time an Add()ed notification is received.
    // The details of the notification are dropped.
    base::CallbackList<void()>& callback_list() { return callback_list_; }

   private:
    // content::NotificationObserver:
    void Observe(int type,
                 const content::NotificationSource& source,
                 const content::NotificationDetails& details) override;

    // extensions::ProcessManagerObserver:
    void OnExtensionFrameUnregistered(
        const std::string& extension_id,
        content::RenderFrameHost* render_frame_host) override;

    content::NotificationRegistrar notification_registrar_;
    base::CallbackList<void()> callback_list_;
    ScopedObserver<extensions::ProcessManager,
                   extensions::ProcessManagerObserver>
        process_manager_observer_;
    DISALLOW_COPY_AND_ASSIGN(NotificationSet);
  };

  // Wait for |condition_| to be met. |notification_set| is the set of
  // notifications to wait for and to check |condition| when observing. This
  // can be NULL if we are instead waiting for a different observer method, like
  // OnPageActionsUpdated().
  void WaitForCondition(const base::Callback<bool(void)>& condition,
                        NotificationSet* notification_set);

  void WaitForNotification(int notification_type);

  // Quits the message loop if |condition_| is met.
  void MaybeQuit();

  content::BrowserContext* context_;

 private:
  content::NotificationRegistrar registrar_;
  std::unique_ptr<content::WindowedNotificationObserver> observer_;

  std::string last_loaded_extension_id_;
  int extension_installs_observed_;
  int extension_load_errors_observed_;
  int crx_installers_done_observed_;

  // The condition for which we are waiting. This should be checked in any
  // observing methods that could trigger it.
  base::Callback<bool(void)> condition_;

  // The closure to quit the currently-running message loop.
  base::Closure quit_closure_;

  DISALLOW_COPY_AND_ASSIGN(ExtensionTestNotificationObserver);
};

}  // namespace extensions

#endif  // EXTENSIONS_TEST_EXTENSION_TEST_NOTIFICATION_OBSERVER_H_
