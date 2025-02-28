// Copyright 2016 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/ui/webui/settings/site_settings_handler.h"

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "base/bind_helpers.h"
#include "base/json/json_reader.h"
#include "base/test/metrics/histogram_tester.h"
#include "base/test/simple_test_clock.h"
#include "base/values.h"
#include "build/build_config.h"
#include "chrome/browser/browsing_data/browsing_data_flash_lso_helper.h"
#include "chrome/browser/browsing_data/mock_browsing_data_cookie_helper.h"
#include "chrome/browser/browsing_data/mock_browsing_data_local_storage_helper.h"
#include "chrome/browser/chrome_notification_types.h"
#include "chrome/browser/content_settings/host_content_settings_map_factory.h"
#include "chrome/browser/extensions/extension_service.h"
#include "chrome/browser/extensions/test_extension_system.h"
#include "chrome/browser/infobars/infobar_service.h"
#include "chrome/browser/permissions/chooser_context_base.h"
#include "chrome/browser/permissions/chooser_context_base_mock_permission_observer.h"
#include "chrome/browser/permissions/permission_decision_auto_blocker.h"
#include "chrome/browser/ui/browser_window.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/browser/ui/webui/site_settings_helper.h"
#include "chrome/browser/usb/usb_chooser_context.h"
#include "chrome/browser/usb/usb_chooser_context_factory.h"
#include "chrome/common/pref_names.h"
#include "chrome/test/base/browser_with_test_window_test.h"
#include "chrome/test/base/testing_profile.h"
#include "components/content_settings/core/browser/host_content_settings_map.h"
#include "components/content_settings/core/common/content_settings.h"
#include "components/content_settings/core/common/content_settings_types.h"
#include "components/content_settings/core/common/pref_names.h"
#include "components/infobars/core/infobar.h"
#include "components/sync_preferences/testing_pref_service_syncable.h"
#include "content/public/browser/navigation_controller.h"
#include "content/public/browser/notification_service.h"
#include "content/public/browser/web_ui_data_source.h"
#include "content/public/test/test_browser_thread_bundle.h"
#include "content/public/test/test_web_ui.h"
#include "device/usb/public/cpp/fake_usb_device_manager.h"
#include "extensions/browser/extension_registry.h"
#include "extensions/common/extension_builder.h"
#include "ppapi/buildflags/buildflags.h"
#include "testing/gtest/include/gtest/gtest.h"

#if defined(OS_CHROMEOS)
#include "chrome/browser/chromeos/login/users/mock_user_manager.h"
#include "components/user_manager/scoped_user_manager.h"
#endif

#if BUILDFLAG(ENABLE_PLUGINS)
#include "chrome/browser/plugins/chrome_plugin_service_filter.h"
#endif

namespace {

constexpr char kCallbackId[] = "test-callback-id";
constexpr char kSetting[] = "setting";
constexpr char kSource[] = "source";
constexpr char kExtensionName[] = "Test Extension";

const struct PatternContentTypeTestCase {
  struct {
    const char* const pattern;
    const char* const content_type;
  } arguments;
  struct {
    const bool validity;
    const char* const reason;
  } expected;
} kPatternsAndContentTypeTestCases[]{
    {{"https://google.com", "cookies"}, {true, ""}},
    {{";", "cookies"}, {false, "Not a valid web address"}},
    {{"*", "cookies"}, {false, "Not a valid web address"}},
    {{"http://google.com", "location"}, {false, "Origin must be secure"}},
    {{"http://127.0.0.1", "location"}, {true, ""}},  // Localhost is secure.
    {{"http://[::1]", "location"}, {true, ""}}};

#if BUILDFLAG(ENABLE_PLUGINS)
// Waits until a change is observed in content settings.
class FlashContentSettingsChangeWaiter : public content_settings::Observer {
 public:
  explicit FlashContentSettingsChangeWaiter(Profile* profile)
      : profile_(profile) {
    HostContentSettingsMapFactory::GetForProfile(profile)->AddObserver(this);
  }
  ~FlashContentSettingsChangeWaiter() override {
    HostContentSettingsMapFactory::GetForProfile(profile_)->RemoveObserver(
        this);
  }

  // content_settings::Observer:
  void OnContentSettingChanged(
      const ContentSettingsPattern& primary_pattern,
      const ContentSettingsPattern& secondary_pattern,
      ContentSettingsType content_type,
      const std::string& resource_identifier) override {
    if (content_type == CONTENT_SETTINGS_TYPE_PLUGINS)
      Proceed();
  }

  void Wait() { run_loop_.Run(); }

 private:
  void Proceed() { run_loop_.Quit(); }

  Profile* profile_;
  base::RunLoop run_loop_;

  DISALLOW_COPY_AND_ASSIGN(FlashContentSettingsChangeWaiter);
};
#endif

}  // namespace

namespace settings {

// Helper class for setting ContentSettings via different sources.
class ContentSettingSourceSetter {
 public:
  ContentSettingSourceSetter(TestingProfile* profile,
                             ContentSettingsType content_type)
      : prefs_(profile->GetTestingPrefService()),
        host_content_settings_map_(
            HostContentSettingsMapFactory::GetForProfile(profile)),
        content_type_(content_type) {}

  void SetPolicyDefault(ContentSetting setting) {
    prefs_->SetManagedPref(GetPrefNameForDefaultPermissionSetting(),
                           std::make_unique<base::Value>(setting));
  }

  const char* GetPrefNameForDefaultPermissionSetting() {
    switch (content_type_) {
      case CONTENT_SETTINGS_TYPE_NOTIFICATIONS:
        return prefs::kManagedDefaultNotificationsSetting;
      default:
        // Add support as needed.
        NOTREACHED();
        return "";
    }
  }

 private:
  sync_preferences::TestingPrefServiceSyncable* prefs_;
  HostContentSettingsMap* host_content_settings_map_;
  ContentSettingsType content_type_;

  DISALLOW_COPY_AND_ASSIGN(ContentSettingSourceSetter);
};

class SiteSettingsHandlerTest : public testing::Test {
 public:
  SiteSettingsHandlerTest()
      : kNotifications(site_settings::ContentSettingsTypeToGroupName(
            CONTENT_SETTINGS_TYPE_NOTIFICATIONS)),
        kCookies(site_settings::ContentSettingsTypeToGroupName(
            CONTENT_SETTINGS_TYPE_COOKIES)),
        kFlash(site_settings::ContentSettingsTypeToGroupName(
            CONTENT_SETTINGS_TYPE_PLUGINS)),
        handler_(&profile_) {
#if defined(OS_CHROMEOS)
    user_manager_enabler_ = std::make_unique<user_manager::ScopedUserManager>(
        std::make_unique<chromeos::MockUserManager>());
#endif
  }

  void SetUp() override {
    handler()->set_web_ui(web_ui());
    handler()->AllowJavascript();
    web_ui()->ClearTrackedCalls();
  }

  TestingProfile* profile() { return &profile_; }
  TestingProfile* incognito_profile() { return incognito_profile_; }
  content::TestWebUI* web_ui() { return &web_ui_; }
  SiteSettingsHandler* handler() { return &handler_; }

  void ValidateBlockAutoplay(bool expected_value, bool expected_enabled) {
    const content::TestWebUI::CallData& data = *web_ui()->call_data().back();
    EXPECT_EQ("cr.webUIListenerCallback", data.function_name());

    std::string event_name;
    ASSERT_TRUE(data.arg1()->GetAsString(&event_name));
    EXPECT_EQ("onBlockAutoplayStatusChanged", event_name);

    const base::DictionaryValue* event_data = nullptr;
    ASSERT_TRUE(data.arg2()->GetAsDictionary(&event_data));

    bool enabled;
    ASSERT_TRUE(event_data->GetBoolean("enabled", &enabled));
    EXPECT_EQ(expected_enabled, enabled);

    const base::DictionaryValue* pref_data = nullptr;
    ASSERT_TRUE(event_data->GetDictionary("pref", &pref_data));

    bool value;
    ASSERT_TRUE(pref_data->GetBoolean("value", &value));
    EXPECT_EQ(expected_value, value);
  }

  void SetSoundContentSettingDefault(ContentSetting value) {
    HostContentSettingsMap* content_settings =
        HostContentSettingsMapFactory::GetForProfile(profile());
    content_settings->SetDefaultContentSetting(CONTENT_SETTINGS_TYPE_SOUND,
                                               value);
  }

  void ValidateDefault(const ContentSetting expected_setting,
                       const site_settings::SiteSettingSource expected_source,
                       size_t expected_total_calls) {
    EXPECT_EQ(expected_total_calls, web_ui()->call_data().size());

    const content::TestWebUI::CallData& data = *web_ui()->call_data().back();
    EXPECT_EQ("cr.webUIResponse", data.function_name());

    std::string callback_id;
    ASSERT_TRUE(data.arg1()->GetAsString(&callback_id));
    EXPECT_EQ(kCallbackId, callback_id);

    bool success = false;
    ASSERT_TRUE(data.arg2()->GetAsBoolean(&success));
    ASSERT_TRUE(success);

    const base::DictionaryValue* default_value = nullptr;
    ASSERT_TRUE(data.arg3()->GetAsDictionary(&default_value));
    std::string setting;
    ASSERT_TRUE(default_value->GetString(kSetting, &setting));
    EXPECT_EQ(content_settings::ContentSettingToString(expected_setting),
              setting);
    std::string source;
    if (default_value->GetString(kSource, &source))
      EXPECT_EQ(site_settings::SiteSettingSourceToString(expected_source),
                source);
  }

