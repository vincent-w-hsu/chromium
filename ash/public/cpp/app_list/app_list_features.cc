// Copyright 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "ash/public/cpp/app_list/app_list_features.h"

#include "ash/public/cpp/app_list/app_list_switches.h"
#include "base/feature_list.h"
#include "base/metrics/field_trial_params.h"
#include "chromeos/constants/chromeos_switches.h"

namespace app_list_features {

const base::Feature kEnableAnswerCard{"EnableAnswerCard",
                                      base::FEATURE_ENABLED_BY_DEFAULT};
const base::Feature kEnableAppShortcutSearch{"EnableAppShortcutSearch",
                                             base::FEATURE_ENABLED_BY_DEFAULT};
const base::Feature kEnableBackgroundBlur{"EnableBackgroundBlur",
                                          base::FEATURE_DISABLED_BY_DEFAULT};
const base::Feature kEnablePlayStoreAppSearch{
    "EnablePlayStoreAppSearch", base::FEATURE_DISABLED_BY_DEFAULT};
const base::Feature kEnableAppDataSearch{"EnableAppDataSearch",
                                         base::FEATURE_DISABLED_BY_DEFAULT};
const base::Feature kEnableSettingsShortcutSearch{
    "EnableSettingsShortcutSearch", base::FEATURE_DISABLED_BY_DEFAULT};
const base::Feature kEnableZeroStateSuggestions{
    "EnableZeroStateSuggestions", base::FEATURE_ENABLED_BY_DEFAULT};
const base::Feature kEnableAppListSearchAutocomplete{
    "EnableAppListSearchAutocomplete", base::FEATURE_ENABLED_BY_DEFAULT};
const base::Feature kEnableAdaptiveResultRanker{
    "EnableAdaptiveResultRanker", base::FEATURE_DISABLED_BY_DEFAULT};
const base::Feature kEnableAppSearchResultRanker{
    "EnableAppSearchResultRanker", base::FEATURE_ENABLED_BY_DEFAULT};
const base::Feature kEnableAppReinstallZeroState{
    "EnableAppReinstallZeroState", base::FEATURE_DISABLED_BY_DEFAULT};
const base::Feature kEnableEmbeddedAssistantUI{
    "EnableEmbeddedAssistantUI", base::FEATURE_DISABLED_BY_DEFAULT};

bool IsAnswerCardEnabled() {
  // Not using local static variable to allow tests to change this value.
  // Do not show answer card if the embedded Assistant UI is enabled.
  return base::FeatureList::IsEnabled(kEnableAnswerCard) &&
         !IsEmbeddedAssistantUIEnabled();
}

bool IsAppShortcutSearchEnabled() {
  return base::FeatureList::IsEnabled(kEnableAppShortcutSearch);
}

bool IsBackgroundBlurEnabled() {
  return base::FeatureList::IsEnabled(kEnableBackgroundBlur);
}

bool IsPlayStoreAppSearchEnabled() {
  // Not using local static variable to allow tests to change this value.
  return base::FeatureList::IsEnabled(kEnablePlayStoreAppSearch);
}

bool IsAppDataSearchEnabled() {
  return base::FeatureList::IsEnabled(kEnableAppDataSearch);
}

bool IsSettingsShortcutSearchEnabled() {
  return base::FeatureList::IsEnabled(kEnableSettingsShortcutSearch);
}

bool IsZeroStateSuggestionsEnabled() {
  return base::FeatureList::IsEnabled(kEnableZeroStateSuggestions);
}

bool IsAppListSearchAutocompleteEnabled() {
  return base::FeatureList::IsEnabled(kEnableAppListSearchAutocomplete);
}

bool IsAdaptiveResultRankerEnabled() {
  return base::FeatureList::IsEnabled(kEnableAdaptiveResultRanker);
}

bool IsAppSearchResultRankerEnabled() {
  return base::FeatureList::IsEnabled(kEnableAppSearchResultRanker);
}

bool IsAppReinstallZeroStateEnabled() {
  return base::FeatureList::IsEnabled(kEnableAppReinstallZeroState);
}

bool IsEmbeddedAssistantUIEnabled() {
  return chromeos::switches::IsAssistantEnabled() &&
         base::FeatureList::IsEnabled(kEnableEmbeddedAssistantUI);
}

std::string AnswerServerUrl() {
  const std::string experiment_url =
      base::GetFieldTrialParamValueByFeature(kEnableAnswerCard, "ServerUrl");
  if (!experiment_url.empty())
    return experiment_url;
  return "https://www.google.com/coac";
}

std::string AnswerServerQuerySuffix() {
  return base::GetFieldTrialParamValueByFeature(kEnableAnswerCard,
                                                "QuerySuffix");
}

std::string AppSearchResultRankerPredictorName() {
  const std::string predictor_name = base::GetFieldTrialParamValueByFeature(
      kEnableAppSearchResultRanker, "app_search_result_ranker_predictor_name");
  if (!predictor_name.empty())
    return predictor_name;
  return std::string("MrfuAppLaunchPredictor");
}

}  // namespace app_list_features
