/*
 * Copyright (C) 2013 Google Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *
 *     * Redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above
 * copyright notice, this list of conditions and the following disclaimer
 * in the documentation and/or other materials provided with the
 * distribution.
 *     * Neither the name of Google Inc. nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "third_party/blink/renderer/modules/exported/web_embedded_worker_impl.h"

#include <memory>
#include <utility>
#include "base/feature_list.h"
#include "services/network/public/cpp/shared_url_loader_factory.h"
#include "services/network/public/mojom/referrer_policy.mojom-shared.h"
#include "third_party/blink/public/common/features.h"
#include "third_party/blink/public/mojom/service_worker/service_worker_installed_scripts_manager.mojom-blink.h"
#include "third_party/blink/public/platform/modules/service_worker/web_service_worker_network_provider.h"
#include "third_party/blink/public/platform/modules/service_worker/web_service_worker_provider.h"
#include "third_party/blink/public/platform/platform.h"
#include "third_party/blink/public/platform/task_type.h"
#include "third_party/blink/public/platform/web_url_request.h"
#include "third_party/blink/public/platform/web_worker_fetch_context.h"
#include "third_party/blink/public/web/modules/service_worker/web_service_worker_context_client.h"
#include "third_party/blink/public/web/web_console_message.h"
#include "third_party/blink/public/web/web_settings.h"
#include "third_party/blink/renderer/bindings/core/v8/source_location.h"
#include "third_party/blink/renderer/core/execution_context/security_context.h"
#include "third_party/blink/renderer/core/frame/csp/content_security_policy.h"
#include "third_party/blink/renderer/core/inspector/console_message.h"
#include "third_party/blink/renderer/core/inspector/worker_devtools_params.h"
#include "third_party/blink/renderer/core/loader/frame_load_request.h"
#include "third_party/blink/renderer/core/loader/worker_fetch_context.h"
#include "third_party/blink/renderer/core/probe/core_probes.h"
#include "third_party/blink/renderer/core/script/script.h"
#include "third_party/blink/renderer/core/workers/parent_execution_context_task_runners.h"
#include "third_party/blink/renderer/core/workers/worker_backing_thread_startup_data.h"
#include "third_party/blink/renderer/core/workers/worker_classic_script_loader.h"
#include "third_party/blink/renderer/core/workers/worker_content_settings_client.h"
#include "third_party/blink/renderer/core/workers/worker_global_scope.h"
#include "third_party/blink/renderer/modules/indexeddb/indexed_db_client.h"
#include "third_party/blink/renderer/modules/service_worker/service_worker_global_scope_client.h"
#include "third_party/blink/renderer/modules/service_worker/service_worker_global_scope_proxy.h"
#include "third_party/blink/renderer/modules/service_worker/service_worker_installed_scripts_manager.h"
#include "third_party/blink/renderer/modules/service_worker/service_worker_thread.h"
#include "third_party/blink/renderer/platform/heap/handle.h"
#include "third_party/blink/renderer/platform/instrumentation/tracing/trace_event.h"
#include "third_party/blink/renderer/platform/loader/fetch/fetch_client_settings_object_snapshot.h"
#include "third_party/blink/renderer/platform/loader/fetch/resource_fetcher.h"
#include "third_party/blink/renderer/platform/loader/fetch/resource_fetcher_properties.h"
#include "third_party/blink/renderer/platform/network/network_utils.h"
#include "third_party/blink/renderer/platform/shared_buffer.h"
#include "third_party/blink/renderer/platform/weborigin/kurl.h"
#include "third_party/blink/renderer/platform/weborigin/security_origin.h"
#include "third_party/blink/renderer/platform/weborigin/security_policy.h"
#include "third_party/blink/renderer/platform/wtf/functional.h"
#include "third_party/blink/renderer/platform/wtf/vector.h"

namespace blink {

// static
std::unique_ptr<WebEmbeddedWorker> WebEmbeddedWorker::Create(
    std::unique_ptr<WebServiceWorkerContextClient> client,
    std::unique_ptr<WebServiceWorkerInstalledScriptsManagerParams>
        installed_scripts_manager_params,
    mojo::ScopedMessagePipeHandle content_settings_handle,
    mojo::ScopedMessagePipeHandle cache_storage,
    mojo::ScopedMessagePipeHandle interface_provider) {
  return std::make_unique<WebEmbeddedWorkerImpl>(
      std::move(client), std::move(installed_scripts_manager_params),
      std::make_unique<ServiceWorkerContentSettingsProxy>(
          // Chrome doesn't use interface versioning.
          // TODO(falken): Is that comment about versioning correct?
          mojom::blink::WorkerContentSettingsProxyPtrInfo(
              std::move(content_settings_handle), 0u)),
      mojom::blink::CacheStoragePtrInfo(std::move(cache_storage),
                                        mojom::blink::CacheStorage::Version_),
      service_manager::mojom::blink::InterfaceProviderPtrInfo(
          std::move(interface_provider),
          service_manager::mojom::blink::InterfaceProvider::Version_));
}

// static
std::unique_ptr<WebEmbeddedWorkerImpl> WebEmbeddedWorkerImpl::CreateForTesting(
    std::unique_ptr<WebServiceWorkerContextClient> client,
    std::unique_ptr<ServiceWorkerInstalledScriptsManager>
        installed_scripts_manager) {
  auto worker_impl = std::make_unique<WebEmbeddedWorkerImpl>(
      std::move(client), nullptr /* installed_scripts_manager_params */,
      std::make_unique<ServiceWorkerContentSettingsProxy>(
          nullptr /* host_info */),
      nullptr /* cache_storage_info */, nullptr /* interface_provider_info */);
  worker_impl->installed_scripts_manager_ =
      std::move(installed_scripts_manager);
  return worker_impl;
}

