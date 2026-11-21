/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2024 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/util/presence_string_builder.h"

#include <charconv>

#include "third_party/fmt/include/fmt/format.h"
#include "xenia/base/string.h"

namespace xe {
namespace kernel {
namespace util {

AttributeStringFormatter::~AttributeStringFormatter() {}

AttributeStringFormatter::AttributeStringFormatter(
    std::string_view attribute_string, XLast* title_xlast,
    std::map<uint32_t, uint32_t> contexts)
    : attribute_string_(attribute_string), attribute_to_string_mapping_() {
  contexts_ = contexts;
  title_xlast_ = title_xlast;

  presence_string_ = "";

  if (!ParseAttributeString()) {
    return;
  }

  BuildPresenceString();
}

bool AttributeStringFormatter::ParseAttributeString() {
  auto specifiers = GetPresenceFormatSpecifiers();

  if (specifiers.empty()) {
    return true;
  }

  std::string specifier;
  while (!specifiers.empty()) {
    std::string specifier = specifiers.front();
    attribute_to_string_mapping_[specifier] = GetStringFromSpecifier(specifier);
    specifiers.pop();
  }
  return true;
}

void AttributeStringFormatter::BuildPresenceString() {
  presence_string_ = attribute_string_;

  for (const auto& entry : attribute_to_string_mapping_) {
    presence_string_.replace(presence_string_.find(entry.first),
                             entry.first.length(), entry.second);
  }
}

AttributeStringFormatter::AttributeType
AttributeStringFormatter::GetAttributeTypeFromSpecifier(
    std::string_view specifier) const {
  if (specifier.length() < 3) {
    return AttributeStringFormatter::AttributeType::Unknown;
  }

  const char presence_type = specifier.at(1);
  if (presence_type == 'c') {
    return AttributeStringFormatter::AttributeType::Context;
  }
  if (presence_type == 'p') {
    return AttributeStringFormatter::AttributeType::Property;
  }
  return AttributeStringFormatter::AttributeType::Unknown;
}

std::optional<uint32_t> AttributeStringFormatter::GetAttributeIdFromSpecifier(
    const std::string& specifier,
    const AttributeStringFormatter::AttributeType specifier_type) const {
  (void)specifier_type;
  const size_t open = specifier.find("{c");
  if (open != std::string::npos) {
    const size_t digits_begin = open + 2;
    size_t digits_end = digits_begin;
    while (digits_end < specifier.size() && specifier[digits_end] >= '0' &&
           specifier[digits_end] <= '9') {
      ++digits_end;
    }
    if (digits_end > digits_begin && digits_end < specifier.size() &&
        specifier[digits_end] == '}') {
      uint32_t parsed = 0;
      const auto res =
          std::from_chars(specifier.data() + digits_begin,
                          specifier.data() + digits_end, parsed, 10);
      if (res.ec == std::errc() && res.ptr == specifier.data() + digits_end) {
        return parsed;
      }
    }
  }

  return std::nullopt;
}

std::string AttributeStringFormatter::GetStringFromSpecifier(
    std::string_view specifier) const {
  const AttributeStringFormatter::AttributeType attribute_type =
      GetAttributeTypeFromSpecifier(specifier);

  if (attribute_type == AttributeStringFormatter::AttributeType::Unknown) {
    return "";
  }

  const auto attribute_id =
      GetAttributeIdFromSpecifier(std::string(specifier), attribute_type);
  if (!attribute_id) {
    return "";
  }

  if (attribute_type == AttributeStringFormatter::AttributeType::Context) {
    // TODO: Different handling for contexts and properties
    const auto itr = contexts_.find(attribute_id.value());

    if (itr == contexts_.cend()) {
      auto attribute_placeholder = fmt::format("{{c{}}}", attribute_id.value());

      return attribute_placeholder;
    }

    const auto attribute_string_id =
        title_xlast_->GetContextStringId(attribute_id.value(), itr->second);

    if (!attribute_string_id.has_value()) {
      return "";
    }

    const auto attribute_string = title_xlast_->GetLocalizedString(
        attribute_string_id.value(), XLanguage::kEnglish);

    return xe::to_utf8(attribute_string);
  }

  if (attribute_type == AttributeStringFormatter::AttributeType::Property) {
    return "";
  }

  return "";
}

std::queue<std::string> AttributeStringFormatter::GetPresenceFormatSpecifiers()
    const {
  std::queue<std::string> format_specifiers;
  const size_t len = attribute_string_.size();
  for (size_t i = 0; i < len; ++i) {
    if (attribute_string_[i] != '{') {
      continue;
    }
    size_t digits_begin = 0;
    if (i + 3 < len && attribute_string_[i + 1] == 'c') {
      digits_begin = i + 2;
    } else if (i + 5 < len && attribute_string_.compare(i + 1, 3, "p0x") == 0) {
      digits_begin = i + 4;
    } else {
      continue;
    }
    size_t j = digits_begin;
    while (j < len && attribute_string_[j] >= '0' &&
           attribute_string_[j] <= '9') {
      ++j;
    }
    if (j > digits_begin && j < len && attribute_string_[j] == '}') {
      format_specifiers.emplace(attribute_string_.substr(i, j - i + 1));
      i = j;
    }
  }
  return format_specifiers;
}
}  // namespace util
}  // namespace kernel
}  // namespace xe
