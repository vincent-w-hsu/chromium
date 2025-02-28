// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package org.chromium.chrome.browser.tasks.tab_list_ui;

import org.chromium.ui.modelutil.PropertyKey;
import org.chromium.ui.modelutil.PropertyModel;

class TabListContainerProperties {
    public static final PropertyModel.WritableBooleanPropertyKey IS_VISIBLE =
            new PropertyModel.WritableBooleanPropertyKey();

    public static final PropertyModel.WritableBooleanPropertyKey IS_INCOGNITO =
            new PropertyModel.WritableBooleanPropertyKey();

    public static final PropertyModel
            .WritableObjectPropertyKey<TabListRecyclerView.VisibilityListener> VISIBILITY_LISTENER =
            new PropertyModel.WritableObjectPropertyKey<>();

    public static final PropertyModel.WritableIntPropertyKey INITIAL_SCROLL_INDEX =
            new PropertyModel.WritableIntPropertyKey();

    public static final PropertyModel.WritableBooleanPropertyKey ANIMATE_VISIBILITY_CHANGES =
            new PropertyModel.WritableBooleanPropertyKey();

    public static final PropertyModel.WritableIntPropertyKey TOP_CONTROLS_HEIGHT =
            new PropertyModel.WritableIntPropertyKey();

    public static final PropertyModel.WritableIntPropertyKey BOTTOM_CONTROLS_HEIGHT =
            new PropertyModel.WritableIntPropertyKey();

    public static final PropertyKey[] ALL_KEYS =
            new PropertyKey[] {IS_VISIBLE, IS_INCOGNITO, VISIBILITY_LISTENER, INITIAL_SCROLL_INDEX,
                    ANIMATE_VISIBILITY_CHANGES, TOP_CONTROLS_HEIGHT, BOTTOM_CONTROLS_HEIGHT};
}