WebEmbeddedWorkerImpl::WebEmbeddedWorkerImpl(
    std::unique_ptr<WebServiceWorkerContextClient> client,
    std::unique_ptr<WebServiceWorkerInstalledScriptsManagerParams>
        installed_scripts_manager_params,
    std::unique_ptr<ServiceWorkerContentSettingsProxy> content_settings_client,
    mojom::blink::CacheStoragePtrInfo cache_storage_info,
    service_manager::mojom::blink::InterfaceProviderPtrInfo
        interface_provider_info)
    : worker_context_client_(std::move(client)),
      content_settings_client_(std::move(content_settings_client)),
      pause_after_download_state_(kDontPauseAfterDownload),
      waiting_for_debugger_state_(kNotWaitingForDebugger),
      cache_storage_info_(std::move(cache_storage_info)),
      interface_provider_info_(std::move(interface_provider_info)) {
  if (installed_scripts_manager_params) {
    DCHECK(installed_scripts_manager_params->manager_request.is_valid());
    DCHECK(installed_scripts_manager_params->manager_host_ptr.is_valid());
    Vector<KURL> installed_scripts_urls;
    installed_scripts_urls.AppendRange(
        installed_scripts_manager_params->installed_scripts_urls.begin(),
        installed_scripts_manager_params->installed_scripts_urls.end());
    installed_scripts_manager_ = std::make_unique<
        ServiceWorkerInstalledScriptsManager>(
        installed_scripts_urls,
        mojom::blink::ServiceWorkerInstalledScriptsManagerRequest(
            std::move(installed_scripts_manager_params->manager_request)),
        mojom::blink::ServiceWorkerInstalledScriptsManagerHostPtrInfo(
            std::move(installed_scripts_manager_params->manager_host_ptr),
            mojom::blink::ServiceWorkerInstalledScriptsManagerHost::Version_),
        Platform::Current()->GetIOTaskRunner());
  }
}

WebEmbeddedWorkerImpl::~WebEmbeddedWorkerImpl() {
  // TerminateWorkerContext() must be called before the destructor.
  DCHECK(asked_to_terminate_);
}

