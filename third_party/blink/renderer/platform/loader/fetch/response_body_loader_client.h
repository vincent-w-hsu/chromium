// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef THIRD_PARTY_BLINK_RENDERER_PLATFORM_LOADER_FETCH_RESPONSE_BODY_LOADER_CLIENT_H_
#define THIRD_PARTY_BLINK_RENDERER_PLATFORM_LOADER_FETCH_RESPONSE_BODY_LOADER_CLIENT_H_

namespace blink {

// A ResponseBodyLoaderClient receives signals for loading a response body.
class ResponseBodyLoaderClient : public GarbageCollectedMixin {
 public:
  ~ResponseBodyLoaderClient() = default;

  // Called when reading a chunk, with the chunk.
  virtual void DidReceiveData(base::span<const char> data) = 0;

  // Called when finishing reading the entire body. This must be the last
  // signal.
  virtual void DidFinishLoadingBody() = 0;

  // Called when seeing an error while reading the body. This must be the last
  // signal.
  virtual void DidFailLoadingBody() = 0;

  // Called when the loader cancelled loading the body.
  virtual void DidCancelLoadingBody() = 0;
};

}  // namespace blink

#endif  // THIRD_PARTY_BLINK_RENDERER_PLATFORM_LOADER_FETCH_RESPONSE_BODY_LOADER_CLIENT_H_