  void ValidateOrigin(const std::string& expected_origin,
                      const std::string& expected_embedding,
                      const std::string& expected_display_name,
                      const ContentSetting expected_setting,
                      const site_settings::SiteSettingSource expected_source,
                      size_t expected_total_calls) {
    EXPECT_EQ(expected_total_calls, web_ui()->call_data().size());

    const content::TestWebUI::CallData& data = *web_ui()->call_data().back();
    EXPECT_EQ("cr.webUIResponse", data.function_name());

    std::string callback_id;
    ASSERT_TRUE(data.arg1()->GetAsString(&callback_id));
    EXPECT_EQ(kCallbackId, callback_id);
    bool success = false;
    ASSERT_TRUE(data.arg2()->GetAsBoolean(&success));
    ASSERT_TRUE(success);

    const base::ListValue* exceptions;
    ASSERT_TRUE(data.arg3()->GetAsList(&exceptions));
    EXPECT_EQ(1U, exceptions->GetSize());
    const base::DictionaryValue* exception;
    ASSERT_TRUE(exceptions->GetDictionary(0, &exception));
    std::string origin, embedding_origin, display_name, setting, source;
    ASSERT_TRUE(exception->GetString(site_settings::kOrigin, &origin));
    ASSERT_EQ(expected_origin, origin);
    ASSERT_TRUE(
        exception->GetString(site_settings::kDisplayName, &display_name));
    ASSERT_EQ(expected_display_name, display_name);
    ASSERT_TRUE(exception->GetString(
        site_settings::kEmbeddingOrigin, &embedding_origin));
    ASSERT_EQ(expected_embedding, embedding_origin);
    ASSERT_TRUE(exception->GetString(site_settings::kSetting, &setting));
    ASSERT_EQ(content_settings::ContentSettingToString(expected_setting),
              setting);
    ASSERT_TRUE(exception->GetString(site_settings::kSource, &source));
    ASSERT_EQ(site_settings::SiteSettingSourceToString(expected_source),
              source);
  }

  void ValidateNoOrigin(size_t expected_total_calls) {
    EXPECT_EQ(expected_total_calls, web_ui()->call_data().size());

    const content::TestWebUI::CallData& data = *web_ui()->call_data().back();
    EXPECT_EQ("cr.webUIResponse", data.function_name());

    std::string callback_id;
    ASSERT_TRUE(data.arg1()->GetAsString(&callback_id));
    EXPECT_EQ(kCallbackId, callback_id);

    bool success = false;
    ASSERT_TRUE(data.arg2()->GetAsBoolean(&success));
    ASSERT_TRUE(success);

    const base::ListValue* exceptions;
    ASSERT_TRUE(data.arg3()->GetAsList(&exceptions));
    EXPECT_EQ(0U, exceptions->GetSize());
  }

  void ValidatePattern(bool expected_validity,
                       size_t expected_total_calls,
                       std::string expected_reason) {
    EXPECT_EQ(expected_total_calls, web_ui()->call_data().size());

    const content::TestWebUI::CallData& data = *web_ui()->call_data().back();
    EXPECT_EQ("cr.webUIResponse", data.function_name());

    std::string callback_id;
    ASSERT_TRUE(data.arg1()->GetAsString(&callback_id));
    EXPECT_EQ(kCallbackId, callback_id);

    bool success = false;
    ASSERT_TRUE(data.arg2()->GetAsBoolean(&success));
    ASSERT_TRUE(success);

    const base::DictionaryValue* result = nullptr;
    ASSERT_TRUE(data.arg3()->GetAsDictionary(&result));

    bool valid = false;
    ASSERT_TRUE(result->GetBoolean("isValid", &valid));
    EXPECT_EQ(expected_validity, valid);

    std::string reason;
    ASSERT_TRUE(result->GetString("reason", &reason));
    EXPECT_EQ(expected_reason, reason);
  }

  void ValidateIncognitoExists(
      bool expected_incognito, size_t expected_total_calls) {
    EXPECT_EQ(expected_total_calls, web_ui()->call_data().size());

    const content::TestWebUI::CallData& data = *web_ui()->call_data().back();
    EXPECT_EQ("cr.webUIListenerCallback", data.function_name());

    std::string callback_id;
    ASSERT_TRUE(data.arg1()->GetAsString(&callback_id));
    EXPECT_EQ("onIncognitoStatusChanged", callback_id);

    bool incognito;
    ASSERT_TRUE(data.arg2()->GetAsBoolean(&incognito));
    EXPECT_EQ(expected_incognito, incognito);
  }

  void ValidateZoom(const std::string& expected_host,
      const std::string& expected_zoom, size_t expected_total_calls) {
    EXPECT_EQ(expected_total_calls, web_ui()->call_data().size());

    const content::TestWebUI::CallData& data = *web_ui()->call_data().back();
    EXPECT_EQ("cr.webUIListenerCallback", data.function_name());

    std::string callback_id;
    ASSERT_TRUE(data.arg1()->GetAsString(&callback_id));
    EXPECT_EQ("onZoomLevelsChanged", callback_id);

    const base::ListValue* exceptions;
    ASSERT_TRUE(data.arg2()->GetAsList(&exceptions));
    if (expected_host.empty()) {
      EXPECT_EQ(0U, exceptions->GetSize());
    } else {
      EXPECT_EQ(1U, exceptions->GetSize());

      const base::DictionaryValue* exception;
      ASSERT_TRUE(exceptions->GetDictionary(0, &exception));

      std::string host;
      ASSERT_TRUE(exception->GetString("origin", &host));
      ASSERT_EQ(expected_host, host);

      std::string zoom;
      ASSERT_TRUE(exception->GetString("zoom", &zoom));
      ASSERT_EQ(expected_zoom, zoom);
    }
  }

  void CreateIncognitoProfile() {
    incognito_profile_ = TestingProfile::Builder().BuildIncognito(&profile_);
  }

  virtual void DestroyIncognitoProfile() {
    content::NotificationService::current()->Notify(
        chrome::NOTIFICATION_PROFILE_DESTROYED,
        content::Source<Profile>(static_cast<Profile*>(incognito_profile_)),
        content::NotificationService::NoDetails());
    profile_.SetOffTheRecordProfile(nullptr);
    ASSERT_FALSE(profile_.HasOffTheRecordProfile());
    incognito_profile_ = nullptr;
  }

  // TODO(https://crbug.com/835712): Currently only set up the cookies and local
  // storage nodes, will update all other nodes in the future.
  void SetUpCookiesTreeModel() {
    scoped_refptr<MockBrowsingDataCookieHelper>
        mock_browsing_data_cookie_helper;
    scoped_refptr<MockBrowsingDataLocalStorageHelper>
        mock_browsing_data_local_storage_helper;

    mock_browsing_data_cookie_helper =
        new MockBrowsingDataCookieHelper(profile());
    mock_browsing_data_local_storage_helper =
        new MockBrowsingDataLocalStorageHelper(profile());

    auto container = std::make_unique<LocalDataContainer>(
        mock_browsing_data_cookie_helper,
        /*database_helper=*/nullptr, mock_browsing_data_local_storage_helper,
        /*session_storage_helper=*/nullptr,
        /*appcache_helper=*/nullptr,
        /*indexed_db_helper=*/nullptr,
        /*file_system_helper=*/nullptr,
        /*quota_helper=*/nullptr,
        /*service_worker_helper=*/nullptr,
        /*data_shared_worker_helper=*/nullptr,
        /*cache_storage_helper=*/nullptr,
        /*flash_lso_helper=*/nullptr,
        /*media_license_helper=*/nullptr);
    auto mock_cookies_tree_model = std::make_unique<CookiesTreeModel>(
        std::move(container), profile()->GetExtensionSpecialStoragePolicy());

    mock_browsing_data_local_storage_helper->AddLocalStorageForOrigin(
        url::Origin::Create(GURL("https://www.example.com/")), 2);

    mock_browsing_data_local_storage_helper->AddLocalStorageForOrigin(
        url::Origin::Create(GURL("https://www.google.com/")), 5);
    mock_browsing_data_local_storage_helper->Notify();

    mock_browsing_data_cookie_helper->AddCookieSamples(
        GURL("http://example.com"), "A=1");
    mock_browsing_data_cookie_helper->AddCookieSamples(
        GURL("http://www.example.com/"), "B=1");
    mock_browsing_data_cookie_helper->AddCookieSamples(
        GURL("http://abc.example.com"), "C=1");
    mock_browsing_data_cookie_helper->AddCookieSamples(
        GURL("http://google.com"), "A=1");
    mock_browsing_data_cookie_helper->AddCookieSamples(
        GURL("http://google.com"), "B=1");
    mock_browsing_data_cookie_helper->AddCookieSamples(
        GURL("http://google.com.au"), "A=1");
    mock_browsing_data_cookie_helper->Notify();

    handler()->SetCookiesTreeModelForTesting(
        std::move(mock_cookies_tree_model));
  }

  const base::ListValue* GetOnStorageFetchedSentListValue() {
    handler()->ClearAllSitesMapForTesting();
    handler()->OnStorageFetched();
    const content::TestWebUI::CallData& data = *web_ui()->call_data().back();
    std::string callback_id;
    data.arg1()->GetAsString(&callback_id);
    const base::ListValue* storage_and_cookie_list;
    data.arg2()->GetAsList(&storage_and_cookie_list);
    return storage_and_cookie_list;
  }

  // Content setting group name for the relevant ContentSettingsType.
  const std::string kNotifications;
  const std::string kCookies;
  const std::string kFlash;