void WebEmbeddedWorkerImpl::StartWorkerContext(
    const WebEmbeddedWorkerStartData& data) {
  DCHECK(!asked_to_terminate_);
  DCHECK(!main_script_loader_);
  DCHECK_EQ(pause_after_download_state_, kDontPauseAfterDownload);
  worker_start_data_ = data;

  // TODO(mkwst): This really needs to be piped through from the requesting
  // document, like we're doing for SharedWorkers. That turns out to be
  // incredibly convoluted, and since ServiceWorkers are locked to the same
  // origin as the page which requested them, the only time it would come
  // into play is a DNS poisoning attack after the page load. It's something
  // we should fix, but we're taking this shortcut for the prototype.
  //
  // https://crbug.com/590714
  KURL script_url = worker_start_data_.script_url;
  worker_start_data_.address_space = mojom::IPAddressSpace::kPublic;
  if (network_utils::IsReservedIPAddress(script_url.Host()))
    worker_start_data_.address_space = mojom::IPAddressSpace::kPrivate;
  if (SecurityOrigin::Create(script_url)->IsLocalhost())
    worker_start_data_.address_space = mojom::IPAddressSpace::kLocal;

  if (data.pause_after_download_mode ==
      WebEmbeddedWorkerStartData::kPauseAfterDownload)
    pause_after_download_state_ = kDoPauseAfterDownload;

  devtools_worker_token_ = data.devtools_worker_token;
  // S13nServiceWorker: |loader_factory| is null since all loads for new scripts
  // go through (internal WebServiceWorkerNetworkProvider class in
  // service_worker_context_client.cc)::script_loader_factory() rather than the
  // shadow page's loader. This is different to shared workers, which use
  // script_loader_factory() for the main script only, and the shadow page
  // loader for importScripts().
  //
  // Non-S13nServiceWorker: |loader_factory| is null since the main script load
  // goes through the shadow page loader which uses the default loader that goes
  // to ResourceDispatcherHost.
  shadow_page_ = std::make_unique<WorkerShadowPage>(
      this, nullptr /* loader_factory */,
      std::move(worker_start_data_.privacy_preferences));
  WebSettings* settings = shadow_page_->GetSettings();

  // Currently we block all mixed-content requests from a ServiceWorker.
  settings->SetStrictMixedContentChecking(true);
  settings->SetAllowRunningOfInsecureContent(false);

  // If we were asked to wait for debugger then now is a good time to do that.
  worker_context_client_->WorkerReadyForInspectionOnMainThread();
  if (worker_start_data_.wait_for_debugger_mode ==
      WebEmbeddedWorkerStartData::kWaitForDebugger) {
    waiting_for_debugger_state_ = kWaitingForDebugger;
    return;
  }

  shadow_page_->Initialize(worker_start_data_.script_url);
}

void WebEmbeddedWorkerImpl::TerminateWorkerContext() {
  if (asked_to_terminate_)
    return;
  asked_to_terminate_ = true;
  if (!shadow_page_->WasInitialized()) {
    // This deletes 'this'.
    worker_context_client_->WorkerContextFailedToStartOnMainThread();
    return;
  }
  if (main_script_loader_) {
    main_script_loader_->Cancel();
    main_script_loader_ = nullptr;
    // This deletes 'this'.
    worker_context_client_->WorkerContextFailedToStartOnMainThread();
    return;
  }
  if (!worker_thread_) {
    // The worker thread has not been created yet if the worker is asked to
    // terminate during waiting for debugger or paused after download.
    DCHECK(worker_start_data_.wait_for_debugger_mode ==
               WebEmbeddedWorkerStartData::kWaitForDebugger ||
           pause_after_download_state_ == kIsPausedAfterDownload);
    // This deletes 'this'.
    worker_context_client_->WorkerContextFailedToStartOnMainThread();
    return;
  }
  worker_thread_->Terminate();
  DevToolsAgent::WorkerThreadTerminated(shadow_page_->GetDocument(),
                                        worker_thread_.get());
}

void WebEmbeddedWorkerImpl::ResumeAfterDownload() {
  DCHECK(!asked_to_terminate_);

  if (base::FeatureList::IsEnabled(
          features::kOffMainThreadServiceWorkerScriptFetch)) {
    // Do nothing. ServiceWorkerGlobalScope suspends worker execution
    // until ReadyToEvaluateScript() is called.
    // TODO(bashi): Don't call ResumeAfterDownload() in the browser side
    // as we do nothing here.
    return;
  }

  DCHECK_EQ(pause_after_download_state_, kIsPausedAfterDownload);
  pause_after_download_state_ = kDontPauseAfterDownload;
  StartWorkerThread();
}

void WebEmbeddedWorkerImpl::AddMessageToConsole(
    const WebConsoleMessage& message) {
  MessageLevel web_core_message_level;
  switch (message.level) {
    case mojom::ConsoleMessageLevel::kVerbose:
      web_core_message_level = kVerboseMessageLevel;
      break;
    case mojom::ConsoleMessageLevel::kInfo:
      web_core_message_level = kInfoMessageLevel;
      break;
    case mojom::ConsoleMessageLevel::kWarning:
      web_core_message_level = kWarningMessageLevel;
      break;
    case mojom::ConsoleMessageLevel::kError:
      web_core_message_level = kErrorMessageLevel;
      break;
    default:
      NOTREACHED();
      return;
  }

  shadow_page_->GetDocument()->AddConsoleMessage(ConsoleMessage::Create(
      kOtherMessageSource, web_core_message_level, message.text,
      SourceLocation::Create(message.url, message.line_number,
                             message.column_number, nullptr)));
}

