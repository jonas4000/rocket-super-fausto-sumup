#pragma once

#include <cstddef>
#include <cstdint>

namespace grinder {

// Flappy Beans top 5, best first. Plain data so stores can persist it as is
// (an NVS blob on the device); sanitize() after loading.
struct ScoreEntry {
  static constexpr size_t kNameMax = 10;
  char name[kNameMax + 1];
  uint32_t score;
  uint32_t day;  // yyyymmdd, 0 = clock unknown when it was set
};

struct Leaderboard {
  static constexpr size_t kSize = 5;
  uint32_t count = 0;
  ScoreEntry entries[kSize] = {};

  // Position (0-based) a new score would get, or -1 if it doesn't make the
  // list. 0 points never count; ties rank below the older entry.
  int rankFor(uint32_t score) const;
  // Inserts at rankFor(score) and returns that rank (-1: not inserted). The
  // name goes through cleanName().
  int insert(const char* name, uint32_t score, uint32_t day);
  // Repairs data from storage: count in range, names terminated and clean,
  // scores in order.
  void sanitize();

  // Uppercase A-Z, 0-9, spaces (collapsed and trimmed), at most kNameMax
  // characters. Falls back to "BEAN" when nothing is left.
  static void cleanName(const char* in, char* out);
};

}  // namespace grinder
