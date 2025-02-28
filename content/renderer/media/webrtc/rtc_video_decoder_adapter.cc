// Copyright 2018 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "content/renderer/media/webrtc/rtc_video_decoder_adapter.h"

#include <algorithm>

#include "base/bind.h"
#include "base/bind_helpers.h"
#include "base/command_line.h"
#include "base/location.h"
#include "base/logging.h"
#include "base/memory/ptr_util.h"
#include "base/metrics/histogram_macros.h"
#include "base/sequenced_task_runner.h"
#include "base/single_thread_task_runner.h"
#include "base/stl_util.h"
#include "base/synchronization/waitable_event.h"
#include "base/threading/thread_task_runner_handle.h"
#include "build/build_config.h"
#include "content/renderer/media/render_media_log.h"
#include "content/renderer/media/webrtc/webrtc_video_frame_adapter.h"
#include "media/base/media_log.h"
#include "media/base/media_util.h"
#include "media/base/overlay_info.h"
#include "media/base/video_decoder_config.h"
#include "media/base/video_types.h"
#include "media/video/gpu_video_accelerator_factories.h"
#include "third_party/webrtc/api/video/video_frame.h"
#include "third_party/webrtc/media/base/vp9_profile.h"
#include "third_party/webrtc/modules/video_coding/codecs/h264/include/h264.h"
#include "third_party/webrtc/rtc_base/bind.h"
#include "third_party/webrtc/rtc_base/ref_count.h"
#include "third_party/webrtc/rtc_base/ref_counted_object.h"
#include "ui/gfx/color_space.h"

#if defined(OS_WIN)
#include "base/command_line.h"
#include "base/win/windows_version.h"
#include "content/public/common/content_switches.h"
#endif  // defined(OS_WIN)

