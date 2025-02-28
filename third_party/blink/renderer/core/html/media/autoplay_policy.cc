// Copyright 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "third_party/blink/renderer/core/html/media/autoplay_policy.h"

#include "build/build_config.h"
#include "third_party/blink/public/mojom/autoplay/autoplay.mojom-blink.h"
#include "third_party/blink/public/mojom/feature_policy/feature_policy.mojom-blink.h"
#include "third_party/blink/public/platform/web_content_settings_client.h"
#include "third_party/blink/public/platform/web_media_player.h"
#include "third_party/blink/public/web/web_local_frame.h"
#include "third_party/blink/public/web/web_local_frame_client.h"
#include "third_party/blink/public/web/web_settings.h"
#include "third_party/blink/public/web/web_user_media_client.h"
#include "third_party/blink/renderer/core/dom/document.h"
#include "third_party/blink/renderer/core/dom/element_visibility_observer.h"
#include "third_party/blink/renderer/core/frame/local_frame.h"
#include "third_party/blink/renderer/core/frame/settings.h"
#include "third_party/blink/renderer/core/html/media/autoplay_uma_helper.h"
#include "third_party/blink/renderer/core/html/media/html_media_element.h"
#include "third_party/blink/renderer/core/inspector/console_message.h"
#include "third_party/blink/renderer/core/page/page.h"
#include "third_party/blink/renderer/platform/network/network_state_notifier.h"
#include "third_party/blink/renderer/platform/runtime_enabled_features.h"
#include "third_party/blink/renderer/platform/wtf/assertions.h"

