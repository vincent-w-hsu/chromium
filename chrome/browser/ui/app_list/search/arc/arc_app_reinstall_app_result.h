// Copyright 2018 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_UI_APP_LIST_SEARCH_ARC_ARC_APP_REINSTALL_APP_RESULT_H_
#define CHROME_BROWSER_UI_APP_LIST_SEARCH_ARC_ARC_APP_REINSTALL_APP_RESULT_H_

#include "base/macros.h"
#include "chrome/browser/ui/app_list/search/chrome_search_result.h"
#include "components/arc/common/app.mojom.h"
#include "ui/gfx/image/image_skia.h"

namespace app_list {

// A ChromeSearchResult that shows an App Reinstall candidate result. These are
// Arc++ apps that can be installed on this device. Opens the app in the play
// store when Open is called.
class ArcAppReinstallAppResult : public ChromeSearchResult {
 public:
  ArcAppReinstallAppResult(
      const arc::mojom::AppReinstallCandidatePtr& mojom_data,
      const gfx::ImageSkia& skia_icon);
  ~ArcAppReinstallAppResult() override;

  // ArcAppResult:
  void Open(int event_flags) override;

 private:
  DISALLOW_COPY_AND_ASSIGN(ArcAppReinstallAppResult);
};

}  // namespace app_list

#endif  // CHROME_BROWSER_UI_APP_LIST_SEARCH_ARC_ARC_APP_REINSTALL_APP_RESULT_H_