 private:
  content::TestBrowserThreadBundle thread_bundle_;
  TestingProfile profile_;
  TestingProfile* incognito_profile_;
  content::TestWebUI web_ui_;
  SiteSettingsHandler handler_;
#if defined(OS_CHROMEOS)
  std::unique_ptr<user_manager::ScopedUserManager> user_manager_enabler_;
#endif
};

TEST_F(SiteSettingsHandlerTest, GetAndSetDefault) {
  // Test the JS -> C++ -> JS callback path for getting and setting defaults.
  base::ListValue get_args;
  get_args.AppendString(kCallbackId);
  get_args.AppendString(kNotifications);
  handler()->HandleGetDefaultValueForContentType(&get_args);
  ValidateDefault(CONTENT_SETTING_ASK,
                  site_settings::SiteSettingSource::kDefault, 1U);

  // Set the default to 'Blocked'.
  base::ListValue set_args;
  set_args.AppendString(kNotifications);
  set_args.AppendString(
      content_settings::ContentSettingToString(CONTENT_SETTING_BLOCK));
  handler()->HandleSetDefaultValueForContentType(&set_args);

  EXPECT_EQ(2U, web_ui()->call_data().size());

  // Verify that the default has been set to 'Blocked'.
  handler()->HandleGetDefaultValueForContentType(&get_args);
  ValidateDefault(CONTENT_SETTING_BLOCK,
                  site_settings::SiteSettingSource::kDefault, 3U);
}

// Flaky on CrOS and Linux. https://crbug.com/930481
#if defined(OS_CHROMEOS) || defined(OS_LINUX)
#define MAYBE_GetAllSites DISABLED_GetAllSites
#else
#define MAYBE_GetAllSites GetAllSites
#endif
TEST_F(SiteSettingsHandlerTest, MAYBE_GetAllSites) {
  base::ListValue get_all_sites_args;
  get_all_sites_args.AppendString(kCallbackId);
  base::Value category_list(base::Value::Type::LIST);
  category_list.GetList().emplace_back(kNotifications);
  category_list.GetList().emplace_back(kFlash);
  get_all_sites_args.GetList().push_back(std::move(category_list));

  // Test all sites is empty when there are no preferences.
  handler()->HandleGetAllSites(&get_all_sites_args);
  EXPECT_EQ(1U, web_ui()->call_data().size());

  {
    const content::TestWebUI::CallData& data = *web_ui()->call_data().back();
    EXPECT_EQ("cr.webUIResponse", data.function_name());
    EXPECT_EQ(kCallbackId, data.arg1()->GetString());
    ASSERT_TRUE(data.arg2()->GetBool());

    const base::Value::ListStorage& site_groups = data.arg3()->GetList();
    EXPECT_EQ(0UL, site_groups.size());
  }

  // Add a couple of exceptions and check they appear in all sites.
  HostContentSettingsMap* map =
      HostContentSettingsMapFactory::GetForProfile(profile());
  const GURL url1("http://example.com");
  const GURL url2("https://other.example.com");
  map->SetContentSettingDefaultScope(url1, url1,
                                     CONTENT_SETTINGS_TYPE_NOTIFICATIONS,
                                     std::string(), CONTENT_SETTING_BLOCK);
  map->SetContentSettingDefaultScope(url2, url2, CONTENT_SETTINGS_TYPE_PLUGINS,
                                     std::string(), CONTENT_SETTING_ALLOW);
  handler()->HandleGetAllSites(&get_all_sites_args);

  {
    const content::TestWebUI::CallData& data = *web_ui()->call_data().back();
    EXPECT_EQ("cr.webUIResponse", data.function_name());
    EXPECT_EQ(kCallbackId, data.arg1()->GetString());
    ASSERT_TRUE(data.arg2()->GetBool());

    const base::Value::ListStorage& site_groups = data.arg3()->GetList();
    EXPECT_EQ(1UL, site_groups.size());
    for (const base::Value& site_group : site_groups) {
      const std::string& etld_plus1_string =
          site_group.FindKey("etldPlus1")->GetString();
      const base::Value::ListStorage& origin_list =
          site_group.FindKey("origins")->GetList();
      EXPECT_EQ("example.com", etld_plus1_string);
      EXPECT_EQ(2UL, origin_list.size());
      EXPECT_EQ(url1.spec(), origin_list[0].FindKey("origin")->GetString());
      EXPECT_EQ(0, origin_list[0].FindKey("engagement")->GetDouble());
      EXPECT_EQ(url2.spec(), origin_list[1].FindKey("origin")->GetString());
      EXPECT_EQ(0, origin_list[1].FindKey("engagement")->GetDouble());
    }
  }

  // Add an additional exception belonging to a different eTLD+1.
  const GURL url3("https://example2.net");
  map->SetContentSettingDefaultScope(url3, url3, CONTENT_SETTINGS_TYPE_PLUGINS,
                                     std::string(), CONTENT_SETTING_BLOCK);
  handler()->HandleGetAllSites(&get_all_sites_args);

  {
    const content::TestWebUI::CallData& data = *web_ui()->call_data().back();
    EXPECT_EQ("cr.webUIResponse", data.function_name());

    EXPECT_EQ(kCallbackId, data.arg1()->GetString());
    ASSERT_TRUE(data.arg2()->GetBool());

    const base::Value::ListStorage& site_groups = data.arg3()->GetList();
    EXPECT_EQ(2UL, site_groups.size());
    for (const base::Value& site_group : site_groups) {
      const std::string& etld_plus1_string =
          site_group.FindKey("etldPlus1")->GetString();
      const base::Value::ListStorage& origin_list =
          site_group.FindKey("origins")->GetList();
      if (etld_plus1_string == "example2.net") {
        EXPECT_EQ(1UL, origin_list.size());
        EXPECT_EQ(url3.spec(), origin_list[0].FindKey("origin")->GetString());
      } else {
        EXPECT_EQ("example.com", etld_plus1_string);
      }
    }
  }

  // Test embargoed settings also appear.
  PermissionDecisionAutoBlocker* auto_blocker =
      PermissionDecisionAutoBlocker::GetForProfile(profile());
  base::SimpleTestClock clock;
  clock.SetNow(base::Time::Now());
  auto_blocker->SetClockForTesting(&clock);
  const GURL url4("https://example2.co.uk");
  for (int i = 0; i < 3; ++i) {
    auto_blocker->RecordDismissAndEmbargo(url4,
                                          CONTENT_SETTINGS_TYPE_NOTIFICATIONS);
  }
  EXPECT_EQ(
      CONTENT_SETTING_BLOCK,
      auto_blocker->GetEmbargoResult(url4, CONTENT_SETTINGS_TYPE_NOTIFICATIONS)
          .content_setting);
  handler()->HandleGetAllSites(&get_all_sites_args);

  {
    const content::TestWebUI::CallData& data = *web_ui()->call_data().back();
    EXPECT_EQ("cr.webUIResponse", data.function_name());
    EXPECT_EQ(kCallbackId, data.arg1()->GetString());
    ASSERT_TRUE(data.arg2()->GetBool());

    const base::Value::ListStorage& site_groups = data.arg3()->GetList();
    EXPECT_EQ(3UL, site_groups.size());
  }

  // Check |url4| disappears from the list when its embargo expires.
  clock.Advance(base::TimeDelta::FromDays(8));
  handler()->HandleGetAllSites(&get_all_sites_args);

  {
    const content::TestWebUI::CallData& data = *web_ui()->call_data().back();
    EXPECT_EQ("cr.webUIResponse", data.function_name());
    EXPECT_EQ(kCallbackId, data.arg1()->GetString());
    ASSERT_TRUE(data.arg2()->GetBool());

    const base::Value::ListStorage& site_groups = data.arg3()->GetList();
    EXPECT_EQ(2UL, site_groups.size());
    EXPECT_EQ("example.com", site_groups[0].FindKey("etldPlus1")->GetString());
    EXPECT_EQ("example2.net", site_groups[1].FindKey("etldPlus1")->GetString());
  }

  // Add an expired embargo setting to an existing eTLD+1 group and make sure it
  // still appears.
  for (int i = 0; i < 3; ++i) {
    auto_blocker->RecordDismissAndEmbargo(url3,
                                          CONTENT_SETTINGS_TYPE_NOTIFICATIONS);
  }
  EXPECT_EQ(
      CONTENT_SETTING_BLOCK,
      auto_blocker->GetEmbargoResult(url3, CONTENT_SETTINGS_TYPE_NOTIFICATIONS)
          .content_setting);
  clock.Advance(base::TimeDelta::FromDays(8));
  EXPECT_EQ(
      CONTENT_SETTING_ASK,
      auto_blocker->GetEmbargoResult(url3, CONTENT_SETTINGS_TYPE_NOTIFICATIONS)
          .content_setting);

  handler()->HandleGetAllSites(&get_all_sites_args);
  {
    const content::TestWebUI::CallData& data = *web_ui()->call_data().back();
    EXPECT_EQ("cr.webUIResponse", data.function_name());
    EXPECT_EQ(kCallbackId, data.arg1()->GetString());
    ASSERT_TRUE(data.arg2()->GetBool());

    const base::Value::ListStorage& site_groups = data.arg3()->GetList();
    EXPECT_EQ(2UL, site_groups.size());
    EXPECT_EQ("example.com", site_groups[0].FindKey("etldPlus1")->GetString());
    EXPECT_EQ("example2.net", site_groups[1].FindKey("etldPlus1")->GetString());
  }

  // Add an expired embargo to a new eTLD+1 and make sure it doesn't appear.
  const GURL url5("http://test.example5.com");
  for (int i = 0; i < 3; ++i) {
    auto_blocker->RecordDismissAndEmbargo(url5,
                                          CONTENT_SETTINGS_TYPE_NOTIFICATIONS);
  }
  EXPECT_EQ(
      CONTENT_SETTING_BLOCK,
      auto_blocker->GetEmbargoResult(url5, CONTENT_SETTINGS_TYPE_NOTIFICATIONS)
          .content_setting);
  clock.Advance(base::TimeDelta::FromDays(8));
  EXPECT_EQ(
      CONTENT_SETTING_ASK,
      auto_blocker->GetEmbargoResult(url5, CONTENT_SETTINGS_TYPE_NOTIFICATIONS)
          .content_setting);

  handler()->HandleGetAllSites(&get_all_sites_args);
  {
    const content::TestWebUI::CallData& data = *web_ui()->call_data().back();
    EXPECT_EQ("cr.webUIResponse", data.function_name());
    EXPECT_EQ(kCallbackId, data.arg1()->GetString());
    ASSERT_TRUE(data.arg2()->GetBool());

    const base::Value::ListStorage& site_groups = data.arg3()->GetList();
    EXPECT_EQ(2UL, site_groups.size());
    EXPECT_EQ("example.com", site_groups[0].FindKey("etldPlus1")->GetString());
    EXPECT_EQ("example2.net", site_groups[1].FindKey("etldPlus1")->GetString());
  }

  // Each call to HandleGetAllSites() above added a callback to the profile's
  // BrowsingDataLocalStorageHelper, so make sure these aren't stuck waiting to
  // run at the end of the test.
  base::RunLoop run_loop;
  run_loop.RunUntilIdle();
}

TEST_F(SiteSettingsHandlerTest, OnStorageFetched) {
  SetUpCookiesTreeModel();

  handler()->ClearAllSitesMapForTesting();

  handler()->OnStorageFetched();
  const content::TestWebUI::CallData& data = *web_ui()->call_data().back();
  EXPECT_EQ("cr.webUIListenerCallback", data.function_name());

  std::string callback_id;
  ASSERT_TRUE(data.arg1()->GetAsString(&callback_id));
  EXPECT_EQ("onStorageListFetched", callback_id);

  const base::ListValue* storage_and_cookie_list;
  ASSERT_TRUE(data.arg2()->GetAsList(&storage_and_cookie_list));
  EXPECT_EQ(3U, storage_and_cookie_list->GetSize());

  const base::DictionaryValue* site_group;
  ASSERT_TRUE(storage_and_cookie_list->GetDictionary(0, &site_group));

  std::string etld_plus1_string;
  ASSERT_TRUE(site_group->GetString("etldPlus1", &etld_plus1_string));
  ASSERT_EQ("example.com", etld_plus1_string);

  EXPECT_EQ(3, site_group->FindKey("numCookies")->GetDouble());

  const base::ListValue* origin_list;
  ASSERT_TRUE(site_group->GetList("origins", &origin_list));
  // There will be 2 origins in this case. Cookie node with url
  // http://www.example.com/ will be treat as https://www.example.com/ because
  // this url existed in the storage nodes.
  EXPECT_EQ(2U, origin_list->GetSize());

  const base::DictionaryValue* origin_info;

  ASSERT_TRUE(origin_list->GetDictionary(0, &origin_info));
  EXPECT_EQ("http://abc.example.com/",
            origin_info->FindKey("origin")->GetString());
  EXPECT_EQ(0, origin_info->FindKey("engagement")->GetDouble());
  EXPECT_EQ(0, origin_info->FindKey("usage")->GetDouble());
  EXPECT_EQ(1, origin_info->FindKey("numCookies")->GetDouble());

  ASSERT_TRUE(origin_list->GetDictionary(1, &origin_info));
  // Even though in the cookies the scheme is http, it still stored as https
  // because there is https data stored.
  EXPECT_EQ("https://www.example.com/",
            origin_info->FindKey("origin")->GetString());
  EXPECT_EQ(0, origin_info->FindKey("engagement")->GetDouble());
  EXPECT_EQ(2, origin_info->FindKey("usage")->GetDouble());
  EXPECT_EQ(1, origin_info->FindKey("numCookies")->GetDouble());

  ASSERT_TRUE(storage_and_cookie_list->GetDictionary(1, &site_group));

  ASSERT_TRUE(site_group->GetString("etldPlus1", &etld_plus1_string));
  ASSERT_EQ("google.com", etld_plus1_string);

  EXPECT_EQ(2, site_group->FindKey("numCookies")->GetDouble());

  ASSERT_TRUE(site_group->GetList("origins", &origin_list));

  EXPECT_EQ(1U, origin_list->GetSize());

  ASSERT_TRUE(origin_list->GetDictionary(0, &origin_info));
  EXPECT_EQ("https://www.google.com/",
            origin_info->FindKey("origin")->GetString());
  EXPECT_EQ(0, origin_info->FindKey("engagement")->GetDouble());
  EXPECT_EQ(5, origin_info->FindKey("usage")->GetDouble());
  EXPECT_EQ(0, origin_info->FindKey("numCookies")->GetDouble());

  ASSERT_TRUE(storage_and_cookie_list->GetDictionary(2, &site_group));

  ASSERT_TRUE(site_group->GetString("etldPlus1", &etld_plus1_string));
  ASSERT_EQ("google.com.au", etld_plus1_string);

  EXPECT_EQ(1, site_group->FindKey("numCookies")->GetDouble());

  ASSERT_TRUE(site_group->GetList("origins", &origin_list));
  EXPECT_EQ(1U, origin_list->GetSize());

  ASSERT_TRUE(origin_list->GetDictionary(0, &origin_info));
  EXPECT_EQ("http://google.com.au/",
            origin_info->FindKey("origin")->GetString());
  EXPECT_EQ(0, origin_info->FindKey("engagement")->GetDouble());
  EXPECT_EQ(0, origin_info->FindKey("usage")->GetDouble());
  EXPECT_EQ(0, origin_info->FindKey("numCookies")->GetDouble());
}

TEST_F(SiteSettingsHandlerTest, Origins) {
  const std::string google("https://www.google.com:443");
  const std::string uma_base("WebsiteSettings.Menu.PermissionChanged");
  {
    // Test the JS -> C++ -> JS callback path for configuring origins, by
    // setting Google.com to blocked.
    base::ListValue set_args;
    set_args.AppendString(google);  // Primary pattern.
    set_args.AppendString(google);  // Secondary pattern.
    set_args.AppendString(kNotifications);
    set_args.AppendString(
        content_settings::ContentSettingToString(CONTENT_SETTING_BLOCK));
    set_args.AppendBoolean(false);  // Incognito.
    base::HistogramTester histograms;
    handler()->HandleSetCategoryPermissionForPattern(&set_args);
    EXPECT_EQ(1U, web_ui()->call_data().size());
    histograms.ExpectTotalCount(uma_base, 1);
    histograms.ExpectTotalCount(uma_base + ".Allowed", 0);
    histograms.ExpectTotalCount(uma_base + ".Blocked", 1);
    histograms.ExpectTotalCount(uma_base + ".Reset", 0);
    histograms.ExpectTotalCount(uma_base + ".SessionOnly", 0);
  }

  base::ListValue get_exception_list_args;
  get_exception_list_args.AppendString(kCallbackId);
  get_exception_list_args.AppendString(kNotifications);
  handler()->HandleGetExceptionList(&get_exception_list_args);
  ValidateOrigin(google, google, google, CONTENT_SETTING_BLOCK,
                 site_settings::SiteSettingSource::kPreference, 2U);

  {
    // Reset things back to how they were.
    base::ListValue reset_args;
    reset_args.AppendString(google);
    reset_args.AppendString(google);
    reset_args.AppendString(kNotifications);
    reset_args.AppendBoolean(false);  // Incognito.
    base::HistogramTester histograms;
    handler()->HandleResetCategoryPermissionForPattern(&reset_args);
    EXPECT_EQ(3U, web_ui()->call_data().size());
    histograms.ExpectTotalCount(uma_base, 1);
    histograms.ExpectTotalCount(uma_base + ".Allowed", 0);
    histograms.ExpectTotalCount(uma_base + ".Blocked", 0);
    histograms.ExpectTotalCount(uma_base + ".Reset", 1);
  }

  // Verify the reset was successful.
  handler()->HandleGetExceptionList(&get_exception_list_args);
  ValidateNoOrigin(4U);
}

TEST_F(SiteSettingsHandlerTest, DefaultSettingSource) {
  // Use a non-default port to verify the display name does not strip this off.
  const std::string google("https://www.google.com:183");
  ContentSettingSourceSetter source_setter(profile(),
                                           CONTENT_SETTINGS_TYPE_NOTIFICATIONS);

  base::ListValue get_origin_permissions_args;
  get_origin_permissions_args.AppendString(kCallbackId);
  get_origin_permissions_args.AppendString(google);
  auto category_list = std::make_unique<base::ListValue>();
  category_list->AppendString(kNotifications);
  get_origin_permissions_args.Append(std::move(category_list));

  // Test Chrome built-in defaults are marked as default.
  handler()->HandleGetOriginPermissions(&get_origin_permissions_args);
  ValidateOrigin(google, google, google, CONTENT_SETTING_ASK,
                 site_settings::SiteSettingSource::kDefault, 1U);

  base::ListValue default_value_args;
  default_value_args.AppendString(kNotifications);
  default_value_args.AppendString(
      content_settings::ContentSettingToString(CONTENT_SETTING_BLOCK));
  handler()->HandleSetDefaultValueForContentType(&default_value_args);
  // A user-set global default should also show up as default.
  handler()->HandleGetOriginPermissions(&get_origin_permissions_args);
  ValidateOrigin(google, google, google, CONTENT_SETTING_BLOCK,
                 site_settings::SiteSettingSource::kDefault, 3U);

  base::ListValue set_notification_pattern_args;
  set_notification_pattern_args.AppendString("[*.]google.com");
  set_notification_pattern_args.AppendString("*");
  set_notification_pattern_args.AppendString(kNotifications);
  set_notification_pattern_args.AppendString(
      content_settings::ContentSettingToString(CONTENT_SETTING_ALLOW));
  set_notification_pattern_args.AppendBoolean(false);
  handler()->HandleSetCategoryPermissionForPattern(
      &set_notification_pattern_args);
  // A user-set pattern should not show up as default.
  handler()->HandleGetOriginPermissions(&get_origin_permissions_args);
  ValidateOrigin(google, google, google, CONTENT_SETTING_ALLOW,
                 site_settings::SiteSettingSource::kPreference, 5U);

  base::ListValue set_notification_origin_args;
  set_notification_origin_args.AppendString(google);
  set_notification_origin_args.AppendString(google);
  set_notification_origin_args.AppendString(kNotifications);
  set_notification_origin_args.AppendString(
      content_settings::ContentSettingToString(CONTENT_SETTING_BLOCK));
  set_notification_origin_args.AppendBoolean(false);
  handler()->HandleSetCategoryPermissionForPattern(
      &set_notification_origin_args);
  // A user-set per-origin permission should not show up as default.
  handler()->HandleGetOriginPermissions(&get_origin_permissions_args);
  ValidateOrigin(google, google, google, CONTENT_SETTING_BLOCK,
                 site_settings::SiteSettingSource::kPreference, 7U);

  // Enterprise-policy set defaults should not show up as default.
  source_setter.SetPolicyDefault(CONTENT_SETTING_ALLOW);
  handler()->HandleGetOriginPermissions(&get_origin_permissions_args);
  ValidateOrigin(google, google, google, CONTENT_SETTING_ALLOW,
                 site_settings::SiteSettingSource::kPolicy, 8U);
}

TEST_F(SiteSettingsHandlerTest, GetAndSetOriginPermissions) {
  const std::string origin_with_port("https://www.example.com:443");
  // The display name won't show the port if it's default for that scheme.
  const std::string origin("https://www.example.com");
  base::ListValue get_args;
  get_args.AppendString(kCallbackId);
  get_args.AppendString(origin_with_port);
  {
    auto category_list = std::make_unique<base::ListValue>();
    category_list->AppendString(kNotifications);
    get_args.Append(std::move(category_list));
  }
  handler()->HandleGetOriginPermissions(&get_args);
  ValidateOrigin(origin_with_port, origin_with_port, origin,
                 CONTENT_SETTING_ASK,
                 site_settings::SiteSettingSource::kDefault, 1U);

  // Block notifications.
  base::ListValue set_args;
  set_args.AppendString(origin_with_port);
  {
    auto category_list = std::make_unique<base::ListValue>();
    category_list->AppendString(kNotifications);
    set_args.Append(std::move(category_list));
  }
  set_args.AppendString(
      content_settings::ContentSettingToString(CONTENT_SETTING_BLOCK));
  handler()->HandleSetOriginPermissions(&set_args);
  EXPECT_EQ(2U, web_ui()->call_data().size());

  // Reset things back to how they were.
  base::ListValue reset_args;
  reset_args.AppendString(origin_with_port);
  auto category_list = std::make_unique<base::ListValue>();
  category_list->AppendString(kNotifications);
  reset_args.Append(std::move(category_list));
  reset_args.AppendString(
      content_settings::ContentSettingToString(CONTENT_SETTING_DEFAULT));

  handler()->HandleSetOriginPermissions(&reset_args);
  EXPECT_EQ(3U, web_ui()->call_data().size());

  // Verify the reset was successful.
  handler()->HandleGetOriginPermissions(&get_args);
  ValidateOrigin(origin_with_port, origin_with_port, origin,
                 CONTENT_SETTING_ASK,
                 site_settings::SiteSettingSource::kDefault, 4U);
}

#if BUILDFLAG(ENABLE_PLUGINS)
TEST_F(SiteSettingsHandlerTest, ChangingFlashSettingForSiteIsRemembered) {
  ChromePluginServiceFilter::GetInstance()->RegisterResourceContext(
      profile(), profile()->GetResourceContext());
  FlashContentSettingsChangeWaiter waiter(profile());

  const std::string origin_with_port("https://www.example.com:443");
  // The display name won't show the port if it's default for that scheme.
  const std::string origin("https://www.example.com");
  base::ListValue get_args;
  get_args.AppendString(kCallbackId);
  get_args.AppendString(origin_with_port);
  const GURL url(origin_with_port);

  HostContentSettingsMap* map =
      HostContentSettingsMapFactory::GetForProfile(profile());
  // Make sure the site being tested doesn't already have this marker set.
  EXPECT_EQ(nullptr,
            map->GetWebsiteSetting(url, url, CONTENT_SETTINGS_TYPE_PLUGINS_DATA,
                                   std::string(), nullptr));

  // Change the Flash setting.
  base::ListValue set_args;
  set_args.AppendString(origin_with_port);
  {
    auto category_list = std::make_unique<base::ListValue>();
    category_list->AppendString(kFlash);
    set_args.Append(std::move(category_list));
  }
  set_args.AppendString(
      content_settings::ContentSettingToString(CONTENT_SETTING_BLOCK));
  handler()->HandleSetOriginPermissions(&set_args);
  EXPECT_EQ(1U, web_ui()->call_data().size());
  waiter.Wait();

  // Check that this site has now been marked for displaying Flash always, then
  // clear it and check this works.
  EXPECT_NE(nullptr,
            map->GetWebsiteSetting(url, url, CONTENT_SETTINGS_TYPE_PLUGINS_DATA,
                                   std::string(), nullptr));
  base::ListValue clear_args;
  clear_args.AppendString(origin_with_port);
  handler()->HandleSetOriginPermissions(&set_args);
  handler()->HandleClearFlashPref(&clear_args);
  EXPECT_EQ(nullptr,
            map->GetWebsiteSetting(url, url, CONTENT_SETTINGS_TYPE_PLUGINS_DATA,
                                   std::string(), nullptr));
}
#endif

TEST_F(SiteSettingsHandlerTest, GetAndSetForInvalidURLs) {
  const std::string origin("arbitrary string");
  EXPECT_FALSE(GURL(origin).is_valid());
  base::ListValue get_args;
  get_args.AppendString(kCallbackId);
  get_args.AppendString(origin);
  {
    auto category_list = std::make_unique<base::ListValue>();
    category_list->AppendString(kNotifications);
    get_args.Append(std::move(category_list));
  }
  handler()->HandleGetOriginPermissions(&get_args);
  // Verify that it'll return CONTENT_SETTING_BLOCK as |origin| is not a secure
  // context, a requirement for notifications. Note that the display string
  // will be blank since it's an invalid URL.
  ValidateOrigin(origin, origin, "", CONTENT_SETTING_BLOCK,
                 site_settings::SiteSettingSource::kInsecureOrigin, 1U);

  // Make sure setting a permission on an invalid origin doesn't crash.
  base::ListValue set_args;
  set_args.AppendString(origin);
  {
    auto category_list = std::make_unique<base::ListValue>();
    category_list->AppendString(kNotifications);
    set_args.Append(std::move(category_list));
  }
  set_args.AppendString(
      content_settings::ContentSettingToString(CONTENT_SETTING_ALLOW));
  handler()->HandleSetOriginPermissions(&set_args);

  // Also make sure the content setting for |origin| wasn't actually changed.
  handler()->HandleGetOriginPermissions(&get_args);
  ValidateOrigin(origin, origin, "", CONTENT_SETTING_BLOCK,
                 site_settings::SiteSettingSource::kInsecureOrigin, 2U);
}

TEST_F(SiteSettingsHandlerTest, ExceptionHelpers) {
  ContentSettingsPattern pattern =
      ContentSettingsPattern::FromString("[*.]google.com");
  std::unique_ptr<base::DictionaryValue> exception =
      site_settings::GetExceptionForPage(
          pattern, pattern, pattern.ToString(), CONTENT_SETTING_BLOCK,
          site_settings::SiteSettingSourceToString(
              site_settings::SiteSettingSource::kPreference),
          false);

  std::string primary_pattern, secondary_pattern, display_name, type;
  bool incognito;
  CHECK(exception->GetString(site_settings::kOrigin, &primary_pattern));
  CHECK(exception->GetString(site_settings::kDisplayName, &display_name));
  CHECK(exception->GetString(site_settings::kEmbeddingOrigin,
                             &secondary_pattern));
  CHECK(exception->GetString(site_settings::kSetting, &type));
  CHECK(exception->GetBoolean(site_settings::kIncognito, &incognito));

  base::ListValue args;
  args.AppendString(primary_pattern);
  args.AppendString(secondary_pattern);
  args.AppendString(kNotifications);  // Chosen arbitrarily.
  args.AppendString(type);
  args.AppendBoolean(incognito);

  // We don't need to check the results. This is just to make sure it doesn't
  // crash on the input.
  handler()->HandleSetCategoryPermissionForPattern(&args);

  scoped_refptr<const extensions::Extension> extension;
  extension = extensions::ExtensionBuilder()
                  .SetManifest(extensions::DictionaryBuilder()
                                   .Set("name", kExtensionName)
                                   .Set("version", "1.0.0")
                                   .Set("manifest_version", 2)
                                   .Build())
                  .SetID("ahfgeienlihckogmohjhadlkjgocpleb")
                  .Build();

  std::unique_ptr<base::ListValue> exceptions(new base::ListValue);
  site_settings::AddExceptionForHostedApp(
      "[*.]google.com", *extension.get(), exceptions.get());

  const base::DictionaryValue* dictionary;
  CHECK(exceptions->GetDictionary(0, &dictionary));
  CHECK(dictionary->GetString(site_settings::kOrigin, &primary_pattern));
  CHECK(dictionary->GetString(site_settings::kDisplayName, &display_name));
  CHECK(dictionary->GetString(site_settings::kEmbeddingOrigin,
                              &secondary_pattern));
  CHECK(dictionary->GetString(site_settings::kSetting, &type));
  CHECK(dictionary->GetBoolean(site_settings::kIncognito, &incognito));

  // Again, don't need to check the results.
  handler()->HandleSetCategoryPermissionForPattern(&args);
}

TEST_F(SiteSettingsHandlerTest, ExtensionDisplayName) {
  auto* extension_registry = extensions::ExtensionRegistry::Get(profile());
  std::string test_extension_id = "test-extension-url";
  std::string test_extension_url = "chrome-extension://" + test_extension_id;
  scoped_refptr<const extensions::Extension> extension =
      extensions::ExtensionBuilder()
          .SetManifest(extensions::DictionaryBuilder()
                           .Set("name", kExtensionName)
                           .Set("version", "1.0.0")
                           .Set("manifest_version", 2)
                           .Build())
          .SetID(test_extension_id)
          .Build();
  extension_registry->AddEnabled(extension);

  base::ListValue get_origin_permissions_args;
  get_origin_permissions_args.AppendString(kCallbackId);
  get_origin_permissions_args.AppendString(test_extension_url);
  {
    auto category_list = std::make_unique<base::ListValue>();
    category_list->AppendString(kNotifications);
    get_origin_permissions_args.Append(std::move(category_list));
  }
  handler()->HandleGetOriginPermissions(&get_origin_permissions_args);
  ValidateOrigin(test_extension_url, test_extension_url, kExtensionName,
                 CONTENT_SETTING_ASK,
                 site_settings::SiteSettingSource::kDefault, 1U);
}

TEST_F(SiteSettingsHandlerTest, PatternsAndContentType) {
  unsigned counter = 1;
  for (const auto& test_case : kPatternsAndContentTypeTestCases) {
    base::ListValue args;
    args.AppendString(kCallbackId);
    args.AppendString(test_case.arguments.pattern);
    args.AppendString(test_case.arguments.content_type);
    handler()->HandleIsPatternValidForType(&args);
    ValidatePattern(test_case.expected.validity, counter,
                    test_case.expected.reason);
    ++counter;
  }
}

TEST_F(SiteSettingsHandlerTest, Incognito) {
  base::ListValue args;
  handler()->HandleUpdateIncognitoStatus(&args);
  ValidateIncognitoExists(false, 1U);

  CreateIncognitoProfile();
  ValidateIncognitoExists(true, 2U);

  DestroyIncognitoProfile();
  ValidateIncognitoExists(false, 3U);
}

TEST_F(SiteSettingsHandlerTest, ZoomLevels) {
  std::string host("http://www.google.com");
  double zoom_level = 1.1;

  content::HostZoomMap* host_zoom_map =
      content::HostZoomMap::GetDefaultForBrowserContext(profile());
  host_zoom_map->SetZoomLevelForHost(host, zoom_level);
  ValidateZoom(host, "122%", 1U);

  base::ListValue args;
  handler()->HandleFetchZoomLevels(&args);
  ValidateZoom(host, "122%", 2U);

  args.AppendString("http://www.google.com");
  handler()->HandleRemoveZoomLevel(&args);
  ValidateZoom("", "", 3U);

  double default_level = host_zoom_map->GetDefaultZoomLevel();
  double level = host_zoom_map->GetZoomLevelForHostAndScheme("http", host);
  EXPECT_EQ(default_level, level);
}

class SiteSettingsHandlerInfobarTest : public BrowserWithTestWindowTest {
 public:
  SiteSettingsHandlerInfobarTest()
      : kNotifications(site_settings::ContentSettingsTypeToGroupName(
            CONTENT_SETTINGS_TYPE_NOTIFICATIONS)) {}

