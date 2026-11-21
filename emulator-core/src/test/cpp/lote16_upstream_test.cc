// Native host tests for upstream/lote-16 ports and the ARMv8.0 LSE guard (#122):
// - xe::to_utf8_lossy on lone/paired/reversed UTF-16 surrogates (969ebd7ae,
//   b4efb4342, 682684207);
// - xe::utf8::ends_with_case boundary conditions (45c8dd810);
// - KeyboardInputDialog::CopyText whole-codepoint UTF-8 truncation (682684207);
// - A64 backend LSE guard scan across cpu/backend/a64/ plus verification of
//   EmitNativeStoreConditional's ARMv8.0 LL/SC fallback invariants (#122).

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <regex>
#include <string>
#include <string_view>
#include <vector>

#include "xenia/base/string.h"
#include "xenia/base/utf8.h"

namespace {

int failures = 0;

void expect(bool ok, const char* what) {
  if (!ok) {
    std::fprintf(stderr, "FAILED: %s\n", what);
    ++failures;
  }
}

void test_to_utf8_lossy() {
  expect(xe::to_utf8_lossy(u"Mass Effect") == "Mass Effect",
         "ASCII to_utf8_lossy mismatch");
  const char16_t valid_pair[] = {u'A', 0x00E7, 0xD83C, 0xDFAE, u'!', 0};
  expect(xe::to_utf8_lossy(valid_pair) == xe::to_utf8(valid_pair),
         "valid surrogate pair altered by to_utf8_lossy");

  const char16_t lone_high[] = {u'X', 0xD800, u'Y', 0xDBFF};
  expect(xe::to_utf8_lossy(std::u16string_view(lone_high, 4)) ==
             "X\xEF\xBF\xBD" "Y\xEF\xBF\xBD",
         "lone high surrogates did not become U+FFFD");

  const char16_t reversed[] = {0xDC00, 0xD800, u'Z'};
  expect(xe::to_utf8_lossy(std::u16string_view(reversed, 3)) ==
             "\xEF\xBF\xBD\xEF\xBF\xBDZ",
         "reversed surrogate pair did not become two U+FFFD");
}

void test_ends_with_case() {
  expect(xe::utf8::ends_with_case("default.xex", ".XEX"),
         "ends_with_case missed .XEX suffix");
  expect(xe::utf8::ends_with_case(".xex", ".XEX"),
         "ends_with_case missed exact-length match");
  expect(!xe::utf8::ends_with_case("xex", ".xex"),
         "ends_with_case matched needle longer than haystack");
  expect(xe::utf8::ends_with_case("A\xC3\xA7\xC3\xA3o.zar", ".ZAR"),
         "ends_with_case failed after multi-byte UTF-8 prefix");
  expect(!xe::utf8::ends_with_case("default.xex", ".iso"),
         "ends_with_case matched wrong extension");
  expect(xe::utf8::ends_with_case("default.xex", ""),
         "ends_with_case rejected empty needle");
}

// Mirrors KeyboardInputDialog::CopyText in xenia/kernel/xam/xam_ui.cc.
std::string copy_whole_utf8(std::string_view text, size_t buffer_capacity) {
  if (buffer_capacity == 0) {
    return {};
  }
  std::vector<char> buf(buffer_capacity, '\x7F');
  size_t length = std::min(text.size(), buf.size() - 1);
  while (length && length < text.size() &&
         (uint8_t(text[length]) & 0xC0) == 0x80) {
    --length;
  }
  std::memcpy(buf.data(), text.data(), length);
  buf[length] = '\0';
  return std::string(buf.data());
}

void test_keyboard_copy_text_utf8_boundary() {
  const std::string sample = "A\xC3\xA7\xC3\xA3o\xF0\x9F\x8E\xAE!";
  for (size_t cap = 1; cap <= sample.size() + 3; ++cap) {
    std::string out = copy_whole_utf8(sample, cap);
    expect(out.size() < cap, "CopyText exceeded capacity - 1");
    bool valid = true;
    try {
      auto u16 = xe::to_utf16(out);
      expect(xe::to_utf8(u16) == out, "CopyText output did not round-trip UTF-16");
    } catch (...) {
      valid = false;
    }
    expect(valid, "CopyText left a cut multi-byte UTF-8 character");
  }
  expect(copy_whole_utf8(sample, 3) == "A",
         "cap=3 should drop 2-byte 'ç' that does not fit before NUL");
  expect(copy_whole_utf8(sample, 4) == "A\xC3\xA7",
         "cap=4 should keep complete 'Aç'");
  expect(copy_whole_utf8(sample, 10) == "A\xC3\xA7\xC3\xA3o",
         "cap=10 should drop 4-byte emoji cut mid-sequence");
  expect(copy_whole_utf8(sample, 11) == "A\xC3\xA7\xC3\xA3o\xF0\x9F\x8E\xAE",
         "cap=11 should keep full 4-byte emoji");
}

void test_a64_lse_source_guard(const char* a64_dir_path) {
  namespace fs = std::filesystem;
  const std::regex lse_call(
      R"(\b(cas[abhl]*|ldadd[abhl]*|stadd[abhl]*|ldclr[abhl]*|stclr[abhl]*|ldeor[abhl]*|steor[abhl]*|ldset[abhl]*|stset[abhl]*|ldsmax[abhl]*|stsmax[abhl]*|ldsmin[abhl]*|stsmin[abhl]*|ldumax[abhl]*|stumax[abhl]*|ldumin[abhl]*|stumin[abhl]*|swp[abhl]*|ldapr[bh]*)\s*\()");
  const std::regex stlxr_call(
      R"(stlxr\s*\(\s*([^,]+)\s*,\s*([^,]+)\s*,\s*ptr\s*\(\s*([^)]+)\s*\)\s*\))");

  size_t files_checked = 0;
  size_t lse_sites = 0;
  bool found_sc_fn = false;

  for (const auto& entry : fs::directory_iterator(a64_dir_path)) {
    if (!entry.is_regular_file()) continue;
    const auto ext = entry.path().extension().string();
    if (ext != ".cc" && ext != ".h") continue;
    ++files_checked;

    std::ifstream in(entry.path());
    std::string whole((std::istreambuf_iterator<char>(in)),
                      std::istreambuf_iterator<char>());

    if (entry.path().filename() == "a64_backend.cc") {
      auto pos = whole.find("A64HelperEmitter::EmitReservedStoreHelper(");
      if (pos != std::string::npos) {
        found_sc_fn = true;
        auto end = whole.find("\n}\n", pos);
        std::string fn_body = whole.substr(pos, end - pos);
        expect(fn_body.find("IsFeatureEnabled(kA64EmitLSE)") !=
                   std::string::npos,
               "EmitReservedStoreHelper must check kA64EmitLSE");
        expect(fn_body.find("ldaxr(") != std::string::npos &&
                   fn_body.find("stlxr(") != std::string::npos,
               "EmitReservedStoreHelper missing ARMv8.0 LL/SC fallback");
      }
    }
    if (entry.path().filename() == "a64_seq_memory.cc") {
      auto pos = whole.find("EmitNativeStoreConditional(");
      if (pos != std::string::npos) {
        found_sc_fn = true;
        auto end = whole.find("\n}\n", pos);
        std::string fn_body = whole.substr(pos, end - pos);
        size_t guards = 0;
        for (size_t p = 0;
             (p = fn_body.find("IsFeatureEnabled(kA64EmitLSE)", p)) !=
             std::string::npos;
             p += 1) {
          ++guards;
        }
        expect(guards == 2,
               "EmitNativeStoreConditional must guard both casal and staddl with kA64EmitLSE");
        expect(fn_body.find("bump_retry") != std::string::npos,
               "EmitNativeStoreConditional missing ARMv8.0 bump_retry LL/SC loop");
      }
    }

    std::vector<std::string> lines;
    size_t start_pos = 0;
    while (start_pos <= whole.size()) {
      auto nl = whole.find('\n', start_pos);
      if (nl == std::string::npos) {
        lines.push_back(whole.substr(start_pos));
        break;
      }
      lines.push_back(whole.substr(start_pos, nl - start_pos));
      start_pos = nl + 1;
    }

    for (size_t i = 0; i < lines.size(); ++i) {
      // Every stlxr(Ws, Wt, ptr(Xn)) with concrete e.w*/e.x* registers must keep
      // Ws distinct from Wt and Xn (ARMv8 constrained unpredictable rule).
      std::smatch sm;
      if (std::regex_search(lines[i], sm, stlxr_call)) {
        std::string ws = sm[1].str();
        std::string wt = sm[2].str();
        std::string xn = sm[3].str();
        auto reg_num = [](const std::string& r) -> int {
          auto p = r.find_first_of("wx");
          if (p == std::string::npos || p + 1 >= r.size()) return -1;
          if (!std::isdigit(static_cast<unsigned char>(r[p + 1]))) return -1;
          return std::stoi(r.substr(p + 1));
        };
        int ns = reg_num(ws), nt = reg_num(wt), nn = reg_num(xn);
        if (ns >= 0 && nt >= 0) {
          expect(ns != nt, "stlxr status register aliases value register");
        }
        if (ns >= 0 && nn >= 0) {
          expect(ns != nn, "stlxr status register aliases base register");
        }
      }

      if (!std::regex_search(lines[i], lse_call)) continue;
      ++lse_sites;
      bool guarded = false;
      const size_t lookback = (i > 48) ? (i - 48) : 0;
      bool has_guard = false;
      int guard_depth = 0;
      int depth = 0;
      for (size_t j = lookback; j <= i; ++j) {
        const bool is_guard =
            (lines[j].find("if (") != std::string::npos &&
             (lines[j].find("kA64EmitLSE") != std::string::npos ||
              lines[j].find("if (lse)") != std::string::npos)) ||
            lines[j].find("kA64EmitLSE") != std::string::npos;
        if (is_guard && !has_guard) {
          has_guard = true;
          guard_depth = depth;
        }
        for (char c : lines[j]) {
          if (c == '{') {
            ++depth;
          } else if (c == '}') {
            --depth;
            if (has_guard && depth <= guard_depth) {
              has_guard = false;
            }
          }
        }
      }
      guarded = has_guard;
      if (!guarded) {
        std::fprintf(stderr, "Unguarded LSE call at %s:%zu: %s\n",
                     entry.path().filename().string().c_str(), i + 1,
                     lines[i].c_str());
      }
      expect(guarded, "ARMv8.1 LSE instruction emitted without kA64EmitLSE guard");
    }
  }
  expect(found_sc_fn && files_checked > 0 && lse_sites >= 4,
         "A64 source scan did not inspect expected LSE sites");
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: %s <path-to-cpu/backend/a64>\n", argv[0]);
    return 2;
  }
  test_to_utf8_lossy();
  test_ends_with_case();
  test_keyboard_copy_text_utf8_boundary();
  test_a64_lse_source_guard(argv[1]);
  if (failures) {
    std::fprintf(stderr, "lote16_upstream_test: %d failure(s)\n", failures);
    return 1;
  }
  std::printf("lote16_upstream_test: ok\n");
  return 0;
}
