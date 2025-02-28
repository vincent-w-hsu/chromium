// Copyright 2018 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package org.chromium.chrome.browser.autofill_assistant.details;

import android.support.annotation.Nullable;

import org.chromium.base.annotations.CalledByNative;
import org.chromium.base.annotations.JNINamespace;

import java.text.ParseException;
import java.text.SimpleDateFormat;
import java.util.Calendar;
import java.util.Date;
import java.util.Locale;

/**
 * Java side equivalent of autofill_assistant::DetailsProto.
 */
@JNINamespace("autofill_assistant")
public class AssistantDetails {
    private static final String RFC_3339_FORMAT_WITHOUT_TIMEZONE = "yyyy'-'MM'-'dd'T'HH':'mm':'ss";

    private final String mTitle;
    private final String mImageUrl;
    @Nullable
    private final Date mDate;
    private final String mDescriptionLine1;
    private final String mDescriptionLine2;
    /** Whether user approval is required (i.e., due to changes). */
    private boolean mUserApprovalRequired;
    /** Whether the title should be highlighted. */
    private boolean mHighlightTitle;
    /** Whether the first description line should be highlighted. */
    private boolean mHighlightLine1;
    /** Whether the second description line should be highlighted. */
    private boolean mHighlightLine2;
    /** Whether empty fields should have the animated placeholder background. */
    private final boolean mShowPlaceholdersForEmptyFields;
    /**
     * The correctly formatted price for the client locale, including the currency.
     * Example: '$20.50' or '20.50 €'.
     */
    private final String mTotalPrice;
    /** An optional price label, such as 'Estimated Total incl. VAT'. */
    private final String mTotalPriceLabel;

    public AssistantDetails(String title, String imageUrl, String totalPriceLabel,
            String totalPrice, @Nullable Date date, String descriptionLine1,
            String descriptionLine2, boolean userApprovalRequired, boolean highlightTitle,
            boolean highlightLine1, boolean highlightLine2,
            boolean showPlaceholdersForEmptyFields) {
        this.mTotalPriceLabel = totalPriceLabel;
        this.mTitle = title;
        this.mImageUrl = imageUrl;
        this.mTotalPrice = totalPrice;
        this.mDate = date;
        this.mDescriptionLine1 = descriptionLine1;
        this.mDescriptionLine2 = descriptionLine2;

        this.mUserApprovalRequired = userApprovalRequired;
        this.mHighlightTitle = highlightTitle;
        this.mHighlightLine1 = highlightLine1;
        this.mHighlightLine2 = highlightLine2;
        this.mShowPlaceholdersForEmptyFields = showPlaceholdersForEmptyFields;
    }

    String getTitle() {
        return mTitle;
    }

    String getImageUrl() {
        return mImageUrl;
    }

    @Nullable
    Date getDate() {
        return mDate;
    }

    String getDescriptionLine1() {
        return mDescriptionLine1;
    }

    String getDescriptionLine2() {
        return mDescriptionLine2;
    }

    String getTotalPrice() {
        return mTotalPrice;
    }

    String getTotalPriceLabel() {
        return mTotalPriceLabel;
    }

    boolean getUserApprovalRequired() {
        return mUserApprovalRequired;
    }

    boolean getHighlightTitle() {
        return mHighlightTitle;
    }

    boolean getHighlightLine1() {
        return mHighlightLine1;
    }

    boolean getHighlightLine2() {
        return mHighlightLine2;
    }

    boolean getShowPlaceholdersForEmptyFields() {
        return mShowPlaceholdersForEmptyFields;
    }

    /**
     * Create details with the given values.
     */
    @CalledByNative
    private static AssistantDetails create(String title, String imageUrl, String totalPriceLabel,
            String totalPrice, String datetime, long year, int month, int day, int hour, int minute,
            int second, String descriptionLine1, String descriptionLine2,
            boolean userApprovalRequired, boolean highlightTitle, boolean highlightLine1,
            boolean highlightLine2) {
        Date date = null;
        if (year > 0 && month > 0 && day > 0 && hour >= 0 && minute >= 0 && second >= 0) {
            Calendar calendar = Calendar.getInstance();
            calendar.clear();
            // Month in Java Date is 0-based, but the one we receive from the server is 1-based.
            calendar.set((int) year, month - 1, day, hour, minute, second);
            date = calendar.getTime();
        } else if (!datetime.isEmpty()) {
            try {
                // The parameter contains the timezone shift from the current location, that we
                // don't care about.
                date = new SimpleDateFormat(RFC_3339_FORMAT_WITHOUT_TIMEZONE, Locale.ROOT)
                               .parse(datetime);
            } catch (ParseException e) {
                // Ignore.
            }
        }

        return new AssistantDetails(title, imageUrl, totalPriceLabel, totalPrice, date,
                descriptionLine1, descriptionLine2, userApprovalRequired, highlightTitle,
                highlightLine1, highlightLine2, /* showPlaceholdersForEmptyFields= */ false);
    }
}
