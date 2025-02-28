// Copyright 2015 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package org.chromium.chrome.browser.omaha;

import android.app.Activity;
import android.content.ActivityNotFoundException;
import android.content.Intent;
import android.content.res.Resources;
import android.net.Uri;
import android.os.Handler;
import android.os.Looper;
import android.support.annotation.ColorInt;
import android.support.annotation.DrawableRes;
import android.support.annotation.NonNull;
import android.support.annotation.Nullable;
import android.support.annotation.StringRes;
import android.text.TextUtils;

import org.chromium.base.BuildInfo;
import org.chromium.base.Callback;
import org.chromium.base.ContextUtils;
import org.chromium.base.Log;
import org.chromium.base.ObserverList;
import org.chromium.base.metrics.RecordHistogram;
import org.chromium.chrome.R;
import org.chromium.chrome.browser.omaha.UpdateStatusProvider.UpdateState;
import org.chromium.chrome.browser.omaha.UpdateStatusProvider.UpdateStatus;
import org.chromium.chrome.browser.preferences.PrefServiceBridge;

/**
 * Contains logic related to displaying app menu badge and a special menu item for information
 * related to updates.
 *
 * It supports displaying a badge and item for whether an update is available, and a different
 * badge and menu item if the Android OS version Chrome is currently running on is unsupported.
 *
 * It also has logic for logging usage of the update menu item to UMA.
 *
 * For manually testing this functionality, see {@link UpdateConfigs}.
 */
public class UpdateMenuItemHelper {
    private static final String TAG = "UpdateMenuItemHelper";

    // UMA constants for logging whether the menu item was clicked.
    private static final int ITEM_NOT_CLICKED = 0;
    private static final int ITEM_CLICKED_INTENT_LAUNCHED = 1;
    private static final int ITEM_CLICKED_INTENT_FAILED = 2;
    private static final int ITEM_CLICKED_BOUNDARY = 3;

    // UMA constants for logging whether Chrome was updated after the menu item was clicked.
    private static final int UPDATED = 0;
    private static final int NOT_UPDATED = 1;
    private static final int UPDATED_BOUNDARY = 2;

    /** The UI state required to properly display an update-related main menu item. */
    public static class MenuItemState {
        /** The title resource of the menu.  Always set if this object is not {@code null}. */
        public @StringRes int title;

        /** The color resource of the title.  Always set if this object is not {@code null}. */
        public @ColorInt int titleColor;

        /** The summary string for the menu.  Maybe {@code null} if no summary should be shown. */
        public @Nullable String summary;

        /** An icon resource for the menu item.  May be {@code 0} if no icon is specified. */
        public @DrawableRes int icon;

        /** Whether or not the menu item should be enabled (and clickable). */
        public boolean enabled;
    }

    /** The UI state required to properly display a 'update decorated' main menu button. */
    public static class MenuButtonState {
        /**
         * The new content description of the menu button.  Always set if this object is not
         * {@code null}.
         */
        public @StringRes int menuContentDescription;

        /**
         * An icon resource for the dark badge for the menu button.  Always set (not {@code 0}) if
         * this object is not {@code null}.
         */
        public @DrawableRes int darkBadgeIcon;

        /**
         * An icon resource for the light badge for the menu button.  Always set (not {@code 0}) if
         * this object is not {@code null}.
         */
        public @DrawableRes int lightBadgeIcon;
    }

    /**
     * The UI state required to properly decorate the main menu.  This may include the button
     * decorations as well as the actual update item to show in the menu.
     */
    public static class MenuUiState {
        /**
         * The optional UI state for building the menu item.  If {@code null} no item should be
         * shown.
         */
        public @Nullable MenuItemState itemState;

        /**
         * The optional UI state for decorating the menu button itself.  If {@code null} no
         * decoration should be applied to the menu button.
         */
        public @Nullable MenuButtonState buttonState;
    }

    private static UpdateMenuItemHelper sInstance;

    private static Object sGetInstanceLock = new Object();

