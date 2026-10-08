// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LUNDUKEEDIT_TEST_HOOKS_HPP
#define LUNDUKEEDIT_TEST_HOOKS_HPP

// Behavior tests are compiled with -DLUNDUKE_EDIT_TEST_HOOKS. The production
// lunduke-edit target is not, so the environment names below are not in that
// binary. Without the define every helper is the real default: the caller's
// limit, or no stand-in for a dialog.

#include <cstddef>

#ifdef LUNDUKE_EDIT_TEST_HOOKS
#include <glib.h>
#include <cstdlib>
#endif

namespace lundukeedit {

#ifdef LUNDUKE_EDIT_TEST_HOOKS

inline std::size_t parse_test_size(const char* value, std::size_t fallback) {
  if (value == nullptr || value[0] == '\0') {
    return fallback;
  }
  char* end = nullptr;
  const unsigned long parsed = std::strtoul(value, &end, 10);
  if (end == value) {
    return fallback;
  }
  return static_cast<std::size_t>(parsed);
}

inline int parse_test_int(const char* value, int fallback) {
  if (value == nullptr || value[0] == '\0') {
    return fallback;
  }
  char* end = nullptr;
  const long parsed = std::strtol(value, &end, 10);
  if (end == value || parsed <= 0) {
    return fallback;
  }
  return static_cast<int>(parsed);
}

#endif

inline bool test_mode() {
#ifdef LUNDUKE_EDIT_TEST_HOOKS
  return g_getenv("LUNDUKE_EDIT_TEST") != nullptr;
#else
  return false;
#endif
}

inline const char* test_large() {
#ifdef LUNDUKE_EDIT_TEST_HOOKS
  return g_getenv("LUNDUKE_EDIT_TEST_LARGE");
#else
  return nullptr;
#endif
}

inline const char* test_discard() {
#ifdef LUNDUKE_EDIT_TEST_HOOKS
  return g_getenv("LUNDUKE_EDIT_TEST_DISCARD");
#else
  return nullptr;
#endif
}

inline const char* test_save_as() {
#ifdef LUNDUKE_EDIT_TEST_HOOKS
  return g_getenv("LUNDUKE_EDIT_TEST_SAVE_AS");
#else
  return nullptr;
#endif
}

inline const char* test_replace() {
#ifdef LUNDUKE_EDIT_TEST_HOOKS
  return g_getenv("LUNDUKE_EDIT_TEST_REPLACE");
#else
  return nullptr;
#endif
}

inline const char* test_huge_undo_choice() {
#ifdef LUNDUKE_EDIT_TEST_HOOKS
  return g_getenv("LUNDUKE_EDIT_TEST_HUGE_UNDO");
#else
  return nullptr;
#endif
}

inline std::size_t test_max_open_bytes(std::size_t fallback) {
#ifdef LUNDUKE_EDIT_TEST_HOOKS
  return parse_test_size(g_getenv("LUNDUKE_EDIT_TEST_MAX_OPEN"), fallback);
#else
  return fallback;
#endif
}

inline std::size_t test_max_open_hard_bytes(std::size_t fallback) {
#ifdef LUNDUKE_EDIT_TEST_HOOKS
  return parse_test_size(g_getenv("LUNDUKE_EDIT_TEST_MAX_OPEN_HARD"), fallback);
#else
  return fallback;
#endif
}

inline std::size_t test_max_paste_bytes(std::size_t fallback) {
#ifdef LUNDUKE_EDIT_TEST_HOOKS
  return parse_test_size(g_getenv("LUNDUKE_EDIT_TEST_MAX_PASTE"), fallback);
#else
  return fallback;
#endif
}

inline int test_max_find_hits(int fallback) {
#ifdef LUNDUKE_EDIT_TEST_HOOKS
  return parse_test_int(g_getenv("LUNDUKE_EDIT_TEST_MAX_HITS"), fallback);
#else
  return fallback;
#endif
}

inline int test_find_chunk(int fallback) {
#ifdef LUNDUKE_EDIT_TEST_HOOKS
  return parse_test_int(g_getenv("LUNDUKE_EDIT_TEST_CHUNK"), fallback);
#else
  return fallback;
#endif
}

inline std::size_t test_huge_undo_bytes(std::size_t fallback) {
#ifdef LUNDUKE_EDIT_TEST_HOOKS
  return parse_test_size(g_getenv("LUNDUKE_EDIT_TEST_HUGE_BYTES"), fallback);
#else
  return fallback;
#endif
}

}  // namespace lundukeedit

#endif
