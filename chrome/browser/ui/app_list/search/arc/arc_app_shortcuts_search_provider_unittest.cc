// Copyright 2018 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/ui/app_list/search/arc/arc_app_shortcuts_search_provider.h"

#include <memory>
#include <utility>

#include "base/files/scoped_temp_dir.h"
#include "base/macros.h"
#include "base/strings/stringprintf.h"
#include "base/strings/utf_string_conversions.h"
#include "chrome/browser/chromeos/arc/icon_decode_request.h"
#include "chrome/browser/ui/app_list/app_list_test_util.h"
#include "chrome/browser/ui/app_list/arc/arc_app_list_prefs.h"
#include "chrome/browser/ui/app_list/arc/arc_app_test.h"
#include "chrome/browser/ui/app_list/search/chrome_search_result.h"
#include "chrome/browser/ui/app_list/search/search_result_ranker/app_search_result_ranker.h"
#include "chrome/browser/ui/app_list/search/search_result_ranker/ranking_item_util.h"
#include "chrome/browser/ui/app_list/test/test_app_list_controller_delegate.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace app_list {

namespace {
constexpr char kFakeAppPackageName[] = "FakeAppPackageName";
}  // namespace

class ArcAppShortcutsSearchProviderTest
    : public AppListTestBase,
      public ::testing::WithParamInterface<bool> {
 protected:
  ArcAppShortcutsSearchProviderTest() = default;
  ~ArcAppShortcutsSearchProviderTest() override = default;

  // AppListTestBase:
  void SetUp() override {
    AppListTestBase::SetUp();
    arc_test_.SetUp(profile());
    controller_ = std::make_unique<test::TestAppListControllerDelegate>();
    ASSERT_TRUE(temp_dir_.CreateUniqueTempDir());
    ranker_ =
        std::make_unique<AppSearchResultRanker>(temp_dir_.GetPath(),
                                                /*is_ephemeral_user=*/true);
  }

  void TearDown() override {
    controller_.reset();
    arc_test_.TearDown();
    AppListTestBase::TearDown();
  }

  arc::mojom::AppInfo CreateAppInfo(const std::string& name,
                                    const std::string& activity,
                                    const std::string& package_name) {
    arc::mojom::AppInfo appinfo;
    appinfo.name = name;
    appinfo.package_name = package_name;
    appinfo.activity = activity;
    return appinfo;
  }

  std::string AddArcAppAndShortcut(const arc::mojom::AppInfo& app_info,
                                   bool launchable) {
    ArcAppListPrefs* const prefs = arc_test_.arc_app_list_prefs();
    // Adding app to the prefs, and check that the app is accessible by id.
    prefs->AddAppAndShortcut(
        app_info.name, app_info.package_name, app_info.activity,
        std::string() /* intent_uri */, std::string() /* icon_resource_id */,
        false /* sticky */, true /* notifications_enabled */,
        true /* app_ready */, false /* suspended */, false /* shortcut */,
        launchable);
    const std::string app_id =
        ArcAppListPrefs::GetAppId(app_info.package_name, app_info.activity);
    EXPECT_TRUE(prefs->GetApp(app_id));
    return app_id;
  }

  base::ScopedTempDir temp_dir_;
  std::unique_ptr<AppSearchResultRanker> ranker_;
  std::unique_ptr<test::TestAppListControllerDelegate> controller_;
  ArcAppTest arc_test_;

 private:
  DISALLOW_COPY_AND_ASSIGN(ArcAppShortcutsSearchProviderTest);
};

TEST_P(ArcAppShortcutsSearchProviderTest, Basic) {
  const bool launchable = GetParam();

  const std::string app_id = AddArcAppAndShortcut(
      CreateAppInfo("FakeName", "FakeActivity", kFakeAppPackageName),
      launchable);

  const size_t kMaxResults = launchable ? 4 : 0;
  constexpr char kQuery[] = "shortlabel";

  auto provider = std::make_unique<ArcAppShortcutsSearchProvider>(
      kMaxResults, profile(), controller_.get(), ranker_.get());
  EXPECT_TRUE(provider->results().empty());
  arc::IconDecodeRequest::DisableSafeDecodingForTesting();

  provider->Start(base::UTF8ToUTF16(kQuery));
  const auto& results = provider->results();
  EXPECT_EQ(kMaxResults, results.size());
  // Verify search results.
  for (size_t i = 0; i < results.size(); ++i) {
    EXPECT_EQ(base::StringPrintf("ShortLabel %zu", i),
              base::UTF16ToUTF8(results[i]->title()));
    EXPECT_EQ(ash::SearchResultDisplayType::kTile, results[i]->display_type());
  }

  // If ranker_ is nullptr, the program won't break
  // TODO(crbug.com/931149): Add more tests to check ranker_ does have some
  // effects
  auto provider_null_ranker = std::make_unique<ArcAppShortcutsSearchProvider>(
      kMaxResults, profile(), controller_.get(), nullptr);

  EXPECT_TRUE(provider_null_ranker->results().empty());
  arc::IconDecodeRequest::DisableSafeDecodingForTesting();

  provider_null_ranker->Start(base::UTF8ToUTF16(kQuery));
  provider_null_ranker->Train(app_id, RankingItemType::kArcAppShortcut);
  const auto& results_null_ranker = provider_null_ranker->results();
  EXPECT_EQ(kMaxResults, results_null_ranker.size());
  // Verify search results.
  for (size_t i = 0; i < results_null_ranker.size(); ++i) {
    EXPECT_EQ(base::StringPrintf("ShortLabel %zu", i),
              base::UTF16ToUTF8(results_null_ranker[i]->title()));
    EXPECT_EQ(ash::SearchResultDisplayType::kTile,
              results_null_ranker[i]->display_type());
  }
}

INSTANTIATE_TEST_SUITE_P(, ArcAppShortcutsSearchProviderTest, testing::Bool());

}  // namespace app_list