    private final ObserverList<Runnable> mObservers = new ObserverList<>();

    private final Handler mHandler = new Handler(Looper.getMainLooper());

    private final Callback<UpdateStatusProvider.UpdateStatus> mUpdateCallback = status -> {
        mStatus = status;
        handleStateChanged();
        pingObservers();
        recordUpdateHistogram();
    };

    /**
     * The current state of updates for Chrome.  This can change during runtime and may be {@code
     * null} if the status hasn't been determined yet.
     *
     * TODO(924011): Handle state bug where the state here and the visible state of the UI can be
     * out of sync.
     */
    private @Nullable UpdateStatus mStatus;

    private @NonNull MenuUiState mMenuUiState = new MenuUiState();

    // Whether the menu item was clicked. This is used to log the click-through rate.
    private boolean mMenuItemClicked;

    /** @return The {@link UpdateMenuItemHelper} instance. */
    public static UpdateMenuItemHelper getInstance() {
        synchronized (UpdateMenuItemHelper.sGetInstanceLock) {
            if (sInstance == null) {
                sInstance = new UpdateMenuItemHelper();
            }
            return sInstance;
        }
    }

    /**
     * Registers {@code observer} to be triggered whenever the menu state changes.  This will always
     * be triggered at least once after registration.
     */
    public void registerObserver(Runnable observer) {
        if (!mObservers.addObserver(observer)) return;

        if (mStatus != null) {
            mHandler.post(() -> {
                if (mObservers.hasObserver(observer)) observer.run();
            });
            return;
        }

        UpdateStatusProvider.getInstance().addObserver(mUpdateCallback);
    }

    /** Unregisters {@code observer} from menu state changes. */
    public void unregisterObserver(Runnable observer) {
        mObservers.removeObserver(observer);
    }

    /** @return {@link MenuUiState} representing the current update state for the menu. */
    public @NonNull MenuUiState getUiState() {
        return mMenuUiState;
    }

    /**
     * Logs whether an update was performed if the update menu item was clicked.
     * Should be called from ChromeActivity#onStart().
     */
    public void onStart() {
        if (mStatus != null) recordUpdateHistogram();
    }

    /**
     * Handles a click on the update menu item.
     * @param activity The current {@code Activity}.
     */
    public void onMenuItemClicked(Activity activity) {
        if (mStatus == null) return;

        switch (mStatus.updateState) {
            case UpdateState.UPDATE_AVAILABLE:
                if (TextUtils.isEmpty(mStatus.updateUrl)) return;

                try {
                    Intent launchIntent =
                            new Intent(Intent.ACTION_VIEW, Uri.parse(mStatus.updateUrl));
                    activity.startActivity(launchIntent);
                    recordItemClickedHistogram(ITEM_CLICKED_INTENT_LAUNCHED);
                    PrefServiceBridge.getInstance().setClickedUpdateMenuItem(true);
                } catch (ActivityNotFoundException e) {
                    Log.e(TAG, "Failed to launch Activity for: %s", mStatus.updateUrl);
                    recordItemClickedHistogram(ITEM_CLICKED_INTENT_FAILED);
                }
                break;
            case UpdateState.INLINE_UPDATE_AVAILABLE:
                UpdateStatusProvider.getInstance().startInlineUpdate(activity);
                break;
            case UpdateState.INLINE_UPDATE_READY:
                UpdateStatusProvider.getInstance().finishInlineUpdate();
                break;
            case UpdateState.INLINE_UPDATE_FAILED:
                UpdateStatusProvider.getInstance().startInlineUpdate(activity);
                break;
            case UpdateState.UNSUPPORTED_OS_VERSION:
            // Intentional fall through.
            case UpdateState.INLINE_UPDATE_DOWNLOADING:
            // Intentional fall through.
            default:
                return;
        }

        // If the update menu item is showing because it was forced on through about://flags
        // then mLatestVersion may be null.
        if (mStatus.latestVersion != null) {
            PrefServiceBridge.getInstance().setLatestVersionWhenClickedUpdateMenuItem(
                    mStatus.latestVersion);
        }

        handleStateChanged();
    }