  void SetUp() override {
    BrowserWithTestWindowTest::SetUp();
    handler_ = std::make_unique<SiteSettingsHandler>(profile());
    handler()->set_web_ui(web_ui());
    handler()->AllowJavascript();
    web_ui()->ClearTrackedCalls();

    window2_ = base::WrapUnique(CreateBrowserWindow());
    browser2_ = base::WrapUnique(
        CreateBrowser(profile(), browser()->type(), false, window2_.get()));

    extensions::TestExtensionSystem* extension_system =
        static_cast<extensions::TestExtensionSystem*>(
            extensions::ExtensionSystem::Get(profile()));
    extension_system->CreateExtensionService(
        base::CommandLine::ForCurrentProcess(), base::FilePath(), false);
  }

  void TearDown() override {
    // SiteSettingsHandler maintains a HostZoomMap::Subscription internally, so
    // make sure that's cleared before BrowserContext / profile destruction.
    handler()->DisallowJavascript();

    // Also destroy |browser2_| before the profile. browser()'s destruction is
    // handled in BrowserWithTestWindowTest::TearDown().
    browser2()->tab_strip_model()->CloseAllTabs();
    browser2_.reset();
    BrowserWithTestWindowTest::TearDown();
  }

  InfoBarService* GetInfobarServiceForTab(Browser* browser,
                                          int tab_index,
                                          GURL* tab_url) {
    content::WebContents* web_contents =
        browser->tab_strip_model()->GetWebContentsAt(tab_index);
    if (tab_url)
      *tab_url = web_contents->GetLastCommittedURL();
    return InfoBarService::FromWebContents(web_contents);
  }

