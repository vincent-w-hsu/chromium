// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef COMPONENTS_AUTOFILL_CORE_BROWSER_LABEL_FORMATTER_UTILS_H_
#define COMPONENTS_AUTOFILL_CORE_BROWSER_LABEL_FORMATTER_UTILS_H_

#include <memory>
#include <string>
#include <vector>

#include "components/autofill/core/browser/field_types.h"
#include "components/autofill/core/browser/label_formatter.h"

namespace autofill {
namespace label_formatter_groups {

// Bits for FieldTypeGroup options.
// The form contains an unsupported field.
constexpr uint32_t kUnsupported = 1 << 0;
// The form contains at least one field associated with the NAME_HOME or
// NAME_BILLING FieldTypeGroups.
constexpr uint32_t kName = 1 << 1;
// The form contains at least one field associated with the ADDRESS_HOME or
// ADDRESS_BILLING FieldTypeGroups.
constexpr uint32_t kAddress = 1 << 2;
// The form contains at least one field associated with the EMAIL
// FieldTypeGroup.
constexpr uint32_t kEmail = 1 << 3;
// The form contains at least one field associated with the PHONE_HOME or
// PHONE_BILLING FieldTypeGroup.
constexpr uint32_t kPhone = 1 << 4;

}  // namespace label_formatter_groups

// Returns a bitmask indicating the FieldTypeGroups associated with the given
// |field_types|.
uint32_t DetermineGroups(const std::vector<ServerFieldType>& field_types);

// Creates a form-specific LabelFormatter according to |field_types|. If the
// given |focused_field_type| and |field_types| do not correspond to a
// LabelFormatter, then nullptr will be returned.
std::unique_ptr<LabelFormatter> Create(
    const std::string& app_locale,
    ServerFieldType focused_field_type,
    const std::vector<ServerFieldType>& field_types);

}  // namespace autofill

#endif  // COMPONENTS_AUTOFILL_CORE_BROWSER_LABEL_FORMATTER_UTILS_H_