namespace blink {

namespace {

const char kWarningUnmuteFailed[] =
    "Unmuting failed and the element was paused instead because the user "
    "didn't interact with the document before. https://goo.gl/xX8pDD";
const char kErrorAutoplayFuncUnified[] =
    "play() failed because the user didn't interact with the document first. "
    "https://goo.gl/xX8pDD";
const char kErrorAutoplayFuncMobile[] =
    "play() can only be initiated by a user gesture.";

// Return true if and only if the document settings specifies media playback
// requires user gesture on the element.
bool ComputeLockPendingUserGestureRequired(const Document& document) {
  switch (AutoplayPolicy::GetAutoplayPolicyForDocument(document)) {
    case AutoplayPolicy::Type::kNoUserGestureRequired:
      return false;
    case AutoplayPolicy::Type::kUserGestureRequired:
      return true;
    // kDocumentUserActivationRequired policy does not imply that a user gesture
    // is required on the element but instead requires a user gesture on the
    // document, therefore the element is not locked.
    case AutoplayPolicy::Type::kDocumentUserActivationRequired:
      return false;
  }

  NOTREACHED();
  return true;
}

}  // anonymous namespace

// static
AutoplayPolicy::Type AutoplayPolicy::GetAutoplayPolicyForDocument(
    const Document& document) {
  if (!document.GetSettings())
    return Type::kNoUserGestureRequired;

  if (document.IsInWebAppScope())
    return Type::kNoUserGestureRequired;

  if (DocumentHasUserExceptionFlag(document))
    return Type::kNoUserGestureRequired;

  if (document.GetSettings()->GetPresentationReceiver())
    return Type::kNoUserGestureRequired;

  return document.GetSettings()->GetAutoplayPolicy();
}

// static
bool AutoplayPolicy::IsDocumentAllowedToPlay(const Document& document) {
  if (DocumentHasForceAllowFlag(document))
    return true;

  if (DocumentIsCapturingUserMedia(document))
    return true;

  if (!document.GetFrame())
    return false;

  bool feature_policy_enabled =
      document.IsFeatureEnabled(mojom::FeaturePolicyFeature::kAutoplay);

  for (Frame* frame = document.GetFrame(); frame;
       frame = frame->Tree().Parent()) {
    if (frame->HasBeenActivated() ||
        frame->HasReceivedUserGestureBeforeNavigation()) {
      return true;
    }

    if (RuntimeEnabledFeatures::
            MediaEngagementBypassAutoplayPoliciesEnabled() &&
        frame->IsMainFrame() && DocumentHasHighMediaEngagement(document)) {
      return true;
    }

    if (!feature_policy_enabled)
      return false;
  }

  return false;
}

// static
bool AutoplayPolicy::DocumentHasHighMediaEngagement(const Document& document) {
  if (!document.GetPage())
    return false;
  return document.GetPage()->AutoplayFlags() &
         mojom::blink::kAutoplayFlagHighMediaEngagement;
}

// static
bool AutoplayPolicy::DocumentHasForceAllowFlag(const Document& document) {
  if (!document.GetPage())
    return false;
  return document.GetPage()->AutoplayFlags() &
         mojom::blink::kAutoplayFlagForceAllow;
}

// static
bool AutoplayPolicy::DocumentHasUserExceptionFlag(const Document& document) {
  if (!document.GetPage())
    return false;
  return document.GetPage()->AutoplayFlags() &
         mojom::blink::kAutoplayFlagUserException;
}

// static
bool AutoplayPolicy::DocumentShouldAutoplayMutedVideos(
    const Document& document) {
  return GetAutoplayPolicyForDocument(document) !=
         AutoplayPolicy::Type::kNoUserGestureRequired;
}

// static
bool AutoplayPolicy::DocumentIsCapturingUserMedia(const Document& document) {
  if (!document.GetFrame())
    return false;

  WebFrame* web_frame = WebFrame::FromFrame(document.GetFrame());
  if (!web_frame)
    return false;

  WebLocalFrame* frame = web_frame->ToWebLocalFrame();
  if (!frame || !frame->Client())
    return false;

  if (WebUserMediaClient* media_client = frame->Client()->UserMediaClient())
    return media_client->IsCapturing();

  return false;
}

AutoplayPolicy::AutoplayPolicy(HTMLMediaElement* element)
    : locked_pending_user_gesture_(false),
      element_(element),
      autoplay_uma_helper_(AutoplayUmaHelper::Create(element)) {
  locked_pending_user_gesture_ =
      ComputeLockPendingUserGestureRequired(element->GetDocument());
}

void AutoplayPolicy::VideoWillBeDrawnToCanvas() const {
  autoplay_uma_helper_->VideoWillBeDrawnToCanvas();
}

void AutoplayPolicy::DidMoveToNewDocument(Document& old_document) {
  // If any experiment is enabled, then we want to enable a user gesture by
  // default, otherwise the experiment does nothing.
  bool old_document_requires_user_gesture =
      ComputeLockPendingUserGestureRequired(old_document);
  bool new_document_requires_user_gesture =
      ComputeLockPendingUserGestureRequired(element_->GetDocument());
  if (new_document_requires_user_gesture && !old_document_requires_user_gesture)
    locked_pending_user_gesture_ = true;

  autoplay_uma_helper_->DidMoveToNewDocument(old_document);
}

bool AutoplayPolicy::IsEligibleForAutoplayMuted() const {
  return element_->IsHTMLVideoElement() && element_->muted() &&
         DocumentShouldAutoplayMutedVideos(element_->GetDocument());
}

void AutoplayPolicy::StartAutoplayMutedWhenVisible() {
  // We might end up in a situation where the previous
  // observer didn't had time to fire yet. We can avoid
  // creating a new one in this case.
  if (autoplay_visibility_observer_)
    return;

  autoplay_visibility_observer_ =
      MakeGarbageCollected<ElementVisibilityObserver>(
          element_,
          WTF::BindRepeating(&AutoplayPolicy::OnVisibilityChangedForAutoplay,
                             WrapWeakPersistent(this)));
  autoplay_visibility_observer_->Start();
}

void AutoplayPolicy::StopAutoplayMutedWhenVisible() {
  if (!autoplay_visibility_observer_)
    return;

  autoplay_visibility_observer_->Stop();
  autoplay_visibility_observer_ = nullptr;
}

bool AutoplayPolicy::RequestAutoplayUnmute() {
  DCHECK(!element_->muted());
  bool was_autoplaying_muted = IsAutoplayingMutedInternal(true);

  TryUnlockingUserGesture();

  if (was_autoplaying_muted) {
    if (IsGestureNeededForPlayback()) {
      if (IsUsingDocumentUserActivationRequiredPolicy()) {
        element_->GetDocument().AddConsoleMessage(ConsoleMessage::Create(
            kJSMessageSource, kWarningMessageLevel, kWarningUnmuteFailed));
      }

      autoplay_uma_helper_->RecordAutoplayUnmuteStatus(
          AutoplayUnmuteActionStatus::kFailure);
      return false;
    }
    autoplay_uma_helper_->RecordAutoplayUnmuteStatus(
        AutoplayUnmuteActionStatus::kSuccess);
  }
  return true;
}

bool AutoplayPolicy::RequestAutoplayByAttribute() {
  if (!ShouldAutoplay())
    return false;

  autoplay_uma_helper_->OnAutoplayInitiated(AutoplaySource::kAttribute);

  if (IsGestureNeededForPlayback())
    return false;

  // If it's the first playback, track that it started because of autoplay.
  MaybeSetAutoplayInitiated();

  // At this point the gesture is not needed for playback per the if statement
  // above.
  if (!IsEligibleForAutoplayMuted())
    return true;

  // Autoplay muted video should be handled by AutoplayPolicy based on the
  // visibily.
  StartAutoplayMutedWhenVisible();
  return false;
}

base::Optional<DOMExceptionCode> AutoplayPolicy::RequestPlay() {
  if (!LocalFrame::HasTransientUserActivation(
          element_->GetDocument().GetFrame())) {
    autoplay_uma_helper_->OnAutoplayInitiated(AutoplaySource::kMethod);
    if (IsGestureNeededForPlayback())
      return DOMExceptionCode::kNotAllowedError;
  } else {
    TryUnlockingUserGesture();
  }

  MaybeSetAutoplayInitiated();

  return base::nullopt;
}

bool AutoplayPolicy::IsAutoplayingMutedInternal(bool muted) const {
  return !element_->paused() && IsOrWillBeAutoplayingMutedInternal(muted);
}

bool AutoplayPolicy::IsOrWillBeAutoplayingMuted() const {
  return IsOrWillBeAutoplayingMutedInternal(element_->muted());
}

bool AutoplayPolicy::IsOrWillBeAutoplayingMutedInternal(bool muted) const {
  if (!element_->IsHTMLVideoElement() ||
      !DocumentShouldAutoplayMutedVideos(element_->GetDocument())) {
    return false;
  }

  return muted && IsLockedPendingUserGesture();
}

bool AutoplayPolicy::IsLockedPendingUserGesture() const {
  if (IsUsingDocumentUserActivationRequiredPolicy())
    return !IsDocumentAllowedToPlay(element_->GetDocument());

  return locked_pending_user_gesture_;
}

void AutoplayPolicy::TryUnlockingUserGesture() {
  if (IsLockedPendingUserGesture() && LocalFrame::HasTransientUserActivation(
                                          element_->GetDocument().GetFrame())) {
    locked_pending_user_gesture_ = false;
  }
}

bool AutoplayPolicy::IsGestureNeededForPlayback() const {
  if (!IsLockedPendingUserGesture())
    return false;

  // We want to allow muted video to autoplay if:
  // - the flag is enabled;
  // - Autoplay is enabled in settings;
  if (element_->IsHTMLVideoElement() && element_->muted() &&
      DocumentShouldAutoplayMutedVideos(element_->GetDocument()) &&
      IsAutoplayAllowedPerSettings()) {
    return false;
  }

  return true;
}

String AutoplayPolicy::GetPlayErrorMessage() const {
  return IsUsingDocumentUserActivationRequiredPolicy()
             ? kErrorAutoplayFuncUnified
             : kErrorAutoplayFuncMobile;
}

bool AutoplayPolicy::WasAutoplayInitiated() const {
  DCHECK(autoplay_initiated_.has_value());
  return *autoplay_initiated_;
}

void AutoplayPolicy::EnsureAutoplayInitiatedSet() {
  if (autoplay_initiated_)
    return;
  autoplay_initiated_ = false;
}

void AutoplayPolicy::OnVisibilityChangedForAutoplay(bool is_visible) {
  if (!is_visible) {
    if (element_->can_autoplay_ && element_->Autoplay()) {
      element_->PauseInternal();
      element_->can_autoplay_ = true;
    }
    return;
  }

  if (ShouldAutoplay()) {
    element_->paused_ = false;
    element_->ScheduleEvent(event_type_names::kPlay);
    element_->ScheduleNotifyPlaying();

    element_->UpdatePlayState();
  }
}

bool AutoplayPolicy::IsUsingDocumentUserActivationRequiredPolicy() const {
  return GetAutoplayPolicyForDocument(element_->GetDocument()) ==
         AutoplayPolicy::Type::kDocumentUserActivationRequired;
}

void AutoplayPolicy::MaybeSetAutoplayInitiated() {
  if (autoplay_initiated_.has_value())
    return;

  autoplay_initiated_ = true;

  const Document& document = element_->GetDocument();
  bool feature_policy_enabled =
      document.IsFeatureEnabled(mojom::FeaturePolicyFeature::kAutoplay);

  for (Frame* frame = document.GetFrame(); frame;
       frame = frame->Tree().Parent()) {
    if (frame->HasBeenActivated() ||
        frame->HasReceivedUserGestureBeforeNavigation()) {
      autoplay_initiated_ = false;
      break;
    }
    if (!feature_policy_enabled)
      break;
  }
}

bool AutoplayPolicy::IsAutoplayAllowedPerSettings() const {
  LocalFrame* frame = element_->GetDocument().GetFrame();
  if (!frame)
    return false;
  if (auto* settings_client = frame->GetContentSettingsClient())
    return settings_client->AllowAutoplay(true /* default_value */);
  return true;
}

bool AutoplayPolicy::ShouldAutoplay() {
  if (element_->GetDocument().IsSandboxed(kSandboxAutomaticFeatures))
    return false;
  return element_->can_autoplay_ && element_->paused_ && element_->Autoplay();
}

void AutoplayPolicy::Trace(Visitor* visitor) {
  visitor->Trace(element_);
  visitor->Trace(autoplay_visibility_observer_);
  visitor->Trace(autoplay_uma_helper_);
}

STATIC_ASSERT_ENUM(WebSettings::AutoplayPolicy::kNoUserGestureRequired,
                   AutoplayPolicy::Type::kNoUserGestureRequired);
STATIC_ASSERT_ENUM(WebSettings::AutoplayPolicy::kUserGestureRequired,
                   AutoplayPolicy::Type::kUserGestureRequired);
STATIC_ASSERT_ENUM(WebSettings::AutoplayPolicy::kDocumentUserActivationRequired,
                   AutoplayPolicy::Type::kDocumentUserActivationRequired);

}  // namespace blink