  content::TestWebUI* web_ui() { return &web_ui_; }

  SiteSettingsHandler* handler() { return handler_.get(); }

  Browser* browser2() { return browser2_.get(); }

  const std::string kNotifications;

 private:
  content::TestWebUI web_ui_;
  std::unique_ptr<SiteSettingsHandler> handler_;
  std::unique_ptr<BrowserWindow> window2_;
  std::unique_ptr<Browser> browser2_;

  DISALLOW_COPY_AND_ASSIGN(SiteSettingsHandlerInfobarTest);
};

TEST_F(SiteSettingsHandlerInfobarTest, SettingPermissionsTriggersInfobar) {
  // Note all GURLs starting with 'origin' below belong to the same origin.
  //               _____  _______________  ________  ________  ___________
  //   Window 1:  / foo \' origin_anchor \' chrome \' origin \' extension \
  // -------------       -----------------------------------------------------
  std::string origin_anchor_string =
      "https://www.example.com/with/path/blah#heading";
  const GURL foo("http://foo");
  const GURL origin_anchor(origin_anchor_string);
  const GURL chrome("chrome://about");
  const GURL origin("https://www.example.com/");
  const GURL extension(
      "chrome-extension://fooooooooooooooooooooooooooooooo/bar.html");

  // Make sure |extension|'s extension ID exists before navigating to it. This
  // fixes a test timeout that occurs with --enable-browser-side-navigation on.
  scoped_refptr<const extensions::Extension> test_extension =
      extensions::ExtensionBuilder("Test")
          .SetID("fooooooooooooooooooooooooooooooo")
          .Build();
  extensions::ExtensionSystem::Get(profile())
      ->extension_service()
      ->AddExtension(test_extension.get());

  //               __________  ______________  ___________________  _______
  //   Window 2:  / insecure '/ origin_query \' example_subdomain \' about \
  // -------------------------                --------------------------------
  const GURL insecure("http://www.example.com/");
  const GURL origin_query("https://www.example.com/?param=value");
  const GURL example_subdomain("https://subdomain.example.com/");
  const GURL about(url::kAboutBlankURL);

  // Set up. Note AddTab() adds tab at index 0, so add them in reverse order.
  AddTab(browser(), extension);
  AddTab(browser(), origin);
  AddTab(browser(), chrome);
  AddTab(browser(), origin_anchor);
  AddTab(browser(), foo);
  for (int i = 0; i < browser()->tab_strip_model()->count(); ++i) {
    EXPECT_EQ(0u,
              GetInfobarServiceForTab(browser(), i, nullptr)->infobar_count());
  }

  AddTab(browser2(), about);
  AddTab(browser2(), example_subdomain);
  AddTab(browser2(), origin_query);
  AddTab(browser2(), insecure);
  for (int i = 0; i < browser2()->tab_strip_model()->count(); ++i) {
    EXPECT_EQ(0u,
              GetInfobarServiceForTab(browser2(), i, nullptr)->infobar_count());
  }

  // Block notifications.
  base::ListValue set_args;
  set_args.AppendString(origin_anchor_string);
  {
    auto category_list = std::make_unique<base::ListValue>();
    category_list->AppendString(kNotifications);
    set_args.Append(std::move(category_list));
  }
  set_args.AppendString(
      content_settings::ContentSettingToString(CONTENT_SETTING_BLOCK));
  handler()->HandleSetOriginPermissions(&set_args);

  // Make sure all tabs belonging to the same origin as |origin_anchor| have an
  // infobar shown.
  GURL tab_url;
  for (int i = 0; i < browser()->tab_strip_model()->count(); ++i) {
    if (i == /*origin_anchor=*/1 || i == /*origin=*/3) {
      EXPECT_EQ(
          1u, GetInfobarServiceForTab(browser(), i, &tab_url)->infobar_count());
      EXPECT_TRUE(url::IsSameOriginWith(origin, tab_url));
    } else {
      EXPECT_EQ(
          0u, GetInfobarServiceForTab(browser(), i, &tab_url)->infobar_count());
      EXPECT_FALSE(url::IsSameOriginWith(origin, tab_url));
    }
  }
  for (int i = 0; i < browser2()->tab_strip_model()->count(); ++i) {
    if (i == /*origin_query=*/1) {
      EXPECT_EQ(
          1u,
          GetInfobarServiceForTab(browser2(), i, &tab_url)->infobar_count());
      EXPECT_TRUE(url::IsSameOriginWith(origin, tab_url));
    } else {
      EXPECT_EQ(
          0u,
          GetInfobarServiceForTab(browser2(), i, &tab_url)->infobar_count());
      EXPECT_FALSE(url::IsSameOriginWith(origin, tab_url));
    }
  }

  // Navigate the |foo| tab to the same origin as |origin_anchor|, and the
  // |origin_query| tab to a different origin.
  const GURL origin_path("https://www.example.com/path/to/page.html");
  content::NavigationController* foo_controller =
      &browser()
           ->tab_strip_model()
           ->GetWebContentsAt(/*foo=*/0)
           ->GetController();
  NavigateAndCommit(foo_controller, origin_path);

  const GURL example_without_www("https://example.com/");
  content::NavigationController* origin_query_controller =
      &browser2()
           ->tab_strip_model()
           ->GetWebContentsAt(/*origin_query=*/1)
           ->GetController();
  NavigateAndCommit(origin_query_controller, example_without_www);

  // Reset all permissions.
  base::ListValue reset_args;
  reset_args.AppendString(origin_anchor_string);
  auto category_list = std::make_unique<base::ListValue>();
  category_list->AppendString(kNotifications);
  reset_args.Append(std::move(category_list));
  reset_args.AppendString(
      content_settings::ContentSettingToString(CONTENT_SETTING_DEFAULT));
  handler()->HandleSetOriginPermissions(&reset_args);

  // Check the same tabs (plus the tab navigated to |origin_path|) still have
  // infobars showing.
  for (int i = 0; i < browser()->tab_strip_model()->count(); ++i) {
    if (i == /*origin_path=*/0 || i == /*origin_anchor=*/1 ||
        i == /*origin=*/3) {
      EXPECT_EQ(
          1u, GetInfobarServiceForTab(browser(), i, &tab_url)->infobar_count());
      EXPECT_TRUE(url::IsSameOriginWith(origin, tab_url));
    } else {
      EXPECT_EQ(
          0u, GetInfobarServiceForTab(browser(), i, &tab_url)->infobar_count());
      EXPECT_FALSE(url::IsSameOriginWith(origin, tab_url));
    }
  }
  // The infobar on the original |origin_query| tab (which has now been
  // navigated to |example_without_www|) should disappear.
  for (int i = 0; i < browser2()->tab_strip_model()->count(); ++i) {
    EXPECT_EQ(
        0u, GetInfobarServiceForTab(browser2(), i, &tab_url)->infobar_count());
    EXPECT_FALSE(url::IsSameOriginWith(origin, tab_url));
  }

  // Make sure it's the correct infobar that's being shown.
  EXPECT_EQ(infobars::InfoBarDelegate::PAGE_INFO_INFOBAR_DELEGATE,
            GetInfobarServiceForTab(browser(), /*origin_path=*/0, &tab_url)
                ->infobar_at(0)
                ->delegate()
                ->GetIdentifier());
  EXPECT_TRUE(url::IsSameOriginWith(origin, tab_url));
}

TEST_F(SiteSettingsHandlerTest, SessionOnlyException) {
  const std::string google_with_port("https://www.google.com:443");
  const std::string uma_base("WebsiteSettings.Menu.PermissionChanged");
  base::ListValue set_args;
  set_args.AppendString(google_with_port);  // Primary pattern.
  set_args.AppendString(google_with_port);  // Secondary pattern.
  set_args.AppendString(kCookies);
  set_args.AppendString(
      content_settings::ContentSettingToString(CONTENT_SETTING_SESSION_ONLY));
  set_args.AppendBoolean(false);  // Incognito.
  base::HistogramTester histograms;
  handler()->HandleSetCategoryPermissionForPattern(&set_args);
  EXPECT_EQ(1U, web_ui()->call_data().size());
  histograms.ExpectTotalCount(uma_base, 1);
  histograms.ExpectTotalCount(uma_base + ".SessionOnly", 1);
}

TEST_F(SiteSettingsHandlerTest, BlockAutoplay_SendOnRequest) {
  base::ListValue args;
  handler()->HandleFetchBlockAutoplayStatus(&args);

  // Check that we are checked and enabled.
  ValidateBlockAutoplay(true, true);
}

TEST_F(SiteSettingsHandlerTest, BlockAutoplay_SoundSettingUpdate) {
  SetSoundContentSettingDefault(CONTENT_SETTING_BLOCK);
  base::RunLoop().RunUntilIdle();

  // Check that we are not checked or enabled.
  ValidateBlockAutoplay(false, false);

  SetSoundContentSettingDefault(CONTENT_SETTING_ALLOW);
  base::RunLoop().RunUntilIdle();

  // Check that we are checked and enabled.
  ValidateBlockAutoplay(true, true);
}

TEST_F(SiteSettingsHandlerTest, BlockAutoplay_PrefUpdate) {
  profile()->GetPrefs()->SetBoolean(prefs::kBlockAutoplayEnabled, false);
  base::RunLoop().RunUntilIdle();

  // Check that we are not checked but are enabled.
  ValidateBlockAutoplay(false, true);

  profile()->GetPrefs()->SetBoolean(prefs::kBlockAutoplayEnabled, true);
  base::RunLoop().RunUntilIdle();

  // Check that we are checked and enabled.
  ValidateBlockAutoplay(true, true);
}

TEST_F(SiteSettingsHandlerTest, BlockAutoplay_Update) {
  EXPECT_TRUE(profile()->GetPrefs()->GetBoolean(prefs::kBlockAutoplayEnabled));

  base::ListValue data;
  data.AppendBoolean(false);

  handler()->HandleSetBlockAutoplayEnabled(&data);
  EXPECT_FALSE(profile()->GetPrefs()->GetBoolean(prefs::kBlockAutoplayEnabled));
}

namespace {

const GURL kAndroidOrigin("https://android.com");
const GURL kChromiumOrigin("https://chromium.org");
const GURL kGoogleOrigin("https://google.com");

constexpr char kUsbPolicySetting[] = R"(
    [
      {
        "devices": [{ "vendor_id": 6353, "product_id": 5678 }],
        "urls": ["https://chromium.org"]
      }, {
        "devices": [{ "vendor_id": 6353 }],
        "urls": ["https://google.com,https://android.com"]
      }, {
        "devices": [{ "vendor_id": 6354 }],
        "urls": ["https://android.com,"]
      }, {
        "devices": [{}],
        "urls": ["https://google.com,https://google.com"]
      }
    ])";

}  // namespace

