#pragma once

#include <cstddef>
#include <cstdint>

#include "Ports.h"

namespace grinder {

struct Config {
  uint32_t maxRunMs = 30000;          // hard motor limit, independent of dose
  uint32_t minGrindMs = 500;
  uint32_t minPriceCents = 100;      // SumUp's minimum card transaction (€1)
  uint32_t maxPriceCents = 9990;
  uint32_t paymentTimeoutMs = 60000;   // unpaid checkout is cancelled after 1 min
  uint32_t paymentPollMs = 2000;      // keep SumUp API calls sparse
  uint32_t doneDisplayMs = 5000;      // "Enjoy!" + cleaning hint
  uint32_t failedDisplayMs = 20000;   // PaymentFailed waits for Try again / Cancel
  uint32_t timedOutDisplayMs = 5000;  // ...but only briefly when nobody paid at all
  uint32_t freeShotMs = 60000;        // a Flappy Beans top 5 free shot expires
  uint32_t freeShotMinScore = 10;     // ...and needs at least this many points
};

enum class State {
  Idle,
  AwaitingPayment,
  Grinding,
  Done,
  PaymentFailed,
  Error,
  ManualGrinding,  // operator test run from Settings, hold-to-run, unpaid
};

const char* stateName(State s);

// All timing is driven by the caller passing a millisecond clock (millis() on
// the device, a fake clock in the simulator/tests). Elapsed time is computed by
// unsigned subtraction, so millis() wraparound is harmless.
class Controller {
 public:
  Controller(PaymentProvider& payment, Motor& motor, SettingsStore& store,
             Dose* doses, size_t doseCount, Config config = Config());

  // Forces the motor off, then loads saved settings. Call first thing on boot.
  void begin(uint32_t nowMs);
  void tick(uint32_t nowMs);

  // UI events. Ignored when not valid in the current state.
  // Starts the payment for this dose right away (checkout sent to the reader).
  // Ignored while offline. With a free shot available it grinds right away
  // instead, unpaid (also offline), and uses the free shot up.
  void selectDose(size_t index, uint32_t nowMs);
  // After a failed payment: send a new checkout for the same dose. While
  // offline it returns to Idle instead, where the UI explains why.
  void retry(uint32_t nowMs);
  void cancel(uint32_t nowMs);

  // Settings. Only allowed while Idle; values are clamped to Config limits
  // and persisted immediately. Return false if rejected.
  bool setPriceCents(size_t index, uint32_t cents);
  bool setGrindMs(size_t index, uint32_t ms);
  // Flappy Beans on/off. Off also drops a pending free shot.
  bool setGameEnabled(bool on);
  bool gameEnabled() const { return gameEnabled_; }
  // Restores the factory price and grind time of every dose (the values the
  // dose array held at construction), turns the game back on, clears the
  // Flappy Beans top 5, zeroes the sold counts and every stat (milestones
  // start over) except the reset date.
  bool resetSettings();
  // Operator test run (Settings, hold-to-run): the motor runs from start until
  // stop, capped at maxRunMs. Start only from Idle; nothing is paid or counted.
  bool startManualGrind(uint32_t nowMs);
  void stopManualGrind(uint32_t nowMs);

  // Optional; notified once per successful payment and once per free shot
  // (logged as refunded).
  void setSalesLog(SalesLog* log) { salesLog_ = log; }
  // Optional; needed to reset the daily shot count at midnight.
  void setWallClock(WallClock* clock) { wallClock_ = clock; }
  // Optional; without it the controller always counts as online.
  void setConnectivity(Connectivity* c) { connectivity_ = c; }
  bool online() const { return !connectivity_ || connectivity_->online(); }

  // Shots ground today (0 once the day has changed and nothing was ground yet).
  uint32_t shotsToday() const;
  uint32_t bestDay() const { return stats_.bestDay; }
  // yyyymmdd of the last resetSettings(), 0 if never (or the clock was unknown).
  uint32_t lastResetDay() const { return stats_.lastReset; }
  uint32_t gamesPlayed() const { return stats_.gamesPlayed; }
  uint32_t freeShotsUsed() const { return stats_.freeShots; }
  // Total motor run time (all grinds, manual test runs included).
  uint32_t totalGrindMs() const { return stats_.grindMs; }
  // Called by the UI when a Flappy Beans round ends; persisted.
  void recordGamePlayed();

  // Flappy Beans top 5 (screensaver game).
  const Leaderboard& leaderboard() const { return leaderboard_; }
  // Rank (0-based) the score would get, -1 if it doesn't make the top 5.
  int leaderboardRank(uint32_t score) const { return leaderboard_.rankFor(score); }
  // A top 5 score with at least Config::freeShotMinScore points (so a board
  // with free places doesn't give coffee away for 1 point).
  bool earnsFreeShot(uint32_t score) const {
    return score >= config_.freeShotMinScore && leaderboardRank(score) >= 0;
  }
  // Adds the score with today's date (0 while the clock is unknown) and
  // saves the list. Returns the rank, or -1 if it didn't make it. Grants a
  // free shot if earnsFreeShot() (one, valid for Config::freeShotMs).
  int submitScore(const char* name, uint32_t score, uint32_t nowMs);
  bool freeShotAvailable(uint32_t nowMs) const { return freeShotRemainingMs(nowMs) > 0; }
  uint32_t freeShotRemainingMs(uint32_t nowMs) const;
  // The current grind is a free shot (valid from Grinding until the next dose
  // is selected, like milestone()).
  bool freeGrind() const { return freeGrind_; }

  State state() const { return state_; }
  const Dose* selectedDose() const;
  size_t doseCount() const { return doseCount_; }
  const Dose& dose(size_t i) const { return doses_[i]; }
  const Config& config() const { return config_; }
  uint32_t grindRemainingMs(uint32_t nowMs) const;
  // Time left before an unpaid checkout times out (0 when not awaiting payment).
  uint32_t paymentRemainingMs(uint32_t nowMs) const;
  // Time left on the Done screen before returning to Idle (0 otherwise).
  uint32_t doneRemainingMs(uint32_t nowMs) const;
  // Sum of all paid shots (all doses).
  uint32_t totalShots() const;
  // Shot number if the current grind is a milestone (free, refunded), else 0.
  // Valid from Grinding until the next dose is selected.
  uint32_t milestone() const { return milestone_; }
  static bool isMilestone(uint32_t shot);

 private:
  void enter(State s, uint32_t nowMs);
  void stopMotor();
  void sanitize(Dose& d) const;
  void startCheckout(uint32_t nowMs);
  void countShotToday();
  uint32_t grindDurationMs() const;

  PaymentProvider& payment_;
  Motor& motor_;
  SettingsStore& store_;
  SalesLog* salesLog_ = nullptr;
  WallClock* wallClock_ = nullptr;
  Connectivity* connectivity_ = nullptr;
  Stats stats_;
  Leaderboard leaderboard_;
  Dose* doses_;
  size_t doseCount_;
  static constexpr size_t kMaxDoses = 4;
  uint32_t defaultPriceCents_[kMaxDoses] = {};
  uint32_t defaultGrindMs_[kMaxDoses] = {};
  Config config_;

  State state_ = State::Idle;
  size_t selected_ = 0;
  uint32_t stateSinceMs_ = 0;
  uint32_t lastPollMs_ = 0;
  uint32_t milestone_ = 0;
  bool freeShot_ = false;  // earned, not used yet
  uint32_t freeShotSinceMs_ = 0;
  bool freeGrind_ = false;
  bool gameEnabled_ = true;
  bool timedOut_ = false;  // current PaymentFailed came from the payment timeout
};

}  // namespace grinder