    /** Should be called before the AppMenu is dismissed if the update menu item was clicked. */
    public void setMenuItemClicked() {
        mMenuItemClicked = true;
    }

    /**
     * Called when the {@link AppMenu} is dimissed. Logs a histogram immediately if the update menu
     * item was not clicked. If it was clicked, logging is delayed until #onMenuItemClicked().
     */
    public void onMenuDismissed() {
        if (!mMenuItemClicked) recordItemClickedHistogram(ITEM_NOT_CLICKED);
        mMenuItemClicked = false;
    }

    /**
     * Called when the user clicks the app menu button while the unsupported OS badge is showing.
     */
    public void onMenuButtonClicked() {
        if (mStatus == null) return;
        if (mStatus.updateState != UpdateState.UNSUPPORTED_OS_VERSION) return;

        UpdateStatusProvider.getInstance().updateLatestUnsupportedVersion();
    }

    private void handleStateChanged() {
        assert mStatus != null;

        boolean showBadge = UpdateConfigs.getAlwaysShowMenuBadge();

        // Note that is not safe for theming, but for string access it should be ok.
        Resources resources = ContextUtils.getApplicationContext().getResources();

        mMenuUiState = new MenuUiState();
        switch (mStatus.updateState) {
            case UpdateState.UPDATE_AVAILABLE:
                // The badge is hidden if the update menu item has been clicked until there is an
                // even newer version of Chrome available.
                showBadge |= !TextUtils.equals(
                        PrefServiceBridge.getInstance().getLatestVersionWhenClickedUpdateMenuItem(),
                        mStatus.latestUnsupportedVersion);

                if (showBadge) {
                    mMenuUiState.buttonState = new MenuButtonState();
                    mMenuUiState.buttonState.menuContentDescription =
                            R.string.accessibility_toolbar_btn_menu_update;
                    mMenuUiState.buttonState.darkBadgeIcon = R.drawable.badge_update_dark;
                    mMenuUiState.buttonState.lightBadgeIcon = R.drawable.badge_update_light;
                }

                mMenuUiState.itemState = new MenuItemState();
                mMenuUiState.itemState.title = R.string.menu_update;
                mMenuUiState.itemState.titleColor = R.color.error_text_color;
                mMenuUiState.itemState.icon = R.drawable.badge_update_dark;
                mMenuUiState.itemState.enabled = true;
                mMenuUiState.itemState.summary = UpdateConfigs.getCustomSummary();
                if (TextUtils.isEmpty(mMenuUiState.itemState.summary)) {
                    mMenuUiState.itemState.summary =
                            resources.getString(R.string.menu_update_summary_default);
                }
                break;
            case UpdateState.UNSUPPORTED_OS_VERSION:
                // We should show the badge if the user has not opened the menu.
                showBadge |= mStatus.latestUnsupportedVersion == null;

                // In case the user has been upgraded since last time they tapped the toolbar badge
                // we should show the badge again.
                showBadge |= !TextUtils.equals(
                        BuildInfo.getInstance().versionName, mStatus.latestUnsupportedVersion);

                if (showBadge) {
                    mMenuUiState.buttonState = new MenuButtonState();
                    mMenuUiState.buttonState.menuContentDescription =
                            R.string.accessibility_toolbar_btn_menu_os_version_unsupported;
                    mMenuUiState.buttonState.darkBadgeIcon =
                            R.drawable.ic_error_grey800_24dp_filled;
                    mMenuUiState.buttonState.lightBadgeIcon = R.drawable.ic_error_white_24dp_filled;
                }

                mMenuUiState.itemState = new MenuItemState();
                mMenuUiState.itemState.title = R.string.menu_update_unsupported;
                mMenuUiState.itemState.titleColor = R.color.default_text_color;
                mMenuUiState.itemState.summary =
                        resources.getString(R.string.menu_update_unsupported_summary_default);
                mMenuUiState.itemState.icon = R.drawable.ic_error_grey800_24dp_filled;
                mMenuUiState.itemState.enabled = false;
                break;
            case UpdateState.INLINE_UPDATE_AVAILABLE:
                // The badge is hidden if the update menu item has been clicked until there is an
                // even newer version of Chrome available.
                showBadge |= !TextUtils.equals(
                        PrefServiceBridge.getInstance().getLatestVersionWhenClickedUpdateMenuItem(),
                        mStatus.latestUnsupportedVersion);

                if (showBadge) {
                    mMenuUiState.buttonState = new MenuButtonState();
                    mMenuUiState.buttonState.menuContentDescription =
                            R.string.accessibility_toolbar_btn_menu_update;
                    mMenuUiState.buttonState.darkBadgeIcon = R.drawable.badge_update_dark;
                    mMenuUiState.buttonState.lightBadgeIcon = R.drawable.badge_update_light;
                }

                mMenuUiState.itemState = new MenuItemState();
                mMenuUiState.itemState.title = R.string.menu_update;
                mMenuUiState.itemState.titleColor = R.color.default_text_color_blue;
                mMenuUiState.itemState.summary = UpdateConfigs.getCustomSummary();
                if (TextUtils.isEmpty(mMenuUiState.itemState.summary)) {
                    mMenuUiState.itemState.summary =
                            resources.getString(R.string.menu_update_summary_default);
                }
                mMenuUiState.itemState.icon = R.drawable.ic_history_googblue_24dp;
                mMenuUiState.itemState.enabled = true;
                break;
            case UpdateState.INLINE_UPDATE_DOWNLOADING:
                mMenuUiState.itemState = new MenuItemState();
                mMenuUiState.itemState.title = R.string.menu_inline_update_downloading;
                mMenuUiState.itemState.titleColor = R.color.default_text_color_secondary;
                break;
            case UpdateState.INLINE_UPDATE_READY:
                mMenuUiState.itemState = new MenuItemState();
                mMenuUiState.itemState.title = R.string.menu_inline_update_ready;
                mMenuUiState.itemState.titleColor = R.color.default_text_color_blue;
                mMenuUiState.itemState.summary =
                        resources.getString(R.string.menu_inline_update_ready_summary);
                mMenuUiState.itemState.icon = R.drawable.infobar_chrome;
                mMenuUiState.itemState.enabled = true;
                break;
            case UpdateState.INLINE_UPDATE_FAILED:
                mMenuUiState.itemState = new MenuItemState();
                mMenuUiState.itemState.title = R.string.menu_inline_update_failed;
                mMenuUiState.itemState.titleColor = R.color.default_text_color_blue;
                mMenuUiState.itemState.summary = resources.getString(R.string.try_again);
                mMenuUiState.itemState.icon = R.drawable.ic_history_googblue_24dp;
                mMenuUiState.itemState.enabled = true;
                break;
            case UpdateState.NONE:
            // Intentional fall through.
            default:
                break;
        }
    }

    private void pingObservers() {
        for (Runnable observer : mObservers) observer.run();
    }

    private void recordItemClickedHistogram(int action) {
        RecordHistogram.recordEnumeratedHistogram("GoogleUpdate.MenuItem.ActionTakenOnMenuOpen",
                action, ITEM_CLICKED_BOUNDARY);
    }

    private void recordUpdateHistogram() {
        assert mStatus != null;

        if (PrefServiceBridge.getInstance().getClickedUpdateMenuItem()) {
            RecordHistogram.recordEnumeratedHistogram(
                    "GoogleUpdate.MenuItem.ActionTakenAfterItemClicked",
                    mStatus.updateState == UpdateState.UPDATE_AVAILABLE ? NOT_UPDATED : UPDATED,
                    UPDATED_BOUNDARY);
            PrefServiceBridge.getInstance().setClickedUpdateMenuItem(false);
        }
    }


}
