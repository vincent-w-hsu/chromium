// Copyright 2018 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "base/bind.h"
#include "base/files/file_path.h"
#include "base/path_service.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/post_task.h"
#include "base/test/metrics/histogram_tester.h"
#include "base/test/scoped_feature_list.h"
#include "base/threading/thread_restrictions.h"
#include "base/time/time.h"
#include "content/browser/frame_host/navigation_handle_impl.h"
#include "content/browser/loader/prefetch_url_loader_service.h"
#include "content/browser/storage_partition_impl.h"
#include "content/browser/web_package/signed_exchange_handler.h"
#include "content/browser/web_package/signed_exchange_utils.h"
#include "content/public/browser/browser_context.h"
#include "content/public/browser/browser_task_traits.h"
#include "content/public/browser/browser_thread.h"
#include "content/public/browser/download_manager.h"
#include "content/public/browser/navigation_controller.h"
#include "content/public/browser/navigation_entry.h"
#include "content/public/browser/navigation_handle.h"
#include "content/public/browser/ssl_status.h"
#include "content/public/browser/web_contents.h"
#include "content/public/browser/web_contents_observer.h"
#include "content/public/common/content_features.h"
#include "content/public/common/content_paths.h"
#include "content/public/common/page_type.h"
#include "content/public/test/browser_test_utils.h"
#include "content/public/test/content_browser_test_utils.h"
#include "content/public/test/content_cert_verifier_browser_test.h"
#include "content/public/test/signed_exchange_browser_test_helper.h"
#include "content/public/test/test_navigation_throttle.h"
#include "content/public/test/url_loader_interceptor.h"
#include "content/shell/browser/shell.h"
#include "content/shell/browser/shell_download_manager_delegate.h"
#include "net/cert/cert_verify_result.h"
#include "net/cert/mock_cert_verifier.h"
#include "net/dns/mock_host_resolver.h"
#include "net/test/cert_test_util.h"
#include "net/test/embedded_test_server/embedded_test_server.h"
#include "net/test/embedded_test_server/http_request.h"
#include "net/test/embedded_test_server/http_response.h"
#include "net/test/test_data_directory.h"
#include "net/test/url_request/url_request_mock_http_job.h"
#include "net/url_request/url_request_filter.h"
#include "net/url_request/url_request_interceptor.h"
#include "services/network/loader_util.h"
#include "services/network/public/cpp/features.h"
#include "testing/gmock/include/gmock/gmock-matchers.h"
#include "third_party/blink/public/common/features.h"

namespace content {

namespace {

constexpr char kExpectedSXGEnabledAcceptHeaderForPrefetch[] =
    "application/signed-exchange;v=b3;q=0.9,*/*;q=0.8";

constexpr char kLoadResultHistogram[] = "SignedExchange.LoadResult2";
constexpr char kPrefetchResultHistogram[] =
    "SignedExchange.Prefetch.LoadResult2";

class RedirectObserver : public WebContentsObserver {
 public:
  explicit RedirectObserver(WebContents* web_contents)
      : WebContentsObserver(web_contents) {}
  ~RedirectObserver() override = default;

  void DidRedirectNavigation(NavigationHandle* handle) override {
    const net::HttpResponseHeaders* response = handle->GetResponseHeaders();
    if (response)
      response_code_ = response->response_code();
  }

  const base::Optional<int>& response_code() const { return response_code_; }

 private:
  base::Optional<int> response_code_;

  DISALLOW_COPY_AND_ASSIGN(RedirectObserver);
};

class AssertNavigationHandleFlagObserver : public WebContentsObserver {
 public:
  explicit AssertNavigationHandleFlagObserver(WebContents* web_contents)
      : WebContentsObserver(web_contents) {}
  ~AssertNavigationHandleFlagObserver() override = default;

  void DidFinishNavigation(NavigationHandle* handle) override {
    EXPECT_TRUE(static_cast<NavigationHandleImpl*>(handle)->IsSignedExchangeInnerResponse());
  }

 private:
  DISALLOW_COPY_AND_ASSIGN(AssertNavigationHandleFlagObserver);
};

class MockContentBrowserClient final : public ContentBrowserClient {
 public:
  std::string GetAcceptLangs(BrowserContext* context) override {
    return accept_langs_;
  }
  void SetAcceptLangs(const std::string langs) { accept_langs_ = langs; }