void WebEmbeddedWorkerImpl::BindDevToolsAgent(
    mojo::ScopedInterfaceEndpointHandle devtools_agent_host_ptr_info,
    mojo::ScopedInterfaceEndpointHandle devtools_agent_request) {
  shadow_page_->DevToolsAgent()->BindRequest(
      mojom::blink::DevToolsAgentHostAssociatedPtrInfo(
          std::move(devtools_agent_host_ptr_info),
          mojom::blink::DevToolsAgentHost::Version_),
      mojom::blink::DevToolsAgentAssociatedRequest(
          std::move(devtools_agent_request)));
}

std::unique_ptr<WebApplicationCacheHost>
WebEmbeddedWorkerImpl::CreateApplicationCacheHost(
    WebApplicationCacheHostClient*) {
  return nullptr;
}

void WebEmbeddedWorkerImpl::OnShadowPageInitialized() {
  DCHECK(!asked_to_terminate_);

  DCHECK(worker_context_client_);
  shadow_page_->DocumentLoader()->SetServiceWorkerNetworkProvider(
      worker_context_client_->CreateServiceWorkerNetworkProviderOnMainThread());

  // If this is an installed service worker, we can start the worker thread
  // now. The script will be streamed in by the installed scripts manager in
  // parallel. For non-installed scripts, the script must be loaded from network
  // before the worker thread can be started.
  if (installed_scripts_manager_ &&
      installed_scripts_manager_->IsScriptInstalled(
          worker_start_data_.script_url)) {
    DCHECK_EQ(pause_after_download_state_, kDontPauseAfterDownload);
    StartWorkerThread();
    return;
  }

  // If this is a module service worker, start the worker thread now. The worker
  // thread will fetch the script.
  if (worker_start_data_.script_type == mojom::ScriptType::kModule) {
    StartWorkerThread();
    return;
  }

  // Bypass main script loading on the main thread.
  if (base::FeatureList::IsEnabled(
          features::kOffMainThreadServiceWorkerScriptFetch)) {
    StartWorkerThread();
    return;
  }

  // Note: We only get here if this is a new (i.e., not installed) service
  // worker.
  DCHECK(!main_script_loader_);
  main_script_loader_ = MakeGarbageCollected<WorkerClassicScriptLoader>();
  main_script_loader_->LoadTopLevelScriptAsynchronously(
      *shadow_page_->GetDocument(), shadow_page_->GetDocument()->Fetcher(),
      worker_start_data_.script_url, mojom::RequestContextType::SERVICE_WORKER,
      network::mojom::FetchRequestMode::kSameOrigin,
      network::mojom::FetchCredentialsMode::kSameOrigin,
      worker_start_data_.address_space, base::OnceClosure(),
      Bind(&WebEmbeddedWorkerImpl::OnScriptLoaderFinished,
           WTF::Unretained(this)));
  // Do nothing here since OnScriptLoaderFinished() might have been already
  // invoked and |this| might have been deleted at this point.
}

void WebEmbeddedWorkerImpl::ResumeStartup() {
  bool was_waiting = (waiting_for_debugger_state_ == kWaitingForDebugger);
  waiting_for_debugger_state_ = kNotWaitingForDebugger;
  if (was_waiting)
    shadow_page_->Initialize(worker_start_data_.script_url);
}

const base::UnguessableToken& WebEmbeddedWorkerImpl::GetDevToolsWorkerToken() {
  return devtools_worker_token_;
}

void WebEmbeddedWorkerImpl::OnScriptLoaderFinished() {
  DCHECK(main_script_loader_);
  if (asked_to_terminate_)
    return;

  if (main_script_loader_->Failed()) {
    TerminateWorkerContext();
    return;
  }
  worker_context_client_->WorkerScriptLoadedOnMainThread();

  if (pause_after_download_state_ == kDoPauseAfterDownload) {
    pause_after_download_state_ = kIsPausedAfterDownload;
    return;
  }
  StartWorkerThread();
}

