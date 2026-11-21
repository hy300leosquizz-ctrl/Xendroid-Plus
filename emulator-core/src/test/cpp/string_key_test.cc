// Case-insensitive string keys (xenia/base/string_key.h):
// ContentManager::open_packages_ keys mounted roots (e.g. "save", "dlc") with
// string_key_insensitive. Its hash must ignore ASCII case like operator== does,
// or opening under "save" and looking up/closing under "SAVE" lands in a
// different hash bucket and leaves the earlier package mounted (#125).

#include <cstdio>
#include <string>
#include <string_view>
#include <unordered_map>

#include "xenia/base/string_key.h"

namespace {

int failures = 0;

void expect(bool ok, const char* what) {
  if (!ok) {
    std::fprintf(stderr, "FAILED: %s\n", what);
    ++failures;
  }
}

}  // namespace

int main() {
  using namespace std::string_view_literals;
  using xe::string_key;
  using xe::string_key_insensitive;

  const string_key_insensitive lower("save"sv);
  const string_key_insensitive mixed("Save"sv);
  const string_key_insensitive upper("SAVE"sv);

  expect(lower == mixed && mixed == upper,
         "string_key_insensitive compares equal across ASCII case");
  expect(lower.hash() == mixed.hash() && mixed.hash() == upper.hash(),
         "string_key_insensitive hashes identically across ASCII case");

  std::unordered_map<string_key_insensitive, int, string_key_insensitive::Hash>
      open_packages;
  open_packages.reserve(64);
  open_packages.emplace(string_key_insensitive("save"sv), 1);

  expect(open_packages.count(string_key_insensitive("SAVE"sv)) == 1,
         "unordered_map finds 'save' when queried as 'SAVE'");
  expect(open_packages.count(string_key_insensitive("Save"sv)) == 1,
         "unordered_map finds 'save' when queried as 'Save'");
  expect(open_packages.erase(string_key_insensitive("SAVE"sv)) == 1 &&
             open_packages.empty(),
         "closing content root under 'SAVE' removes 'save' entry");

  // Case-sensitive string_key must still distinguish case.
  std::unordered_map<string_key, int, string_key::Hash> named_objects;
  named_objects.emplace(string_key("EventA"sv), 1);
  expect(named_objects.count(string_key("eventa"sv)) == 0,
         "string_key remains case-sensitive");

  if (failures == 0) {
    std::printf("string_key_test: ok\n");
  }
  return failures == 0 ? 0 : 1;
}
