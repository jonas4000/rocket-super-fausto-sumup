#pragma once

#include <cstddef>
#include <cstdint>

#include "Leaderboard.h"

// Interfaces between the pure state machine and the outside world.
// Firmware implements them against real hardware / SumUp / NVS; the simulators
// and tests implement them with fakes.

namespace grinder {

struct Dose {
  const char* name;
  uint32_t priceCents;  // editable in settings
  uint32_t grindMs;     // editable in settings
  uint32_t count;       // paid grinds, shown in settings
  // Portafilter to use, e.g. "single-spout"; shown while waiting for payment.
  // nullptr hides the note.
  const char* portafilter = nullptr;
  uint8_t cups = 1;  // cup icons on the start screen
};

enum class PaymentStatus {
  Pending,    // still waiting, or status unknown (e.g. network error)
  Succeeded,  // confirmed by SumUp; the only status that may start the motor
  Failed,     // declined, cancelled on the reader, or checkout rejected
};

class PaymentProvider {
 public:
  virtual ~PaymentProvider() = default;
  // Sends a checkout to the reader. Returns false if it could not be started.
  virtual bool startCheckout(uint32_t amountCents, const char* description) = 0;
  // Called periodically while awaiting payment. Must not block for long.
  virtual PaymentStatus pollStatus() = 0;
  // Best effort: abort the checkout on the reader.
  virtual void cancelCheckout() = 0;
  // Refunds the payment that just succeeded (milestone shots are free).
  // Must not block; a real implementation queues the API call.
  virtual void refundLastPayment() = 0;
};

class Motor {
 public:
  virtual ~Motor() = default;
  virtual void setOn(bool on) = 0;
};

// Stats beyond the per-dose counters, shown on Info > Stats; persisted.
struct Stats {
  uint32_t day = 0;         // yyyymmdd of `today`, 0 = never set
  uint32_t today = 0;       // shots ground on `day`
  uint32_t bestDay = 0;     // most shots ground on a single day
  // "Reset to defaults" clears everything except this one:
  uint32_t lastReset = 0;   // yyyymmdd of the last "Reset to defaults", 0 = never/unknown
  uint32_t gamesPlayed = 0;  // Flappy Beans rounds finished
  uint32_t freeShots = 0;    // free shots ground (Flappy Beans records)
  uint32_t grindMs = 0;      // total motor run time, paid, free and manual
};

// Persists price, grind time and count per dose (NVS on the device).
class SettingsStore {
 public:
  virtual ~SettingsStore() = default;
  // Overwrites the given defaults with saved values, if any exist.
  virtual void load(Dose* doses, size_t count) = 0;
  virtual void save(const Dose* doses, size_t count) = 0;
  // Stats; stores without persistence can keep the no-ops.
  virtual void loadStats(Stats&) {}
  virtual void saveStats(const Stats&) {}
  // Flappy Beans top 5.
  virtual void loadLeaderboard(Leaderboard&) {}
  virtual void saveLeaderboard(const Leaderboard&) {}
  // Whether Flappy Beans (and the screensaver that leads to it) is on.
  virtual void loadGameEnabled(bool&) {}
  virtual void saveGameEnabled(bool) {}
};

struct LocalTime {
  uint32_t day;  // yyyymmdd
  int hour;      // 0..23
  int minute;    // 0..59
};

// Local wall-clock time (NTP on the device).
class WallClock {
 public:
  virtual ~WallClock() = default;
  // Returns false while the time is unknown, e.g. before the first NTP sync.
  virtual bool now(LocalTime& out) = 0;
};

// Whether the payment service can be reached (Wi-Fi on the device). While
// offline, no new payment is started: it could not be followed up.
class Connectivity {
 public:
  virtual ~Connectivity() = default;
  virtual bool online() = 0;
};

// Records each paid sale (Supabase on the device). Must not block: called from
// the controller tick right after a payment succeeds.
class SalesLog {
 public:
  virtual ~SalesLog() = default;
  // refunded: the shot was free (milestone) and its payment is being refunded.
  virtual void recordSale(const Dose& dose, bool refunded) = 0;
};

}  // namespace grinder