 private:
  std::string accept_langs_ = "en";
};

}  // namespace

class SignedExchangeRequestHandlerBrowserTestBase
    : public CertVerifierBrowserTest {
 public:
  SignedExchangeRequestHandlerBrowserTestBase() {
    // This installs "root_ca_cert.pem" from which our test certificates are
    // created. (Needed for the tests that use real certificate, i.e.
    // RealCertVerifier)
    net::EmbeddedTestServer::RegisterTestCerts();
  }

  void SetUp() override {
    sxg_test_helper_.SetUp();
    feature_list_.InitWithFeatures({features::kSignedHTTPExchange}, {});
    CertVerifierBrowserTest::SetUp();
  }

  void SetUpOnMainThread() override {
    original_client_ = SetBrowserClientForTesting(&client_);
    CertVerifierBrowserTest::SetUpOnMainThread();
  }

  void TearDownOnMainThread() override {
    sxg_test_helper_.TearDownOnMainThread();
    if (original_client_)
      SetBrowserClientForTesting(original_client_);
  }

 protected:
  void InstallUrlInterceptor(const GURL& url, const std::string& data_path) {
    sxg_test_helper_.InstallUrlInterceptor(url, data_path);
  }

  void InstallMockCert() {
    sxg_test_helper_.InstallMockCert(mock_cert_verifier());
  }

  void InstallMockCertChainInterceptor() {
    sxg_test_helper_.InstallMockCertChainInterceptor();
  }

  void TriggerPrefetch(const GURL& url, bool expect_success) {
    const GURL prefetch_html_url = embedded_test_server()->GetURL(
        std::string("/sxg/prefetch.html#") + url.spec());
    base::string16 expected_title =
        base::ASCIIToUTF16(expect_success ? "OK" : "FAIL");
    TitleWatcher title_watcher(shell()->web_contents(), expected_title);
    NavigateToURL(shell(), prefetch_html_url);
    EXPECT_EQ(expected_title, title_watcher.WaitAndGetTitle());
  }

  void SetAcceptLangs(const std::string langs) {
    client_.SetAcceptLangs(langs);
    StoragePartitionImpl* partition = static_cast<StoragePartitionImpl*>(
        BrowserContext::GetDefaultStoragePartition(
            shell()->web_contents()->GetBrowserContext()));
    partition->GetPrefetchURLLoaderService()->SetAcceptLanguages(langs);
  }

  const base::HistogramTester histogram_tester_;

 private:
  ContentBrowserClient* original_client_ = nullptr;
  MockContentBrowserClient client_;

  base::test::ScopedFeatureList feature_list_;
  SignedExchangeBrowserTestHelper sxg_test_helper_;

  DISALLOW_COPY_AND_ASSIGN(SignedExchangeRequestHandlerBrowserTestBase);
};

enum class SignedExchangeRequestHandlerBrowserTestPrefetchParam {
  kPrefetchDisabled,
  kPrefetchEnabled
};

class SignedExchangeRequestHandlerBrowserTest
    : public SignedExchangeRequestHandlerBrowserTestBase,
      public testing::WithParamInterface<
          SignedExchangeRequestHandlerBrowserTestPrefetchParam> {
 public:
  SignedExchangeRequestHandlerBrowserTest() = default;

 protected:
  bool PrefetchIsEnabled() {
    return GetParam() == SignedExchangeRequestHandlerBrowserTestPrefetchParam::
                             kPrefetchEnabled;
  }

 private:
  DISALLOW_COPY_AND_ASSIGN(SignedExchangeRequestHandlerBrowserTest);
};

IN_PROC_BROWSER_TEST_P(SignedExchangeRequestHandlerBrowserTest, Simple) {
  InstallMockCert();
  InstallMockCertChainInterceptor();

  embedded_test_server()->ServeFilesFromSourceDirectory("content/test/data");
  ASSERT_TRUE(embedded_test_server()->Start());
  GURL url = embedded_test_server()->GetURL("/sxg/test.example.org_test.sxg");
  if (PrefetchIsEnabled())
    TriggerPrefetch(url, true);

  base::string16 title = base::ASCIIToUTF16("https://test.example.org/test/");
  TitleWatcher title_watcher(shell()->web_contents(), title);
  RedirectObserver redirect_observer(shell()->web_contents());
  AssertNavigationHandleFlagObserver assert_navigation_handle_flag_observer(
      shell()->web_contents());

  NavigateToURL(shell(), url);
  EXPECT_EQ(title, title_watcher.WaitAndGetTitle());
  EXPECT_EQ(303, redirect_observer.response_code());

  NavigationEntry* entry =
      shell()->web_contents()->GetController().GetVisibleEntry();
  EXPECT_TRUE(entry->GetSSL().initialized);
  EXPECT_FALSE(!!(entry->GetSSL().content_status &
                  SSLStatus::DISPLAYED_INSECURE_CONTENT));
  ASSERT_TRUE(entry->GetSSL().certificate);

  // "test.example.org.public.pem.cbor" is generated from
  // "prime256v1-sha256.public.pem". So the SHA256 of the certificates must
  // match.
  const net::SHA256HashValue fingerprint =
      net::X509Certificate::CalculateFingerprint256(
          entry->GetSSL().certificate->cert_buffer());
  scoped_refptr<net::X509Certificate> original_cert =
      SignedExchangeBrowserTestHelper::LoadCertificate();
  const net::SHA256HashValue original_fingerprint =
      net::X509Certificate::CalculateFingerprint256(
          original_cert->cert_buffer());
  EXPECT_EQ(original_fingerprint, fingerprint);
  histogram_tester_.ExpectUniqueSample(kLoadResultHistogram,
                                       SignedExchangeLoadResult::kSuccess,
                                       PrefetchIsEnabled() ? 2 : 1);
  histogram_tester_.ExpectTotalCount(
      "SignedExchange.Time.CertificateFetch.Success",
      PrefetchIsEnabled() ? 2 : 1);
  if (PrefetchIsEnabled()) {
    histogram_tester_.ExpectUniqueSample(kPrefetchResultHistogram,
                                         SignedExchangeLoadResult::kSuccess, 1);
    histogram_tester_.ExpectUniqueSample(
        "SignedExchange.Prefetch.Recall.30Seconds", true, 1);
    histogram_tester_.ExpectUniqueSample(
        "SignedExchange.Prefetch.Precision.30Seconds", true, 1);
  } else {
    histogram_tester_.ExpectUniqueSample(
        "SignedExchange.Prefetch.Recall.30Seconds", false, 1);
  }
}

IN_PROC_BROWSER_TEST_P(SignedExchangeRequestHandlerBrowserTest, VariantMatch) {
  SetAcceptLangs("en-US,fr");
  InstallUrlInterceptor(
      GURL("https://cert.example.org/cert.msg"),
      "content/test/data/sxg/test.example.org.public.pem.cbor");
  InstallMockCert();

  embedded_test_server()->ServeFilesFromSourceDirectory("content/test/data");
  ASSERT_TRUE(embedded_test_server()->Start());
  GURL url =
      embedded_test_server()->GetURL("/sxg/test.example.org_fr_variant.sxg");
  if (PrefetchIsEnabled())
    TriggerPrefetch(url, true);

  base::string16 title = base::ASCIIToUTF16("https://test.example.org/test/");
  TitleWatcher title_watcher(shell()->web_contents(), title);
  NavigateToURL(shell(), url);
  EXPECT_EQ(title, title_watcher.WaitAndGetTitle());
  histogram_tester_.ExpectUniqueSample(kLoadResultHistogram,
                                       SignedExchangeLoadResult::kSuccess,
                                       PrefetchIsEnabled() ? 2 : 1);
}

IN_PROC_BROWSER_TEST_P(SignedExchangeRequestHandlerBrowserTest,
                       VariantMismatch) {
  SetAcceptLangs("en-US,ja");
  InstallUrlInterceptor(
      GURL("https://cert.example.org/cert.msg"),
      "content/test/data/sxg/test.example.org.public.pem.cbor");
  InstallUrlInterceptor(GURL("https://test.example.org/test/"),
                        "content/test/data/sxg/fallback.html");
  InstallMockCert();

  embedded_test_server()->ServeFilesFromSourceDirectory("content/test/data");
  ASSERT_TRUE(embedded_test_server()->Start());
  GURL url =
      embedded_test_server()->GetURL("/sxg/test.example.org_fr_variant.sxg");
  if (PrefetchIsEnabled())
    TriggerPrefetch(url, false);

  base::string16 title = base::ASCIIToUTF16("Fallback URL response");
  TitleWatcher title_watcher(shell()->web_contents(), title);
  NavigateToURL(shell(), url);
  EXPECT_EQ(title, title_watcher.WaitAndGetTitle());
  histogram_tester_.ExpectUniqueSample(
      kLoadResultHistogram, SignedExchangeLoadResult::kVariantMismatch,
      PrefetchIsEnabled() ? 2 : 1);
}

IN_PROC_BROWSER_TEST_P(SignedExchangeRequestHandlerBrowserTest,
                       MissingNosniff) {
  InstallUrlInterceptor(GURL("https://test.example.org/test/"),
                        "content/test/data/sxg/fallback.html");
  InstallMockCert();

  embedded_test_server()->ServeFilesFromSourceDirectory("content/test/data");
  ASSERT_TRUE(embedded_test_server()->Start());
  GURL url = embedded_test_server()->GetURL(
      "/sxg/test.example.org_test_missing_nosniff.sxg");
  if (PrefetchIsEnabled())
    TriggerPrefetch(url, false);

  base::string16 title = base::ASCIIToUTF16("Fallback URL response");
  TitleWatcher title_watcher(shell()->web_contents(), title);
  RedirectObserver redirect_observer(shell()->web_contents());
  NavigateToURL(shell(), url);
  EXPECT_EQ(title, title_watcher.WaitAndGetTitle());
  EXPECT_EQ(303, redirect_observer.response_code());
  histogram_tester_.ExpectUniqueSample(
      kLoadResultHistogram, SignedExchangeLoadResult::kSXGServedWithoutNosniff,
      PrefetchIsEnabled() ? 2 : 1);
}

IN_PROC_BROWSER_TEST_P(SignedExchangeRequestHandlerBrowserTest,
                       InvalidContentType) {
  InstallUrlInterceptor(GURL("https://test.example.org/test/"),
                        "content/test/data/sxg/fallback.html");
  InstallMockCert();
  InstallMockCertChainInterceptor();

  embedded_test_server()->ServeFilesFromSourceDirectory("content/test/data");
  ASSERT_TRUE(embedded_test_server()->Start());
  GURL url = embedded_test_server()->GetURL(
      "/sxg/test.example.org_test_invalid_content_type.sxg");
  if (PrefetchIsEnabled())
    TriggerPrefetch(url, false);

  base::string16 title = base::ASCIIToUTF16("Fallback URL response");
  TitleWatcher title_watcher(shell()->web_contents(), title);
  RedirectObserver redirect_observer(shell()->web_contents());
  NavigateToURL(shell(), url);
  EXPECT_EQ(title, title_watcher.WaitAndGetTitle());
  EXPECT_EQ(303, redirect_observer.response_code());
  histogram_tester_.ExpectUniqueSample(
      kLoadResultHistogram, SignedExchangeLoadResult::kVersionMismatch,
      PrefetchIsEnabled() ? 2 : 1);
}

IN_PROC_BROWSER_TEST_P(SignedExchangeRequestHandlerBrowserTest, Expired) {
  SignedExchangeHandler::SetVerificationTimeForTesting(
      base::Time::UnixEpoch() +
      base::TimeDelta::FromSeconds(
          SignedExchangeBrowserTestHelper::kSignatureHeaderExpires + 1));

  InstallMockCertChainInterceptor();
  InstallUrlInterceptor(GURL("https://test.example.org/test/"),
                        "content/test/data/sxg/fallback.html");
  InstallMockCert();

  embedded_test_server()->ServeFilesFromSourceDirectory("content/test/data");
  ASSERT_TRUE(embedded_test_server()->Start());
  GURL url = embedded_test_server()->GetURL("/sxg/test.example.org_test.sxg");

  base::string16 title = base::ASCIIToUTF16("Fallback URL response");
  TitleWatcher title_watcher(shell()->web_contents(), title);
  RedirectObserver redirect_observer(shell()->web_contents());
  NavigateToURL(shell(), url);
  EXPECT_EQ(title, title_watcher.WaitAndGetTitle());
  EXPECT_EQ(303, redirect_observer.response_code());
  histogram_tester_.ExpectUniqueSample(
      kLoadResultHistogram,
      SignedExchangeLoadResult::kSignatureVerificationError, 1);
  histogram_tester_.ExpectUniqueSample(
      "SignedExchange.SignatureVerificationResult",
      SignedExchangeSignatureVerifier::Result::kErrExpired, 1);
}

IN_PROC_BROWSER_TEST_P(SignedExchangeRequestHandlerBrowserTest,
                       RedirectBrokenSignedExchanges) {
  InstallUrlInterceptor(GURL("https://test.example.org/test/"),
                        "content/test/data/sxg/fallback.html");

  embedded_test_server()->ServeFilesFromSourceDirectory("content/test/data");
  ASSERT_TRUE(embedded_test_server()->Start());

  constexpr const char* kBrokenExchanges[] = {
      "/sxg/test.example.org_test_invalid_magic_string.sxg",
      "/sxg/test.example.org_test_invalid_cbor_header.sxg",
  };

  for (const auto* broken_exchange : kBrokenExchanges) {
    SCOPED_TRACE(testing::Message()
                 << "testing broken exchange: " << broken_exchange);

    GURL url = embedded_test_server()->GetURL(broken_exchange);

    if (PrefetchIsEnabled())
      TriggerPrefetch(url, false);

    base::string16 title = base::ASCIIToUTF16("Fallback URL response");
    TitleWatcher title_watcher(shell()->web_contents(), title);
    NavigateToURL(shell(), url);
    EXPECT_EQ(title, title_watcher.WaitAndGetTitle());
  }
  histogram_tester_.ExpectTotalCount(kLoadResultHistogram,
                                     PrefetchIsEnabled() ? 4 : 2);
  histogram_tester_.ExpectBucketCount(
      kLoadResultHistogram, SignedExchangeLoadResult::kVersionMismatch,
      PrefetchIsEnabled() ? 2 : 1);
  histogram_tester_.ExpectBucketCount(
      kLoadResultHistogram, SignedExchangeLoadResult::kHeaderParseError,
      PrefetchIsEnabled() ? 2 : 1);
}

IN_PROC_BROWSER_TEST_P(SignedExchangeRequestHandlerBrowserTest, CertNotFound) {
  InstallUrlInterceptor(GURL("https://cert.example.org/cert.msg"),
                        "content/test/data/sxg/404.msg");
  InstallUrlInterceptor(GURL("https://test.example.org/test/"),
                        "content/test/data/sxg/fallback.html");

  embedded_test_server()->ServeFilesFromSourceDirectory("content/test/data");
  ASSERT_TRUE(embedded_test_server()->Start());
  GURL url = embedded_test_server()->GetURL("/sxg/test.example.org_test.sxg");

  if (PrefetchIsEnabled())
    TriggerPrefetch(url, false);

  base::string16 title = base::ASCIIToUTF16("Fallback URL response");
  TitleWatcher title_watcher(shell()->web_contents(), title);
  NavigateToURL(shell(), url);
  EXPECT_EQ(title, title_watcher.WaitAndGetTitle());
  histogram_tester_.ExpectUniqueSample(
      kLoadResultHistogram, SignedExchangeLoadResult::kCertFetchError,
      PrefetchIsEnabled() ? 2 : 1);
  histogram_tester_.ExpectTotalCount(
      "SignedExchange.Time.CertificateFetch.Failure",
      PrefetchIsEnabled() ? 2 : 1);
}

INSTANTIATE_TEST_SUITE_P(
    SignedExchangeRequestHandlerBrowserTest,
    SignedExchangeRequestHandlerBrowserTest,
    testing::Values(
        SignedExchangeRequestHandlerBrowserTestPrefetchParam::kPrefetchDisabled,
        SignedExchangeRequestHandlerBrowserTestPrefetchParam::
            kPrefetchEnabled));

class SignedExchangeRequestHandlerDownloadBrowserTest
    : public SignedExchangeRequestHandlerBrowserTestBase {
 public:
  SignedExchangeRequestHandlerDownloadBrowserTest() = default;
  ~SignedExchangeRequestHandlerDownloadBrowserTest() override = default;

 protected:
  class DownloadObserver : public DownloadManager::Observer {
   public:
    DownloadObserver(DownloadManager* manager) : manager_(manager) {
      manager_->AddObserver(this);
    }
    ~DownloadObserver() override { manager_->RemoveObserver(this); }

    void WaitUntilDownloadCreated() { run_loop_.Run(); }

    const GURL& observed_url() const { return url_; }
    const std::string& observed_content_disposition() const {
      return content_disposition_;
    }

    // content::DownloadManager::Observer implementation.
    void OnDownloadCreated(content::DownloadManager* manager,
                           download::DownloadItem* item) override {
      url_ = item->GetURL();
      content_disposition_ = item->GetContentDisposition();
      run_loop_.Quit();
    }

   private:
    DownloadManager* manager_;
    base::RunLoop run_loop_;
    GURL url_;
    std::string content_disposition_;
  };

  void SetUpOnMainThread() override {
    SignedExchangeRequestHandlerBrowserTestBase::SetUpOnMainThread();
    ASSERT_TRUE(downloads_directory_.CreateUniqueTempDir());
    ShellDownloadManagerDelegate* delegate =
        static_cast<ShellDownloadManagerDelegate*>(
            shell()
                ->web_contents()
                ->GetBrowserContext()
                ->GetDownloadManagerDelegate());
    delegate->SetDownloadBehaviorForTesting(downloads_directory_.GetPath());
  }

 private:
  base::ScopedTempDir downloads_directory_;
};

IN_PROC_BROWSER_TEST_F(SignedExchangeRequestHandlerDownloadBrowserTest,
                       Download) {
  std::unique_ptr<DownloadObserver> observer =
      std::make_unique<DownloadObserver>(BrowserContext::GetDownloadManager(
          shell()->web_contents()->GetBrowserContext()));

  embedded_test_server()->ServeFilesFromSourceDirectory("content/test/data");
  ASSERT_TRUE(embedded_test_server()->Start());
  NavigateToURL(shell(), embedded_test_server()->GetURL("/empty.html"));

  const std::string load_sxg =
      "const iframe = document.createElement('iframe');"
      "iframe.src = './sxg/test.example.org_test_download.sxg';"
      "document.body.appendChild(iframe);";
  EXPECT_TRUE(ExecuteScript(shell()->web_contents(), load_sxg));
  observer->WaitUntilDownloadCreated();
  EXPECT_EQ(
      embedded_test_server()->GetURL("/sxg/test.example.org_test_download.sxg"),
      observer->observed_url());
  EXPECT_EQ("attachment; filename=test.sxg",
            observer->observed_content_disposition());
}

IN_PROC_BROWSER_TEST_F(SignedExchangeRequestHandlerDownloadBrowserTest,
                       DataURLDownload) {
  const GURL sxg_url = GURL("data:application/signed-exchange,");
  std::unique_ptr<DownloadObserver> observer =
      std::make_unique<DownloadObserver>(BrowserContext::GetDownloadManager(
          shell()->web_contents()->GetBrowserContext()));

  embedded_test_server()->ServeFilesFromSourceDirectory("content/test/data");
  ASSERT_TRUE(embedded_test_server()->Start());
  NavigateToURL(shell(), embedded_test_server()->GetURL("/empty.html"));

  const std::string load_sxg = base::StringPrintf(
      "const iframe = document.createElement('iframe');"
      "iframe.src = '%s';"
      "document.body.appendChild(iframe);",
      sxg_url.spec().c_str());
  EXPECT_TRUE(ExecuteScript(shell()->web_contents(), load_sxg));
  observer->WaitUntilDownloadCreated();
  EXPECT_EQ(sxg_url, observer->observed_url());
}

class SignedExchangeRequestHandlerRealCertVerifierBrowserTest
    : public SignedExchangeRequestHandlerBrowserTestBase {
 public:
  SignedExchangeRequestHandlerRealCertVerifierBrowserTest() {
    // Use "real" CertVerifier.
    disable_mock_cert_verifier();
  }
};

IN_PROC_BROWSER_TEST_F(SignedExchangeRequestHandlerRealCertVerifierBrowserTest,
                       Basic) {
  InstallMockCertChainInterceptor();
  InstallUrlInterceptor(GURL("https://test.example.org/test/"),
                        "content/test/data/sxg/fallback.html");

  embedded_test_server()->ServeFilesFromSourceDirectory("content/test/data");
  ASSERT_TRUE(embedded_test_server()->Start());
  GURL url = embedded_test_server()->GetURL("/sxg/test.example.org_test.sxg");

  // "test.example.org_test.sxg" should pass CertVerifier::Verify() and then
  // fail at SignedExchangeHandler::CheckOCSPStatus() because of the dummy OCSP
  // response.
  // TODO(https://crbug.com/815024): Make this test pass the OCSP check. We'll
  // need to either generate an OCSP response on the fly, or override the OCSP
  // verification time.
  base::string16 title = base::ASCIIToUTF16("Fallback URL response");
  TitleWatcher title_watcher(shell()->web_contents(), title);
  NavigateToURL(shell(), url);
  EXPECT_EQ(title, title_watcher.WaitAndGetTitle());
  // Verify that it failed at the OCSP check step.
  histogram_tester_.ExpectUniqueSample(kLoadResultHistogram,
                                       SignedExchangeLoadResult::kOCSPError, 1);
}

IN_PROC_BROWSER_TEST_P(SignedExchangeRequestHandlerBrowserTest,
                       LogicalUrlInServiceWorkerScope) {
  // SW-scope: https://test.example.org/test/
  // SXG physical URL: http://127.0.0.1:PORT/sxg/test.example.org_test.sxg
  // SXG logical URL: https://test.example.org/test/
  InstallMockCert();
  InstallMockCertChainInterceptor();

  const GURL install_sw_url =
      GURL("https://test.example.org/test/publisher-service-worker.html");

  InstallUrlInterceptor(install_sw_url,
                        "content/test/data/sxg/publisher-service-worker.html");
  InstallUrlInterceptor(
      GURL("https://test.example.org/test/publisher-service-worker.js"),
      "content/test/data/sxg/publisher-service-worker.js");
  {
    base::string16 title = base::ASCIIToUTF16("Done");
    TitleWatcher title_watcher(shell()->web_contents(), title);
    NavigateToURL(shell(), install_sw_url);
    EXPECT_EQ(title, title_watcher.WaitAndGetTitle());
  }
  embedded_test_server()->ServeFilesFromSourceDirectory("content/test/data");
  ASSERT_TRUE(embedded_test_server()->Start());

  GURL url = embedded_test_server()->GetURL("/sxg/test.example.org_test.sxg");

  base::string16 title = base::ASCIIToUTF16("https://test.example.org/test/");
  TitleWatcher title_watcher(shell()->web_contents(), title);
  title_watcher.AlsoWaitForTitle(base::ASCIIToUTF16("Generated"));
  NavigateToURL(shell(), url);
  // The page content shoud be served from the signed exchange.
  EXPECT_EQ(title, title_watcher.WaitAndGetTitle());
}

IN_PROC_BROWSER_TEST_P(SignedExchangeRequestHandlerBrowserTest,
                       NotControlledByDistributorsSW) {
  // SW-scope: http://127.0.0.1:PORT/sxg/
  // SXG physical URL: http://127.0.0.1:PORT/sxg/test.example.org_test.sxg
  // SXG logical URL: https://test.example.org/test/
  InstallMockCert();
  InstallMockCertChainInterceptor();
  embedded_test_server()->ServeFilesFromSourceDirectory("content/test/data");
  ASSERT_TRUE(embedded_test_server()->Start());

  const GURL install_sw_url = embedded_test_server()->GetURL(
      "/sxg/no-respond-with-service-worker.html");

  {
    base::string16 title = base::ASCIIToUTF16("Done");
    TitleWatcher title_watcher(shell()->web_contents(), title);
    NavigateToURL(shell(), install_sw_url);
    EXPECT_EQ(title, title_watcher.WaitAndGetTitle());
  }

  base::string16 title = base::ASCIIToUTF16("https://test.example.org/test/");
  TitleWatcher title_watcher(shell()->web_contents(), title);
  NavigateToURL(shell(), embedded_test_server()->GetURL(
                             "/sxg/test.example.org_test.sxg"));
  EXPECT_EQ(title, title_watcher.WaitAndGetTitle());

  // The page must not be controlled by the service worker of the physical URL.
  EXPECT_EQ(false, EvalJs(shell(), "!!navigator.serviceWorker.controller"));
}

IN_PROC_BROWSER_TEST_P(SignedExchangeRequestHandlerBrowserTest,
                       NotControlledBySameOriginDistributorsSW) {
  // SW-scope: https://test.example.org/scope/
  // SXG physical URL: https://test.example.org/scope/test.example.org_test.sxg
  // SXG logical URL: https://test.example.org/test/
  InstallMockCert();
  InstallMockCertChainInterceptor();

  InstallUrlInterceptor(GURL("https://test.example.org/scope/test.sxg"),
                        "content/test/data/sxg/test.example.org_test.sxg");

  const GURL install_sw_url = GURL(
      "https://test.example.org/scope/no-respond-with-service-worker.html");

  InstallUrlInterceptor(
      install_sw_url,
      "content/test/data/sxg/no-respond-with-service-worker.html");
  InstallUrlInterceptor(
      GURL("https://test.example.org/scope/no-respond-with-service-worker.js"),
      "content/test/data/sxg/no-respond-with-service-worker.js");

  {
    base::string16 title = base::ASCIIToUTF16("Done");
    TitleWatcher title_watcher(shell()->web_contents(), title);
    NavigateToURL(shell(), install_sw_url);
    EXPECT_EQ(title, title_watcher.WaitAndGetTitle());
  }

  base::string16 title = base::ASCIIToUTF16("https://test.example.org/test/");
  TitleWatcher title_watcher(shell()->web_contents(), title);
  NavigateToURL(shell(), GURL("https://test.example.org/scope/test.sxg"));
  EXPECT_EQ(title, title_watcher.WaitAndGetTitle());

  // The page must not be controlled by the service worker of the physical URL.
  EXPECT_EQ(false, EvalJs(shell(), "!!navigator.serviceWorker.controller"));
}

IN_PROC_BROWSER_TEST_P(SignedExchangeRequestHandlerBrowserTest,
                       RegisterServiceWorkerFromSignedExchange) {
  // SXG physical URL: http://127.0.0.1:PORT/sxg/test.example.org_test.sxg
  // SXG logical URL: https://test.example.org/test/
  InstallMockCert();
  InstallMockCertChainInterceptor();

  InstallUrlInterceptor(
      GURL("https://test.example.org/test/publisher-service-worker.js"),
      "content/test/data/sxg/publisher-service-worker.js");

  embedded_test_server()->ServeFilesFromSourceDirectory("content/test/data");
  ASSERT_TRUE(embedded_test_server()->Start());

  GURL url = embedded_test_server()->GetURL("/sxg/test.example.org_test.sxg");

  {
    base::string16 title = base::ASCIIToUTF16("https://test.example.org/test/");
    TitleWatcher title_watcher(shell()->web_contents(), title);
    NavigateToURL(shell(), url);
    EXPECT_EQ(title, title_watcher.WaitAndGetTitle());
  }

  const std::string register_sw_script =
      "(async function() {"
      "  try {"
      "    const registration = await navigator.serviceWorker.register("
      "        'publisher-service-worker.js', {scope: './'});"
      "    window.domAutomationController.send(true);"
      "  } catch (e) {"
      "    window.domAutomationController.send(false);"
      "  }"
      "})();";
  bool result = false;
  EXPECT_TRUE(ExecuteScriptAndExtractBool(shell()->web_contents(),
                                          register_sw_script, &result));
  // serviceWorker.register() fails because the document URL of
  // ServiceWorkerProviderHost is empty.
  EXPECT_FALSE(result);
}

class SignedExchangeAcceptHeaderBrowserTest
    : public ContentBrowserTest,
      public testing::WithParamInterface<bool> {
 public:
  using self = SignedExchangeAcceptHeaderBrowserTest;
  SignedExchangeAcceptHeaderBrowserTest()
      : https_server_(net::EmbeddedTestServer::TYPE_HTTPS) {}
  ~SignedExchangeAcceptHeaderBrowserTest() override = default;

 protected:
  void SetUp() override {
    if (GetParam()) {
      feature_list_.InitAndEnableFeature(features::kSignedHTTPExchange);
    } else {
      feature_list_.InitAndDisableFeature(features::kSignedHTTPExchange);
    }

    https_server_.ServeFilesFromSourceDirectory("content/test/data");
    https_server_.RegisterRequestHandler(
        base::BindRepeating(&self::RedirectResponseHandler));
    https_server_.RegisterRequestMonitor(
        base::BindRepeating(&self::MonitorRequest, base::Unretained(this)));
    ASSERT_TRUE(https_server_.Start());

    ContentBrowserTest::SetUp();
  }

  void NavigateAndWaitForTitle(const GURL& url, const std::string title) {
    base::string16 expected_title = base::ASCIIToUTF16(title);
    TitleWatcher title_watcher(shell()->web_contents(), expected_title);
    NavigateToURL(shell(), url);
    EXPECT_EQ(expected_title, title_watcher.WaitAndGetTitle());
  }

  bool ShouldHaveSXGAcceptHeaderInEnabledOrigin() const { return GetParam(); }

  bool ShouldHaveSXGAcceptHeader() const { return GetParam(); }

  void CheckAcceptHeader(const GURL& url, bool is_navigation) {
    const auto accept_header = GetInterceptedAcceptHeader(url);
    ASSERT_TRUE(accept_header);
    EXPECT_EQ(
        *accept_header,
        ShouldHaveSXGAcceptHeader()
            ? (is_navigation
                   ? std::string(network::kFrameAcceptHeader) +
                         std::string(kAcceptHeaderSignedExchangeSuffix)
                   : std::string(kExpectedSXGEnabledAcceptHeaderForPrefetch))
            : (is_navigation ? std::string(network::kFrameAcceptHeader)
                             : std::string(network::kDefaultAcceptHeader)));
  }

  void CheckNavigationAcceptHeader(const std::vector<GURL>& urls) {
    for (const auto& url : urls) {
      SCOPED_TRACE(url);
      CheckAcceptHeader(url, true /* is_navigation */);
    }
  }

  void CheckPrefetchAcceptHeader(const std::vector<GURL>& urls) {
    for (const auto& url : urls) {
      SCOPED_TRACE(url);
      CheckAcceptHeader(url, false /* is_navigation */);
    }
  }

  base::Optional<std::string> GetInterceptedAcceptHeader(
      const GURL& url) const {
    const auto it = url_accept_header_map_.find(url);
    if (it == url_accept_header_map_.end())
      return base::nullopt;
    return it->second;
  }

  void ClearInterceptedAcceptHeaders() { url_accept_header_map_.clear(); }

  net::EmbeddedTestServer https_server_;

 private:
  static std::unique_ptr<net::test_server::HttpResponse>
  RedirectResponseHandler(const net::test_server::HttpRequest& request) {
    if (!base::StartsWith(request.relative_url, "/r?",
                          base::CompareCase::SENSITIVE)) {
      return std::unique_ptr<net::test_server::HttpResponse>();
    }
    std::unique_ptr<net::test_server::BasicHttpResponse> http_response(
        new net::test_server::BasicHttpResponse);
    http_response->set_code(net::HTTP_MOVED_PERMANENTLY);
    http_response->AddCustomHeader("Location", request.relative_url.substr(3));
    http_response->AddCustomHeader("Cache-Control", "no-cache");
    return std::move(http_response);
  }

  void MonitorRequest(const net::test_server::HttpRequest& request) {
    const auto it = request.headers.find(std::string(network::kAcceptHeader));
    if (it == request.headers.end())
      return;
    url_accept_header_map_[request.base_url.Resolve(request.relative_url)] =
        it->second;
  }

  base::test::ScopedFeatureList feature_list_;
  base::test::ScopedFeatureList feature_list_for_accept_header_;

  std::map<GURL, std::string> url_accept_header_map_;
};

IN_PROC_BROWSER_TEST_P(SignedExchangeAcceptHeaderBrowserTest, Simple) {
  const GURL test_url = https_server_.GetURL("/sxg/test.html");
  EXPECT_EQ(ShouldHaveSXGAcceptHeaderInEnabledOrigin(),
            signed_exchange_utils::IsSignedExchangeHandlingEnabled());
  NavigateAndWaitForTitle(test_url, test_url.spec());
  CheckNavigationAcceptHeader({test_url});
}

IN_PROC_BROWSER_TEST_P(SignedExchangeAcceptHeaderBrowserTest, Redirect) {
  const GURL test_url = https_server_.GetURL("/sxg/test.html");
  const GURL redirect_url = https_server_.GetURL("/r?" + test_url.spec());
  const GURL redirect_redirect_url =
      https_server_.GetURL("/r?" + redirect_url.spec());
  NavigateAndWaitForTitle(redirect_redirect_url, test_url.spec());

  CheckNavigationAcceptHeader({redirect_redirect_url, redirect_url, test_url});
}

IN_PROC_BROWSER_TEST_P(SignedExchangeAcceptHeaderBrowserTest,
                       PrefetchEnabledPageEnabledTarget) {
  const GURL target = https_server_.GetURL("/sxg/hello.txt");
  const GURL page_url =
      https_server_.GetURL(std::string("/sxg/prefetch.html#") + target.spec());
  NavigateAndWaitForTitle(page_url, "OK");
  CheckPrefetchAcceptHeader({target});
}

IN_PROC_BROWSER_TEST_P(SignedExchangeAcceptHeaderBrowserTest,
                       PrefetchRedirect) {
  const GURL target = https_server_.GetURL("/sxg/hello.txt");
  const GURL redirect_url = https_server_.GetURL("/r?" + target.spec());
  const GURL redirect_redirect_url =
      https_server_.GetURL("/r?" + redirect_url.spec());

  const GURL page_url = https_server_.GetURL(
      std::string("/sxg/prefetch.html#") + redirect_redirect_url.spec());

  NavigateAndWaitForTitle(page_url, "OK");

  CheckPrefetchAcceptHeader({redirect_redirect_url, redirect_url, target});
}

IN_PROC_BROWSER_TEST_P(SignedExchangeAcceptHeaderBrowserTest, ServiceWorker) {
  NavigateAndWaitForTitle(https_server_.GetURL("/sxg/service-worker.html"),
                          "Done");

  const std::string frame_accept = std::string(network::kFrameAcceptHeader);
  const std::string frame_accept_with_sxg =
      frame_accept + std::string(kAcceptHeaderSignedExchangeSuffix);
  const std::vector<std::string> scopes = {"/sxg/sw-scope-generated/",
                                           "/sxg/sw-scope-navigation-preload/",
                                           "/sxg/sw-scope-no-respond-with/"};
  for (const auto& scope : scopes) {
    SCOPED_TRACE(scope);
    const bool is_generated_scope =
        scope == std::string("/sxg/sw-scope-generated/");
    const GURL target_url = https_server_.GetURL(scope + "test.html");
    const GURL redirect_target_url =
        https_server_.GetURL("/r?" + target_url.spec());
    const GURL redirect_redirect_target_url =
        https_server_.GetURL("/r?" + redirect_target_url.spec());

    const std::string expected_title =
        is_generated_scope
            ? (ShouldHaveSXGAcceptHeader() ? frame_accept_with_sxg
                                           : frame_accept)
            : "Done";
    const base::Optional<std::string> expected_target_accept_header =
        is_generated_scope
            ? base::nullopt
            : base::Optional<std::string>(ShouldHaveSXGAcceptHeader()
                                              ? frame_accept_with_sxg
                                              : frame_accept);

    NavigateAndWaitForTitle(target_url, expected_title);
    EXPECT_EQ(expected_target_accept_header,
              GetInterceptedAcceptHeader(target_url));
    ClearInterceptedAcceptHeaders();

    NavigateAndWaitForTitle(redirect_target_url, expected_title);
    CheckNavigationAcceptHeader({redirect_target_url});
    EXPECT_EQ(expected_target_accept_header,
              GetInterceptedAcceptHeader(target_url));
    ClearInterceptedAcceptHeaders();

    NavigateAndWaitForTitle(redirect_redirect_target_url, expected_title);
    CheckNavigationAcceptHeader(
        {redirect_redirect_target_url, redirect_target_url});
    EXPECT_EQ(expected_target_accept_header,
              GetInterceptedAcceptHeader(target_url));
    ClearInterceptedAcceptHeaders();
  }
}

IN_PROC_BROWSER_TEST_P(SignedExchangeAcceptHeaderBrowserTest,
                       ServiceWorkerPrefetch) {
  NavigateAndWaitForTitle(
      https_server_.GetURL("/sxg/service-worker-prefetch.html"), "Done");
  const std::string scope = "/sxg/sw-prefetch-scope/";
  const GURL target_url = https_server_.GetURL(scope + "test.html");

  const GURL prefetch_target =
      https_server_.GetURL(std::string("/sxg/hello.txt"));
  const std::string load_prefetch_script = base::StringPrintf(
      "(function loadPrefetch(urls) {"
      "  for (let url of urls) {"
      "    let link = document.createElement('link');"
      "    link.rel = 'prefetch';"
      "    link.href = url;"
      "    document.body.appendChild(link);"
      "  }"
      "  function check() {"
      "    const entries = performance.getEntriesByType('resource');"
      "    const url_set = new Set(urls);"
      "    for (let entry of entries) {"
      "      url_set.delete(entry.name);"
      "    }"
      "    if (!url_set.size) {"
      "      window.domAutomationController.send(true);"
      "    } else {"
      "      setTimeout(check, 100);"
      "    }"
      "  }"
      "  check();"
      "})(['%s'])",
      prefetch_target.spec().c_str());
  bool unused = false;

  NavigateAndWaitForTitle(target_url, "Done");
  EXPECT_TRUE(ExecuteScriptAndExtractBool(shell()->web_contents(),
                                          load_prefetch_script, &unused));
  CheckPrefetchAcceptHeader({prefetch_target});
  ClearInterceptedAcceptHeaders();
}

INSTANTIATE_TEST_SUITE_P(SignedExchangeAcceptHeaderBrowserTest,
                         SignedExchangeAcceptHeaderBrowserTest,
                         testing::Bool());

}  // namespace content
