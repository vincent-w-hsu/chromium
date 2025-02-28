// Copyright 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPONENTS_PREVIEWS_CORE_PREVIEWS_FEATURES_H_
#define COMPONENTS_PREVIEWS_CORE_PREVIEWS_FEATURES_H_

#include "base/feature_list.h"

namespace previews {
namespace features {

extern const base::Feature kPreviews;
extern const base::Feature kOfflinePreviews;
extern const base::Feature kClientLoFi;
extern const base::Feature kNoScriptPreviews;
extern const base::Feature kStalePreviewsTimestamp;
extern const base::Feature kOptimizationHints;
extern const base::Feature kOptimizationHintsExperiments;
constexpr char kOptimizationHintsExperimentNameParam[] = "experiment_name";
extern const base::Feature kResourceLoadingHints;
extern const base::Feature kLitePageServerPreviews;
extern const base::Feature kAndroidOmniboxPreviewsBadge;
extern const base::Feature kSlowPageTriggering;
extern const base::Feature kHTTPSServerPreviewsUsingURLLoader;
extern const base::Feature kDataSaverLiteModeRebranding;
extern const base::Feature kPreviewsReloadsAreSoftOptOuts;

}  // namespace features
}  // namespace previews

#endif  // COMPONENTS_PREVIEWS_CORE_PREVIEWS_FEATURES_H_
