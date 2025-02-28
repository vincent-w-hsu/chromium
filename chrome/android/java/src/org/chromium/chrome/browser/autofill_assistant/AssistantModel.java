// Copyright 2018 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package org.chromium.chrome.browser.autofill_assistant;

import org.chromium.base.annotations.CalledByNative;
import org.chromium.base.annotations.JNINamespace;
import org.chromium.chrome.browser.autofill_assistant.carousel.AssistantCarouselModel;
import org.chromium.chrome.browser.autofill_assistant.details.AssistantDetailsModel;
import org.chromium.chrome.browser.autofill_assistant.header.AssistantHeaderModel;
import org.chromium.chrome.browser.autofill_assistant.overlay.AssistantOverlayModel;
import org.chromium.chrome.browser.autofill_assistant.payment.AssistantPaymentRequestModel;
import org.chromium.ui.modelutil.PropertyModel;

/**
 * State for the Autofill Assistant UI.
 */
@JNINamespace("autofill_assistant")
class AssistantModel extends PropertyModel {
    static final WritableBooleanPropertyKey ALLOW_SOFT_KEYBOARD = new WritableBooleanPropertyKey();
    static final WritableBooleanPropertyKey ALLOW_SWIPING_SHEET = new WritableBooleanPropertyKey();
    static final WritableBooleanPropertyKey VISIBLE = new WritableBooleanPropertyKey();

    private final AssistantOverlayModel mOverlayModel = new AssistantOverlayModel();
    private final AssistantHeaderModel mHeaderModel = new AssistantHeaderModel();
    private final AssistantDetailsModel mDetailsModel = new AssistantDetailsModel();
    private final AssistantPaymentRequestModel mPaymentRequestModel =
            new AssistantPaymentRequestModel();
    private final AssistantCarouselModel mSuggestionsModel = new AssistantCarouselModel();
    private final AssistantCarouselModel mActionsModel = new AssistantCarouselModel();

    AssistantModel() {
        super(ALLOW_SOFT_KEYBOARD, ALLOW_SWIPING_SHEET, VISIBLE);
    }

    @CalledByNative
    public AssistantOverlayModel getOverlayModel() {
        return mOverlayModel;
    }

    @CalledByNative
    public AssistantHeaderModel getHeaderModel() {
        return mHeaderModel;
    }

    @CalledByNative
    public AssistantDetailsModel getDetailsModel() {
        return mDetailsModel;
    }

    @CalledByNative
    public AssistantPaymentRequestModel getPaymentRequestModel() {
        return mPaymentRequestModel;
    }

    public AssistantCarouselModel getSuggestionsModel() {
        return mSuggestionsModel;
    }

    public AssistantCarouselModel getActionsModel() {
        return mActionsModel;
    }

    @CalledByNative
    private void setAllowSoftKeyboard(boolean allowed) {
        set(ALLOW_SOFT_KEYBOARD, allowed);
    }

    @CalledByNative
    private void setAllowSwipingSheet(boolean allowed) {
        set(ALLOW_SWIPING_SHEET, allowed);
    }

    @CalledByNative
    void setVisible(boolean visible) {
        set(VISIBLE, visible);
    }

    @CalledByNative
    private boolean getVisible() {
        return get(VISIBLE);
    }
}