class SiteSettingsHandlerChooserExceptionTest : public SiteSettingsHandlerTest {
 protected:
  void SetUp() override {
    // Set up UsbChooserContext first, since the granting of device permissions
    // causes the WebUI listener callbacks for
    // contentSettingSitePermissionChanged and
    // contentSettingChooserPermissionChanged to be fired. The base class SetUp
    // method reset the WebUI call data.
    SetUpUsbChooserContext();
    SiteSettingsHandlerTest::SetUp();
  }

  void TearDown() override {
    auto* chooser_context = UsbChooserContextFactory::GetForProfile(profile());
    chooser_context->ChooserContextBase::RemoveObserver(&observer_);
  }

  // Sets up the UsbChooserContext with two devices and permissions for these
  // devices. It also adds three policy defined permissions. There are three
  // devices that are granted user permissions. Two are covered by different
  // policy permissions, while the third is not covered by policy at all. These
  // unit tests will check that the WebUI is able to receive the exceptions and
  // properly manipulate their permissions.
  void SetUpUsbChooserContext() {
    persistent_device_info_ = device_manager_.CreateAndAddDevice(
        6353, 5678, "Google", "Gizmo", "123ABC");
    ephemeral_device_info_ =
        device_manager_.CreateAndAddDevice(6354, 0, "Google", "Gadget", "");
    user_granted_device_info_ = device_manager_.CreateAndAddDevice(
        6355, 0, "Google", "Widget", "789XYZ");

    auto* chooser_context = UsbChooserContextFactory::GetForProfile(profile());
    device::mojom::UsbDeviceManagerPtr device_manager_ptr;
    device_manager_.AddBinding(mojo::MakeRequest(&device_manager_ptr));
    chooser_context->SetDeviceManagerForTesting(std::move(device_manager_ptr));
    chooser_context->GetDevices(
        base::DoNothing::Once<std::vector<device::mojom::UsbDeviceInfoPtr>>());
    base::RunLoop().RunUntilIdle();

    // Add the user granted permissions for testing.
    // These two persistent device permissions should be lumped together with
    // the policy permissions, since they apply to the same device and URL.
    chooser_context->GrantDevicePermission(kChromiumOrigin, kChromiumOrigin,
                                           *persistent_device_info_);
    chooser_context->GrantDevicePermission(kChromiumOrigin, kGoogleOrigin,
                                           *persistent_device_info_);
    chooser_context->GrantDevicePermission(kAndroidOrigin, kChromiumOrigin,
                                           *persistent_device_info_);
    chooser_context->GrantDevicePermission(kAndroidOrigin, kAndroidOrigin,
                                           *ephemeral_device_info_);
    chooser_context->GrantDevicePermission(kAndroidOrigin, kAndroidOrigin,
                                           *user_granted_device_info_);

    // Add the policy granted permissions for testing.
    auto policy_value = base::JSONReader::ReadDeprecated(kUsbPolicySetting);
    DCHECK(policy_value);
    profile()->GetPrefs()->Set(prefs::kManagedWebUsbAllowDevicesForUrls,
                               *policy_value);

    // Add the observer for permission changes.
    chooser_context->ChooserContextBase::AddObserver(&observer_);
  }

