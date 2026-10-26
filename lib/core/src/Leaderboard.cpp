#include "Leaderboard.h"

#include <cstring>

namespace grinder {

int Leaderboard::rankFor(uint32_t score) const {
  if (score == 0) return -1;
  size_t n = count < kSize ? count : kSize;
  for (size_t i = 0; i < n; ++i)
    if (score > entries[i].score) return static_cast<int>(i);
  return n < kSize ? static_cast<int>(n) : -1;
}

int Leaderboard::insert(const char* name, uint32_t score, uint32_t day) {
  int rank = rankFor(score);
  if (rank < 0) return -1;
  if (count < kSize) ++count;
  for (size_t i = count - 1; i > static_cast<size_t>(rank); --i) entries[i] = entries[i - 1];
  ScoreEntry& e = entries[rank];
  cleanName(name, e.name);
  e.score = score;
  e.day = day;
  return rank;
}

void Leaderboard::sanitize() {
  if (count > kSize) count = kSize;
  for (size_t i = 0; i < count; ++i) {
    ScoreEntry& e = entries[i];
    e.name[ScoreEntry::kNameMax] = '\0';
    char clean[ScoreEntry::kNameMax + 1];
    cleanName(e.name, clean);
    std::memcpy(e.name, clean, sizeof clean);
    // Garbage or zero scores end the list there.
    if (e.score == 0 || (i > 0 && e.score > entries[i - 1].score)) {
      count = i;
      break;
    }
  }
  for (size_t i = count; i < kSize; ++i) entries[i] = ScoreEntry{};
}

void Leaderboard::cleanName(const char* in, char* out) {
  size_t n = 0;
  bool pendingSpace = false;
  for (const char* p = in; p && *p && n < ScoreEntry::kNameMax; ++p) {
    char c = *p;
    if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    if (c == ' ') {
      pendingSpace = n > 0;  // no leading spaces, runs become one
      continue;
    }
    if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))) continue;
    if (pendingSpace) {
      if (n + 1 >= ScoreEntry::kNameMax) break;  // no trailing space either
      out[n++] = ' ';
      pendingSpace = false;
    }
    out[n++] = c;
  }
  out[n] = '\0';
  if (n == 0) std::strcpy(out, "BEAN");
}

}  // namespace grinder