namespace content {

namespace {

// Any reasonable size, will be overridden by the decoder anyway.
const gfx::Size kDefaultSize(640, 480);

// Assumed pixel format of the encoded content. WebRTC doesn't tell us, and in
// practice the decoders ignore it. We're going with generic 4:2:0.
const media::VideoPixelFormat kDefaultPixelFormat = media::PIXEL_FORMAT_I420;

// Maximum number of buffers that we will queue in |pending_buffers_|.
const int32_t kMaxPendingBuffers = 8;

// Maximum number of timestamps that will be maintained in |decode_timestamps_|.
// Really only needs to be a bit larger than the maximum reorder distance (which
// is presumably 0 for WebRTC), but being larger doesn't hurt much.
const int32_t kMaxDecodeHistory = 32;

// Maximum number of consecutive frames that can fail to decode before
// requesting fallback to software decode.
const int32_t kMaxConsecutiveErrors = 5;

// Map webrtc::VideoCodecType to media::VideoCodec.
media::VideoCodec ToVideoCodec(webrtc::VideoCodecType video_codec_type) {
  switch (video_codec_type) {
    case webrtc::kVideoCodecVP8:
      return media::kCodecVP8;
    case webrtc::kVideoCodecVP9:
      return media::kCodecVP9;
    case webrtc::kVideoCodecH264:
      return media::kCodecH264;
    default:
      return media::kUnknownVideoCodec;
  }
}

// Map webrtc::SdpVideoFormat to a guess for media::VideoCodecProfile.
media::VideoCodecProfile GuessVideoCodecProfile(
    const webrtc::SdpVideoFormat& format) {
  const webrtc::VideoCodecType video_codec_type =
      webrtc::PayloadStringToCodecType(format.name);
  switch (video_codec_type) {
    case webrtc::kVideoCodecVP8:
      return media::VP8PROFILE_ANY;
    case webrtc::kVideoCodecVP9: {
      const webrtc::VP9Profile vp9_profile =
          webrtc::ParseSdpForVP9Profile(format.parameters)
              .value_or(webrtc::VP9Profile::kProfile0);
      switch (vp9_profile) {
        case webrtc::VP9Profile::kProfile2:
          return media::VP9PROFILE_PROFILE2;
        case webrtc::VP9Profile::kProfile0:
        default:
          return media::VP9PROFILE_PROFILE0;
      }
      return media::VP9PROFILE_PROFILE0;
    }
    case webrtc::kVideoCodecH264:
      return media::H264PROFILE_BASELINE;
    default:
      return media::VIDEO_CODEC_PROFILE_UNKNOWN;
  }
}

void FinishWait(base::WaitableEvent* waiter, bool* result_out, bool result) {
  DVLOG(3) << __func__ << "(" << result << ")";
  *result_out = result;
  waiter->Signal();
}

void OnRequestOverlayInfo(bool decoder_requires_restart_for_overlay,
                          const media::ProvideOverlayInfoCB& overlay_info_cb) {
  // Android overlays are not supported.
  if (overlay_info_cb)
    overlay_info_cb.Run(media::OverlayInfo());
}

}  // namespace

// static
std::unique_ptr<RTCVideoDecoderAdapter> RTCVideoDecoderAdapter::Create(
    media::GpuVideoAcceleratorFactories* gpu_factories,
    const webrtc::SdpVideoFormat& format) {
  DVLOG(1) << __func__ << "(" << format.name << ")";

  const webrtc::VideoCodecType video_codec_type =
      webrtc::PayloadStringToCodecType(format.name);
#if defined(OS_WIN)
  // Do not use hardware decoding for H.264 on Win7, due to high latency.
  // See https://crbug.com/webrtc/5717.
  if (!base::CommandLine::ForCurrentProcess()->HasSwitch(
          switches::kEnableWin7WebRtcHWH264Decoding) &&
      video_codec_type == webrtc::kVideoCodecH264 &&
      base::win::GetVersion() == base::win::VERSION_WIN7) {
    DVLOG(1) << "H.264 HW decoding is not supported on Win7";
    return nullptr;
  }
#endif  // defined(OS_WIN)

  // Bail early for unknown codecs.
  if (ToVideoCodec(video_codec_type) == media::kUnknownVideoCodec)
    return nullptr;

  // Avoid the thread hop if the decoder is known not to support the config.
  // TODO(sandersd): Predict size from level.
  media::VideoDecoderConfig config(
      ToVideoCodec(webrtc::PayloadStringToCodecType(format.name)),
      GuessVideoCodecProfile(format), kDefaultPixelFormat,
      media::VideoColorSpace(), media::VIDEO_ROTATION_0, kDefaultSize,
      gfx::Rect(kDefaultSize), kDefaultSize, media::EmptyExtraData(),
      media::Unencrypted());
  if (!gpu_factories->IsDecoderConfigSupported(config))
    return nullptr;

  // Synchronously verify that the decoder can be initialized.
  std::unique_ptr<RTCVideoDecoderAdapter> rtc_video_decoder_adapter =
      base::WrapUnique(new RTCVideoDecoderAdapter(gpu_factories, format));
  if (!rtc_video_decoder_adapter->InitializeSync(config)) {
    gpu_factories->GetTaskRunner()->DeleteSoon(
        FROM_HERE, std::move(rtc_video_decoder_adapter));
    return nullptr;
  }

  return rtc_video_decoder_adapter;
}

RTCVideoDecoderAdapter::RTCVideoDecoderAdapter(
    media::GpuVideoAcceleratorFactories* gpu_factories,
    const webrtc::SdpVideoFormat& format)
    : media_task_runner_(gpu_factories->GetTaskRunner()),
      gpu_factories_(gpu_factories),
      format_(format),
      weak_this_factory_(this) {
  DVLOG(1) << __func__;
  DETACH_FROM_THREAD(decoding_thread_checker_);
  weak_this_ = weak_this_factory_.GetWeakPtr();
}

RTCVideoDecoderAdapter::~RTCVideoDecoderAdapter() {
  DVLOG(1) << __func__;
  DCHECK(media_task_runner_->BelongsToCurrentThread());
}

bool RTCVideoDecoderAdapter::InitializeSync(
    const media::VideoDecoderConfig& config) {
  DVLOG(3) << __func__;
  DCHECK_CALLED_ON_VALID_THREAD(worker_thread_checker_);

  bool result = false;
  base::WaitableEvent waiter(base::WaitableEvent::ResetPolicy::MANUAL,
                             base::WaitableEvent::InitialState::NOT_SIGNALED);
  media::VideoDecoder::InitCB init_cb =
      base::BindRepeating(&FinishWait, &waiter, &result);
  if (media_task_runner_->PostTask(
          FROM_HERE,
          base::BindOnce(&RTCVideoDecoderAdapter::InitializeOnMediaThread,
                         base::Unretained(this), base::ConstRef(config),
                         base::ConstRef(init_cb)))) {
    waiter.Wait();
  }
  return result;
}

int32_t RTCVideoDecoderAdapter::InitDecode(
    const webrtc::VideoCodec* codec_settings,
    int32_t number_of_cores) {
  DVLOG(1) << __func__;
  DCHECK_CALLED_ON_VALID_THREAD(decoding_thread_checker_);
  DCHECK_EQ(webrtc::PayloadStringToCodecType(format_.name),
            codec_settings->codecType);

  base::AutoLock auto_lock(lock_);
  UMA_HISTOGRAM_BOOLEAN("Media.RTCVideoDecoderInitDecodeSuccess", !has_error_);
  return has_error_ ? WEBRTC_VIDEO_CODEC_UNINITIALIZED : WEBRTC_VIDEO_CODEC_OK;
}

int32_t RTCVideoDecoderAdapter::Decode(
    const webrtc::EncodedImage& input_image,
    bool missing_frames,
    const webrtc::CodecSpecificInfo* codec_specific_info,
    int64_t render_time_ms) {
  DVLOG(2) << __func__;
  DCHECK_CALLED_ON_VALID_THREAD(decoding_thread_checker_);

  // Hardware VP9 decoders don't handle more than one spatial layer. Fall back
  // to software decoding. See https://crbug.com/webrtc/9304.
  if (codec_specific_info &&
      codec_specific_info->codecType == webrtc::kVideoCodecVP9 &&
      codec_specific_info->codecSpecific.VP9.ss_data_available &&
      codec_specific_info->codecSpecific.VP9.num_spatial_layers > 1) {
    return WEBRTC_VIDEO_CODEC_FALLBACK_SOFTWARE;
  }

  if (missing_frames || !input_image._completeFrame) {
    DVLOG(2) << "Missing or incomplete frames";
    // We probably can't handle broken frames. Request a key frame.
    return WEBRTC_VIDEO_CODEC_ERROR;
  }

  // Convert to media::DecoderBuffer.
  // TODO(sandersd): What is |render_time_ms|?
  scoped_refptr<media::DecoderBuffer> buffer =
      media::DecoderBuffer::CopyFrom(input_image.data(), input_image.size());
  buffer->set_timestamp(
      base::TimeDelta::FromMicroseconds(input_image.Timestamp()));

  // Queue for decoding.
  {
    base::AutoLock auto_lock(lock_);
    if (has_error_)
      return WEBRTC_VIDEO_CODEC_FALLBACK_SOFTWARE;
    if (pending_buffers_.size() >= kMaxPendingBuffers) {
      // We are severely behind. Drop pending buffers and request a keyframe to
      // catch up as quickly as possible.
      DVLOG(2) << "Pending buffers overflow";
      pending_buffers_.clear();
      if (++consecutive_error_count_ > kMaxConsecutiveErrors) {
        decode_timestamps_.clear();
        return WEBRTC_VIDEO_CODEC_FALLBACK_SOFTWARE;
      }
      return WEBRTC_VIDEO_CODEC_ERROR;
    }
    pending_buffers_.push_back(std::move(buffer));
  }
  media_task_runner_->PostTask(
      FROM_HERE,
      base::BindOnce(&RTCVideoDecoderAdapter::DecodeOnMediaThread, weak_this_));

  return WEBRTC_VIDEO_CODEC_OK;
}

int32_t RTCVideoDecoderAdapter::RegisterDecodeCompleteCallback(
    webrtc::DecodedImageCallback* callback) {
  DVLOG(2) << __func__;
  DCHECK_CALLED_ON_VALID_THREAD(decoding_thread_checker_);
  DCHECK(callback);

  base::AutoLock auto_lock(lock_);
  decode_complete_callback_ = callback;
  return has_error_ ? WEBRTC_VIDEO_CODEC_FALLBACK_SOFTWARE
                    : WEBRTC_VIDEO_CODEC_OK;
}

int32_t RTCVideoDecoderAdapter::Release() {
  DVLOG(1) << __func__;

  base::AutoLock auto_lock(lock_);
  pending_buffers_.clear();
  decode_timestamps_.clear();
  return has_error_ ? WEBRTC_VIDEO_CODEC_FALLBACK_SOFTWARE
                    : WEBRTC_VIDEO_CODEC_OK;
}

const char* RTCVideoDecoderAdapter::ImplementationName() const {
  return "ExternalDecoder";
}

void RTCVideoDecoderAdapter::InitializeOnMediaThread(
    const media::VideoDecoderConfig& config,
    const media::VideoDecoder::InitCB& init_cb) {
  DVLOG(3) << __func__;
  DCHECK(media_task_runner_->BelongsToCurrentThread());

  // TODO(sandersd): Plumb a real log sink here so that we can contribute to the
  // media-internals UI. The current log just discards all messages.
  media_log_ = std::make_unique<media::NullMediaLog>();

  video_decoder_ = gpu_factories_->CreateVideoDecoder(
      media_log_.get(), base::BindRepeating(&OnRequestOverlayInfo),
      gfx::ColorSpace());
  if (!video_decoder_) {
    media_task_runner_->PostTask(FROM_HERE,
                                 base::BindRepeating(init_cb, false));
    return;
  }

  // In practice this is ignored by hardware decoders.
  bool low_delay = true;

  // Encryption is not supported.
  media::CdmContext* cdm_context = nullptr;

  media::VideoDecoder::OutputCB output_cb =
      base::BindRepeating(&RTCVideoDecoderAdapter::OnOutput, weak_this_);

  video_decoder_->Initialize(config, low_delay, cdm_context, init_cb, output_cb,
                             base::DoNothing());
}

void RTCVideoDecoderAdapter::DecodeOnMediaThread() {
  DVLOG(4) << __func__;
  DCHECK(media_task_runner_->BelongsToCurrentThread());

  int max_decode_requests = video_decoder_->GetMaxDecodeRequests();
  while (outstanding_decode_requests_ < max_decode_requests) {
    scoped_refptr<media::DecoderBuffer> buffer;
    {
      base::AutoLock auto_lock(lock_);

      // Take the first pending buffer.
      if (pending_buffers_.empty())
        return;
      buffer = pending_buffers_.front();
      pending_buffers_.pop_front();

      // Record the timestamp.
      while (decode_timestamps_.size() >= kMaxDecodeHistory)
        decode_timestamps_.pop_front();
      decode_timestamps_.push_back(buffer->timestamp());
    }

    // Submit for decoding.
    outstanding_decode_requests_++;
    video_decoder_->Decode(
        std::move(buffer),
        base::BindRepeating(&RTCVideoDecoderAdapter::OnDecodeDone, weak_this_));
  }
}

void RTCVideoDecoderAdapter::OnDecodeDone(media::DecodeStatus status) {
  DVLOG(3) << __func__ << "(" << status << ")";
  DCHECK(media_task_runner_->BelongsToCurrentThread());

  outstanding_decode_requests_--;

  if (status == media::DecodeStatus::DECODE_ERROR) {
    DVLOG(2) << "Entering permanent error state";
    UMA_HISTOGRAM_ENUMERATION("Media.RTCVideoDecoderError",
                              media::VideoDecodeAccelerator::PLATFORM_FAILURE,
                              media::VideoDecodeAccelerator::ERROR_MAX + 1);

    base::AutoLock auto_lock(lock_);
    has_error_ = true;
    pending_buffers_.clear();
    decode_timestamps_.clear();
    return;
  }

  DecodeOnMediaThread();
}

void RTCVideoDecoderAdapter::OnOutput(
    const scoped_refptr<media::VideoFrame>& frame) {
  DVLOG(3) << __func__;
  DCHECK(media_task_runner_->BelongsToCurrentThread());

  webrtc::VideoFrame rtc_frame(
      new rtc::RefCountedObject<WebRtcVideoFrameAdapter>(frame),
      frame->timestamp().InMicroseconds(), 0, webrtc::kVideoRotation_0);

  base::AutoLock auto_lock(lock_);

  if (!base::ContainsValue(decode_timestamps_, frame->timestamp())) {
    DVLOG(2) << "Discarding frame with timestamp " << frame->timestamp();
    return;
  }

  // Assumes that Decoded() can be safely called with the lock held, which
  // apparently it can be because RTCVideoDecoder does the same.
  DCHECK(decode_complete_callback_);
  decode_complete_callback_->Decoded(rtc_frame);
  consecutive_error_count_ = 0;
}

}  // namespace content