  void SetUpOffTheRecordUsbChooserContext() {
    off_the_record_device_ = device_manager_.CreateAndAddDevice(
        6353, 8765, "Google", "Contraption", "A9B8C7");

    CreateIncognitoProfile();
    auto* chooser_context =
        UsbChooserContextFactory::GetForProfile(incognito_profile());
    device::mojom::UsbDeviceManagerPtr device_manager_ptr;
    device_manager_.AddBinding(mojo::MakeRequest(&device_manager_ptr));
    chooser_context->SetDeviceManagerForTesting(std::move(device_manager_ptr));
    chooser_context->GetDevices(
        base::DoNothing::Once<std::vector<device::mojom::UsbDeviceInfoPtr>>());
    base::RunLoop().RunUntilIdle();

    chooser_context->GrantDevicePermission(kChromiumOrigin, kAndroidOrigin,
                                           *off_the_record_device_);

    // Add the observer for permission changes.
    chooser_context->ChooserContextBase::AddObserver(&observer_);
  }

  void DestroyIncognitoProfile() override {
    auto* chooser_context =
        UsbChooserContextFactory::GetForProfile(incognito_profile());
    chooser_context->ChooserContextBase::RemoveObserver(&observer_);

    SiteSettingsHandlerTest::DestroyIncognitoProfile();
  }

  // Call SiteSettingsHandler::HandleGetChooserExceptionList for |chooser_type|
  // and return the exception list received by the WebUI.
  void ValidateChooserExceptionList(const std::string& chooser_type,
                                    size_t expected_total_calls) {
    base::ListValue args;
    args.AppendString(kCallbackId);
    args.AppendString(chooser_type);

    handler()->HandleGetChooserExceptionList(&args);

    EXPECT_EQ(web_ui()->call_data().size(), expected_total_calls);

    const content::TestWebUI::CallData& data = *web_ui()->call_data().back();
    EXPECT_EQ("cr.webUIResponse", data.function_name());

    ASSERT_TRUE(data.arg1());
    ASSERT_TRUE(data.arg1()->is_string());
    EXPECT_EQ(data.arg1()->GetString(), kCallbackId);

    ASSERT_TRUE(data.arg2());
    ASSERT_TRUE(data.arg2()->is_bool());
    EXPECT_TRUE(data.arg2()->GetBool());

    ASSERT_TRUE(data.arg3());
    ASSERT_TRUE(data.arg3()->is_list());
  }

  const base::Value& GetChooserExceptionListFromWebUiCallData(
      const std::string& chooser_type,
      size_t expected_total_calls) {
    ValidateChooserExceptionList(chooser_type, expected_total_calls);
    return *web_ui()->call_data().back()->arg3();
  }

  // Iterate through the exception's sites array and return true if a site
  // exception matches |requesting_origin| and |embedding_origin|.
  bool ChooserExceptionContainsSiteException(
      const base::Value& exception,
      const std::string& requesting_origin,
      const std::string& embedding_origin) {
    const base::Value* site_value = exception.FindKey(site_settings::kSites);
    for (const auto& site : site_value->GetList()) {
      const base::Value* origin_value = site.FindKey(site_settings::kOrigin);
      if (origin_value->GetString() != requesting_origin)
        continue;

      const base::Value* embedding_origin_value =
          site.FindKey(site_settings::kEmbeddingOrigin);
      if (embedding_origin_value->GetString() == embedding_origin)
        return true;
    }
    return false;
  }

  // Iterate through the |exception_list| array and return true if there is a
  // chooser exception with |display_name| that contains a site exception for
  // |requesting_origin| and |embedding_origin|.
  bool ChooserExceptionContainsSiteException(
      const base::Value& exception_list,
      const std::string& display_name,
      const std::string& requesting_origin,
      const std::string& embedding_origin) {
    for (const auto& exception : exception_list.GetList()) {
      const base::Value* display_name_value =
          exception.FindKey(site_settings::kDisplayName);
      if (display_name_value->GetString() == display_name) {
        return ChooserExceptionContainsSiteException(
            exception, requesting_origin, embedding_origin);
      }
    }
    return false;
  }

  device::mojom::UsbDeviceInfoPtr ephemeral_device_info_;
  device::mojom::UsbDeviceInfoPtr off_the_record_device_;
  device::mojom::UsbDeviceInfoPtr persistent_device_info_;
  device::mojom::UsbDeviceInfoPtr user_granted_device_info_;

  MockPermissionObserver observer_;

 private:
  device::FakeUsbDeviceManager device_manager_;
};

TEST_F(SiteSettingsHandlerChooserExceptionTest,
       HandleGetChooserExceptionListForUsb) {
  const std::string kUsbChooserGroupName =
      site_settings::ContentSettingsTypeToGroupName(
          CONTENT_SETTINGS_TYPE_USB_CHOOSER_DATA);

  const base::Value& exceptions = GetChooserExceptionListFromWebUiCallData(
      kUsbChooserGroupName, /*expected_total_calls=*/1u);
  EXPECT_EQ(exceptions.GetList().size(), 5u);
}