void WebEmbeddedWorkerImpl::StartWorkerThread() {
  // The mechanism of pause-after-download in this class isn't used when
  // off-the-main-thread script fetch is enabled. The browser process delays
  // initializing the global scope to pause the worker when needed.
  DCHECK(base::FeatureList::IsEnabled(
             features::kOffMainThreadServiceWorkerScriptFetch) ||
         pause_after_download_state_ == kDontPauseAfterDownload);
  DCHECK(!asked_to_terminate_);

  Document* document = shadow_page_->GetDocument();

  // For now we don't use global scope name for service workers.
  const String global_scope_name = g_empty_string;

  // FIXME: this document's origin is pristine and without any extra privileges.
  // (crbug.com/254993)
  const SecurityOrigin* starter_origin = document->GetSecurityOrigin();
  bool starter_secure_context = document->IsSecureContext();
  const HttpsState starter_https_state = document->GetHttpsState();

  WorkerClients* worker_clients = WorkerClients::Create();
  ProvideIndexedDBClientToWorker(worker_clients,
                                 IndexedDBClient::Create(*worker_clients));

  ProvideContentSettingsClientToWorker(worker_clients,
                                       std::move(content_settings_client_));
  ProvideServiceWorkerGlobalScopeClientToWorker(
      worker_clients, MakeGarbageCollected<ServiceWorkerGlobalScopeClient>(
                          *worker_context_client_));

  // |web_worker_fetch_context| is null in some unit tests.
  scoped_refptr<WebWorkerFetchContext> web_worker_fetch_context =
      worker_context_client_->CreateServiceWorkerFetchContextOnMainThread(
          shadow_page_->DocumentLoader()->GetServiceWorkerNetworkProvider());

  std::unique_ptr<WorkerSettings> worker_settings =
      std::make_unique<WorkerSettings>(document->GetSettings());

  std::unique_ptr<GlobalScopeCreationParams> global_scope_creation_params;
  String source_code;
  std::unique_ptr<Vector<uint8_t>> cached_meta_data;

  const OffMainThreadWorkerScriptFetchOption off_main_thread_fetch_option =
      (base::FeatureList::IsEnabled(
           features::kOffMainThreadServiceWorkerScriptFetch) ||
       worker_start_data_.script_type == mojom::ScriptType::kModule)
          ? OffMainThreadWorkerScriptFetchOption::kEnabled
          : OffMainThreadWorkerScriptFetchOption::kDisabled;

  // |main_script_loader_| isn't created if the InstalledScriptsManager had the
  // script.
  if (main_script_loader_) {
    ContentSecurityPolicy* content_security_policy =
        main_script_loader_->GetContentSecurityPolicy();
    network::mojom::ReferrerPolicy referrer_policy =
        network::mojom::ReferrerPolicy::kDefault;
    if (!main_script_loader_->GetReferrerPolicy().IsNull()) {
      SecurityPolicy::ReferrerPolicyFromHeaderValue(
          main_script_loader_->GetReferrerPolicy(),
          kDoNotSupportReferrerPolicyLegacyKeywords, &referrer_policy);
    }
    global_scope_creation_params = std::make_unique<GlobalScopeCreationParams>(
        worker_start_data_.script_url, worker_start_data_.script_type,
        off_main_thread_fetch_option, global_scope_name,
        worker_start_data_.user_agent, std::move(web_worker_fetch_context),
        content_security_policy ? content_security_policy->Headers()
                                : Vector<CSPHeaderAndType>(),
        referrer_policy, starter_origin, starter_secure_context,
        starter_https_state, worker_clients,
        main_script_loader_->ResponseAddressSpace(),
        main_script_loader_->OriginTrialTokens(), devtools_worker_token_,
        std::move(worker_settings),
        static_cast<V8CacheOptions>(worker_start_data_.v8_cache_options),
        nullptr /* worklet_module_respones_map */,
        std::move(interface_provider_info_));
    source_code = main_script_loader_->SourceText();
    cached_meta_data = main_script_loader_->ReleaseCachedMetadata();
    main_script_loader_ = nullptr;
  } else {
    // We don't have to set ContentSecurityPolicy and ReferrerPolicy. They're
    // served by the installed scripts manager on the worker thread.
    global_scope_creation_params = std::make_unique<GlobalScopeCreationParams>(
        worker_start_data_.script_url, worker_start_data_.script_type,
        off_main_thread_fetch_option, global_scope_name,
        worker_start_data_.user_agent, std::move(web_worker_fetch_context),
        Vector<CSPHeaderAndType>(), network::mojom::ReferrerPolicy::kDefault,
        starter_origin, starter_secure_context, starter_https_state,
        worker_clients, worker_start_data_.address_space,
        nullptr /* OriginTrialTokens */, devtools_worker_token_,
        std::move(worker_settings),
        static_cast<V8CacheOptions>(worker_start_data_.v8_cache_options),
        nullptr /* worklet_module_respones_map */,
        std::move(interface_provider_info_));
  }

  // Generate the full code cache in the first execution of the script.
  global_scope_creation_params->v8_cache_options =
      kV8CacheOptionsFullCodeWithoutHeatCheck;

  bool is_script_installed = installed_scripts_manager_ &&
                             installed_scripts_manager_->IsScriptInstalled(
                                 worker_start_data_.script_url);

  worker_thread_ = std::make_unique<ServiceWorkerThread>(
      ServiceWorkerGlobalScopeProxy::Create(*this, *worker_context_client_),
      std::move(installed_scripts_manager_), std::move(cache_storage_info_));

  auto devtools_params = DevToolsAgent::WorkerThreadCreated(
      document, worker_thread_.get(), worker_start_data_.script_url,
      global_scope_name);

  // We have a dummy document here for loading but it doesn't really represent
  // the document/frame of associated document(s) for this worker. Here we
  // populate the task runners with default task runners of the main thread.
  worker_thread_->Start(std::move(global_scope_creation_params),
                        WorkerBackingThreadStartupData::CreateDefault(),
                        std::move(devtools_params),
                        ParentExecutionContextTaskRunners::Create());

  // If this is a new script (not installed), we are in the Update algorithm
  // here:
  // > Switching on job’s worker type, run these substeps with the following
  // > options:
  // https://w3c.github.io/ServiceWorker/#update-algorithm
  //
  // If this is an installed script, there is no corresponding algorithm steps
  // about fetching the script. The installed script is read from storage.
  if (worker_start_data_.script_type == mojom::ScriptType::kClassic) {
    // > "classic": Fetch a classic worker script given job’s serialized script
    // > url, job’s client, "serviceworker", and the to-be-created environment
    // > settings object for this service worker.

    // The script is ready to evaluate in following cases:
    // - Installed script: ServiceWorkerGlobalScope will get the script from
    //   InstalledScriptsManager in EvaluateClassicScript().
    // - OMT fetch is disabled: the script was already fetched and is in
    //   |source_code|.
    if (is_script_installed ||
        off_main_thread_fetch_option ==
            OffMainThreadWorkerScriptFetchOption::kDisabled) {
      worker_thread_->EvaluateClassicScript(
          worker_start_data_.script_url, source_code,
          std::move(cached_meta_data), v8_inspector::V8StackTraceId());
    } else {
      // When OMT fetch is enabled and this is a new script, fetch the script
      // now using ImportClassicScript().
      auto* outside_settings_object = CreateFetchClientSettingsObject();
      worker_thread_->ImportClassicScript(worker_start_data_.script_url,
                                          *outside_settings_object,
                                          v8_inspector::V8StackTraceId());
    }
  } else {
    // > "module": Fetch a module worker script graph given job’s serialized
    // > script url, job’s client, "serviceworker", "omit", and the
    // > to-be-created environment settings object for this service worker.

    auto* outside_settings_object = CreateFetchClientSettingsObject();
    network::mojom::FetchCredentialsMode credentials_mode =
        network::mojom::FetchCredentialsMode::kOmit;
    worker_thread_->ImportModuleScript(worker_start_data_.script_url,
                                       *outside_settings_object,
                                       credentials_mode);
  }
}

FetchClientSettingsObjectSnapshot*
WebEmbeddedWorkerImpl::CreateFetchClientSettingsObject() {
  DCHECK(shadow_page_->WasInitialized());
  // TODO(crbug.com/924043): Currently, we use the shadow page's Document as an
  // outside_settings_object as a workaround. For new worker case, this should
  // be the Document that called navigator.serviceWorker.register(). For
  // ServiceWorkerRegistration#update() case, it should be the Document that
  // called update(). For soft update case, it seems to be 'null' document.
  //
  // To get a correct settings, we need to make a way to pass the settings
  // object over mojo IPCs.
  Document* document = shadow_page_->GetDocument();
  return MakeGarbageCollected<FetchClientSettingsObjectSnapshot>(
      document->Fetcher()->GetProperties().GetFetchClientSettingsObject());
}

}  // namespace blink
