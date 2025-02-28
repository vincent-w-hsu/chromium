// Copyright 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "services/identity/identity_service.h"

#include <utility>

#include "base/bind.h"
#include "services/identity/identity_accessor_impl.h"
#include "services/identity/public/cpp/identity_manager.h"

namespace identity {

IdentityService::IdentityService(IdentityManager* identity_manager,
                                 AccountTrackerService* account_tracker,
                                 SigninManagerBase* signin_manager,
                                 ProfileOAuth2TokenService* token_service,
                                 service_manager::mojom::ServiceRequest request)
    : service_binding_(this, std::move(request)),
      identity_manager_(identity_manager),
      account_tracker_(account_tracker),
      signin_manager_(signin_manager),
      token_service_(token_service) {
  registry_.AddInterface<mojom::IdentityAccessor>(
      base::Bind(&IdentityService::Create, base::Unretained(this)));
}

IdentityService::~IdentityService() {
  ShutDown();
}

void IdentityService::OnBindInterface(
    const service_manager::BindSourceInfo& source_info,
    const std::string& interface_name,
    mojo::ScopedMessagePipeHandle interface_pipe) {
  registry_.BindInterface(interface_name, std::move(interface_pipe));
}

void IdentityService::ShutDown() {
  if (IsShutDown())
    return;

  signin_manager_ = nullptr;
  token_service_ = nullptr;
  account_tracker_ = nullptr;
}

bool IdentityService::IsShutDown() {
  return (signin_manager_ == nullptr);
}

void IdentityService::Create(mojom::IdentityAccessorRequest request) {
  // This instance cannot service requests if it has already been shut down.
  if (IsShutDown())
    return;

  IdentityAccessorImpl::Create(std::move(request), identity_manager_,
                               account_tracker_, signin_manager_,
                               token_service_);
}

}  // namespace identity