TEST_F(SiteSettingsHandlerChooserExceptionTest,
       HandleGetChooserExceptionListForUsbOffTheRecord) {
  const std::string kUsbChooserGroupName =
      site_settings::ContentSettingsTypeToGroupName(
          CONTENT_SETTINGS_TYPE_USB_CHOOSER_DATA);
  SetUpOffTheRecordUsbChooserContext();
  web_ui()->ClearTrackedCalls();

  // The objects returned by GetChooserExceptionListFromProfile should also
  // include the incognito permissions. The two extra objects represent the
  // "Widget" device and the policy permission for "Unknown product 0x162E from
  // Google Inc.". The policy granted permission shows up here because the off
  // the record profile does not have a user granted permission for the
  // |persistent_device_info_|, so it cannot use the name of that device.
  {
    const base::Value& exceptions = GetChooserExceptionListFromWebUiCallData(
        kUsbChooserGroupName, /*expected_total_calls=*/1u);
    EXPECT_EQ(exceptions.GetList().size(), 7u);
    for (const auto& exception : exceptions.GetList()) {
      LOG(INFO) << exception.FindKey(site_settings::kDisplayName)->GetString();
    }
  }

  // Destroy the off the record profile and check that the objects returned do
  // not include incognito permissions anymore. The destruction of the profile
  // causes the "onIncognitoStatusChanged" WebUIListener callback to fire.
  DestroyIncognitoProfile();
  EXPECT_EQ(web_ui()->call_data().size(), 2u);

  {
    const base::Value& exceptions = GetChooserExceptionListFromWebUiCallData(
        kUsbChooserGroupName, /*expected_total_calls=*/3u);
    EXPECT_EQ(exceptions.GetList().size(), 5u);
  }
}

TEST_F(SiteSettingsHandlerChooserExceptionTest,
       HandleResetChooserExceptionForSiteForUsb) {
  const std::string kUsbChooserGroupName =
      site_settings::ContentSettingsTypeToGroupName(
          CONTENT_SETTINGS_TYPE_USB_CHOOSER_DATA);
  const std::string kAndroidOriginStr = kAndroidOrigin.GetOrigin().spec();
  const std::string kChromiumOriginStr = kChromiumOrigin.GetOrigin().spec();

  {
    const base::Value& exceptions = GetChooserExceptionListFromWebUiCallData(
        kUsbChooserGroupName, /*expected_total_calls=*/1u);
    EXPECT_EQ(exceptions.GetList().size(), 5u);
  }

  // User granted USB permissions for devices also containing policy permissions
  // should be able to be reset without removing the chooser exception object
  // from the list.
  base::ListValue args;
  args.AppendString(kUsbChooserGroupName);
  args.AppendString(kAndroidOriginStr);
  args.AppendString(kChromiumOriginStr);
  args.Append(
      UsbChooserContext::DeviceInfoToDictValue(*persistent_device_info_));

  EXPECT_CALL(observer_, OnChooserObjectPermissionChanged(
                             CONTENT_SETTINGS_TYPE_USB_GUARD,
                             CONTENT_SETTINGS_TYPE_USB_CHOOSER_DATA));
  EXPECT_CALL(observer_, OnPermissionRevoked(kAndroidOrigin, kChromiumOrigin));
  handler()->HandleResetChooserExceptionForSite(&args);

  // The HandleResetChooserExceptionForSite() method should have also caused the
  // WebUIListenerCallbacks for contentSettingSitePermissionChanged and
  // contentSettingChooserPermissionChanged to fire.
  EXPECT_EQ(web_ui()->call_data().size(), 3u);
  {
    // The exception list size should not have been reduced since there is still
    // a policy granted permission for the "Gizmo" device.
    const base::Value& exceptions = GetChooserExceptionListFromWebUiCallData(
        kUsbChooserGroupName, /*expected_total_calls=*/4u);
    EXPECT_EQ(exceptions.GetList().size(), 5u);

    // Ensure that the sites list does not contain the URLs of the removed
    // permission.
    EXPECT_FALSE(ChooserExceptionContainsSiteException(
        exceptions, "Gizmo", kAndroidOriginStr, kChromiumOriginStr));
  }

  // User granted USB permissions that are also granted by policy should not
  // be able to be reset.
  args.Clear();
  args.AppendString(kUsbChooserGroupName);
  args.AppendString(kChromiumOriginStr);
  args.AppendString(kChromiumOriginStr);
  args.Append(
      UsbChooserContext::DeviceInfoToDictValue(*persistent_device_info_));

  {
    const base::Value& exceptions =
        GetChooserExceptionListFromWebUiCallData(kUsbChooserGroupName, 5u);
    EXPECT_EQ(exceptions.GetList().size(), 5u);

    // User granted exceptions that are also granted by policy are only
    // displayed through the policy granted site exception, so ensure that a
    // site exception entry for a requesting and embedding origin of
    // kChromiumOriginStr does not exist.
    EXPECT_TRUE(ChooserExceptionContainsSiteException(
        exceptions, "Gizmo", kChromiumOriginStr, std::string()));
    EXPECT_FALSE(ChooserExceptionContainsSiteException(
        exceptions, "Gizmo", kChromiumOriginStr, kChromiumOriginStr));
  }

  EXPECT_CALL(observer_, OnChooserObjectPermissionChanged(
                             CONTENT_SETTINGS_TYPE_USB_GUARD,
                             CONTENT_SETTINGS_TYPE_USB_CHOOSER_DATA));
  EXPECT_CALL(observer_, OnPermissionRevoked(kChromiumOrigin, kChromiumOrigin));
  handler()->HandleResetChooserExceptionForSite(&args);

  // The HandleResetChooserExceptionForSite() method should have also caused the
  // WebUIListenerCallbacks for contentSettingSitePermissionChanged and
  // contentSettingChooserPermissionChanged to fire.
  EXPECT_EQ(web_ui()->call_data().size(), 7u);
  {
    const base::Value& exceptions = GetChooserExceptionListFromWebUiCallData(
        kUsbChooserGroupName, /*expected_total_calls=*/8u);
    EXPECT_EQ(exceptions.GetList().size(), 5u);

    // Ensure that the sites list still displays a site exception entry for a
    // requesting origin of kChromiumOriginStr and a wildcard embedding origin.
    EXPECT_TRUE(ChooserExceptionContainsSiteException(
        exceptions, "Gizmo", kChromiumOriginStr, std::string()));
    EXPECT_FALSE(ChooserExceptionContainsSiteException(
        exceptions, "Gizmo", kChromiumOriginStr, kChromiumOriginStr));
  }

  // User granted USB permissions that are not covered by policy should be able
  // to be reset and the chooser exception entry should be removed from the list
  // when the exception only has one site exception granted to it..
  args.Clear();
  args.AppendString(kUsbChooserGroupName);
  args.AppendString(kAndroidOriginStr);
  args.AppendString(kAndroidOriginStr);
  args.Append(
      UsbChooserContext::DeviceInfoToDictValue(*user_granted_device_info_));

  {
    const base::Value& exceptions =
        GetChooserExceptionListFromWebUiCallData(kUsbChooserGroupName, 9u);
    EXPECT_EQ(exceptions.GetList().size(), 5u);
    EXPECT_TRUE(ChooserExceptionContainsSiteException(
        exceptions, "Widget", kAndroidOriginStr, kAndroidOriginStr));
  }

  EXPECT_CALL(observer_, OnChooserObjectPermissionChanged(
                             CONTENT_SETTINGS_TYPE_USB_GUARD,
                             CONTENT_SETTINGS_TYPE_USB_CHOOSER_DATA));
  EXPECT_CALL(observer_, OnPermissionRevoked(kAndroidOrigin, kAndroidOrigin));
  handler()->HandleResetChooserExceptionForSite(&args);

  // The HandleResetChooserExceptionForSite() method should have also caused the
  // WebUIListenerCallbacks for contentSettingSitePermissionChanged and
  // contentSettingChooserPermissionChanged to fire.
  EXPECT_EQ(web_ui()->call_data().size(), 11u);
  {
    const base::Value& exceptions = GetChooserExceptionListFromWebUiCallData(
        kUsbChooserGroupName, /*expected_total_calls=*/12u);
    EXPECT_EQ(exceptions.GetList().size(), 4u);
    EXPECT_FALSE(ChooserExceptionContainsSiteException(
        exceptions, "Widget", kAndroidOriginStr, kAndroidOriginStr));
  }
}

TEST_F(SiteSettingsHandlerTest, HandleClearEtldPlus1DataAndCookies) {
  SetUpCookiesTreeModel();

  EXPECT_EQ(22, handler()->cookies_tree_model_->GetRoot()->GetTotalNodeCount());

  const base::ListValue* storage_and_cookie_list =
      GetOnStorageFetchedSentListValue();
  EXPECT_EQ(3U, storage_and_cookie_list->GetSize());
  const base::DictionaryValue* site_group;
  ASSERT_TRUE(storage_and_cookie_list->GetDictionary(0, &site_group));
  std::string etld_plus1_string;
  ASSERT_TRUE(site_group->GetString("etldPlus1", &etld_plus1_string));
  ASSERT_EQ("example.com", etld_plus1_string);

  base::ListValue args;
  args.AppendString("example.com");
  handler()->HandleClearEtldPlus1DataAndCookies(&args);
  EXPECT_EQ(11, handler()->cookies_tree_model_->GetRoot()->GetTotalNodeCount());

  storage_and_cookie_list = GetOnStorageFetchedSentListValue();
  EXPECT_EQ(2U, storage_and_cookie_list->GetSize());
  ASSERT_TRUE(storage_and_cookie_list->GetDictionary(0, &site_group));
  ASSERT_TRUE(site_group->GetString("etldPlus1", &etld_plus1_string));
  ASSERT_EQ("google.com", etld_plus1_string);

  args.Clear();
  args.AppendString("google.com");

  handler()->HandleClearEtldPlus1DataAndCookies(&args);

  EXPECT_EQ(4, handler()->cookies_tree_model_->GetRoot()->GetTotalNodeCount());

  storage_and_cookie_list = GetOnStorageFetchedSentListValue();
  EXPECT_EQ(1U, storage_and_cookie_list->GetSize());
  ASSERT_TRUE(storage_and_cookie_list->GetDictionary(0, &site_group));
  ASSERT_TRUE(site_group->GetString("etldPlus1", &etld_plus1_string));
  ASSERT_EQ("google.com.au", etld_plus1_string);

  args.Clear();
  args.AppendString("google.com.au");

  handler()->HandleClearEtldPlus1DataAndCookies(&args);

  EXPECT_EQ(1, handler()->cookies_tree_model_->GetRoot()->GetTotalNodeCount());

  storage_and_cookie_list = GetOnStorageFetchedSentListValue();
  EXPECT_EQ(0U, storage_and_cookie_list->GetSize());
}
}  // namespace settings
