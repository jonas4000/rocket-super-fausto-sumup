#include "Controller.h"

namespace grinder {

const char* stateName(State s) {
  switch (s) {
    case State::Idle: return "Idle";
    case State::AwaitingPayment: return "AwaitingPayment";
    case State::Grinding: return "Grinding";
    case State::Done: return "Done";
    case State::PaymentFailed: return "PaymentFailed";
    case State::Error: return "Error";
    case State::ManualGrinding: return "ManualGrinding";
  }
  return "?";
}

Controller::Controller(PaymentProvider& payment, Motor& motor,
                       SettingsStore& store, Dose* doses, size_t doseCount,
                       Config config)
    : payment_(payment),
      motor_(motor),
      store_(store),
      doses_(doses),
      doseCount_(doseCount),
      config_(config) {
  // Snapshot the factory defaults before begin() loads the saved settings.
  for (size_t i = 0; i < doseCount_ && i < kMaxDoses; ++i) {
    defaultPriceCents_[i] = doses_[i].priceCents;
    defaultGrindMs_[i] = doses_[i].grindMs;
  }
}

void Controller::begin(uint32_t nowMs) {
  stopMotor();
  store_.load(doses_, doseCount_);
  for (size_t i = 0; i < doseCount_; ++i) sanitize(doses_[i]);
  store_.loadStats(stats_);
  store_.loadLeaderboard(leaderboard_);
  leaderboard_.sanitize();
  store_.loadGameEnabled(gameEnabled_);
  enter(State::Idle, nowMs);
}

void Controller::sanitize(Dose& d) const {
  if (d.priceCents < config_.minPriceCents) d.priceCents = config_.minPriceCents;
  if (d.priceCents > config_.maxPriceCents) d.priceCents = config_.maxPriceCents;
  if (d.grindMs < config_.minGrindMs) d.grindMs = config_.minGrindMs;
  if (d.grindMs > config_.maxRunMs) d.grindMs = config_.maxRunMs;
}

bool Controller::setPriceCents(size_t index, uint32_t cents) {
  if (index >= doseCount_ || state_ != State::Idle) return false;
  doses_[index].priceCents = cents;
  sanitize(doses_[index]);
  store_.save(doses_, doseCount_);
  return true;
}

bool Controller::setGrindMs(size_t index, uint32_t ms) {
  if (index >= doseCount_ || state_ != State::Idle) return false;
  doses_[index].grindMs = ms;
  sanitize(doses_[index]);
  store_.save(doses_, doseCount_);
  return true;
}

bool Controller::setGameEnabled(bool on) {
  if (state_ != State::Idle) return false;
  gameEnabled_ = on;
  if (!on) freeShot_ = false;
  store_.saveGameEnabled(on);
  return true;
}

bool Controller::resetSettings() {
  if (state_ != State::Idle) return false;
  for (size_t i = 0; i < doseCount_ && i < kMaxDoses; ++i) {
    doses_[i].priceCents = defaultPriceCents_[i];
    doses_[i].grindMs = defaultGrindMs_[i];
    doses_[i].count = 0;
    sanitize(doses_[i]);
  }
  store_.save(doses_, doseCount_);
  leaderboard_ = Leaderboard();
  store_.saveLeaderboard(leaderboard_);
  freeShot_ = false;
  gameEnabled_ = true;
  store_.saveGameEnabled(true);
  // Every stat starts over; only the date of the last reset survives.
  const uint32_t lastReset = stats_.lastReset;
  stats_ = Stats();
  stats_.lastReset = lastReset;
  LocalTime t;
  if (wallClock_ && wallClock_->now(t)) stats_.lastReset = t.day;
  store_.saveStats(stats_);
  return true;
}

bool Controller::startManualGrind(uint32_t nowMs) {
  if (state_ != State::Idle) return false;
  enter(State::ManualGrinding, nowMs);
  return true;
}

void Controller::stopManualGrind(uint32_t nowMs) {
  if (state_ == State::ManualGrinding) enter(State::Idle, nowMs);
}

const Dose* Controller::selectedDose() const {
  if (state_ == State::Idle || state_ == State::ManualGrinding) return nullptr;
  return &doses_[selected_];
}

uint32_t Controller::grindDurationMs() const {
  uint32_t ms = doses_[selected_].grindMs;
  return ms < config_.maxRunMs ? ms : config_.maxRunMs;
}

uint32_t Controller::grindRemainingMs(uint32_t nowMs) const {
  if (state_ != State::Grinding) return 0;
  uint32_t elapsed = nowMs - stateSinceMs_;
  uint32_t total = grindDurationMs();
  return elapsed >= total ? 0 : total - elapsed;
}

uint32_t Controller::paymentRemainingMs(uint32_t nowMs) const {
  if (state_ != State::AwaitingPayment) return 0;
  uint32_t elapsed = nowMs - stateSinceMs_;
  return elapsed >= config_.paymentTimeoutMs ? 0 : config_.paymentTimeoutMs - elapsed;
}

uint32_t Controller::doneRemainingMs(uint32_t nowMs) const {
  if (state_ != State::Done) return 0;
  uint32_t elapsed = nowMs - stateSinceMs_;
  return elapsed >= config_.doneDisplayMs ? 0 : config_.doneDisplayMs - elapsed;
}

void Controller::stopMotor() { motor_.setOn(false); }

void Controller::enter(State s, uint32_t nowMs) {
  // The motor stops whenever a grinding state is left: add up its run time.
  const bool wasGrinding = state_ == State::Grinding || state_ == State::ManualGrinding;
  if (wasGrinding && s != state_) {
    stats_.grindMs += nowMs - stateSinceMs_;
    store_.saveStats(stats_);
  }
  state_ = s;
  timedOut_ = false;
  stateSinceMs_ = nowMs;
  if (s == State::Grinding || s == State::ManualGrinding) {
    motor_.setOn(true);
  } else {
    // Every non-grinding state implies the motor is off.
    stopMotor();
  }
}

void Controller::selectDose(size_t index, uint32_t nowMs) {
  if (index >= doseCount_) return;
  if (state_ != State::Idle) return;
  if (freeShotAvailable(nowMs)) {
    // Flappy Beans top 5: no payment and not counted as a paid shot. Logged
    // as refunded like a milestone, so the sales log shows every shot.
    freeShot_ = false;
    selected_ = index;
    milestone_ = 0;
    freeGrind_ = true;
    ++stats_.freeShots;
    store_.saveStats(stats_);
    if (salesLog_) salesLog_->recordSale(doses_[index], true);
    enter(State::Grinding, nowMs);
    return;
  }
  if (!online()) return;
  selected_ = index;
  milestone_ = 0;
  freeGrind_ = false;
  startCheckout(nowMs);
}

bool Controller::isMilestone(uint32_t shot) {
  static const uint32_t kMilestones[] = {25, 50, 100, 200, 500, 1000};
  for (uint32_t m : kMilestones)
    if (shot == m) return true;
  return false;
}

void Controller::countShotToday() {
  LocalTime t;
  // Without a known time, keep counting on the last known day.
  if (wallClock_ && wallClock_->now(t) && t.day != stats_.day) {
    stats_.day = t.day;
    stats_.today = 0;
  }
  ++stats_.today;
  if (stats_.today > stats_.bestDay) stats_.bestDay = stats_.today;
  store_.saveStats(stats_);
}

uint32_t Controller::shotsToday() const {
  LocalTime t;
  if (wallClock_ && wallClock_->now(t) && t.day != stats_.day) return 0;
  return stats_.today;
}

void Controller::recordGamePlayed() {
  ++stats_.gamesPlayed;
  store_.saveStats(stats_);
}

int Controller::submitScore(const char* name, uint32_t score, uint32_t nowMs) {
  const bool earned = earnsFreeShot(score);
  LocalTime t;
  uint32_t day = wallClock_ && wallClock_->now(t) ? t.day : 0;
  int rank = leaderboard_.insert(name, score, day);
  if (rank >= 0) store_.saveLeaderboard(leaderboard_);
  if (earned) {
    freeShot_ = true;
    freeShotSinceMs_ = nowMs;
  }
  return rank;
}

uint32_t Controller::freeShotRemainingMs(uint32_t nowMs) const {
  if (!freeShot_) return 0;
  uint32_t elapsed = nowMs - freeShotSinceMs_;
  return elapsed >= config_.freeShotMs ? 0 : config_.freeShotMs - elapsed;
}

uint32_t Controller::totalShots() const {
  uint32_t total = 0;
  for (size_t i = 0; i < doseCount_; ++i) total += doses_[i].count;
  return total;
}

void Controller::retry(uint32_t nowMs) {
  if (state_ != State::PaymentFailed) return;
  if (!online()) {
    enter(State::Idle, nowMs);
    return;
  }
  startCheckout(nowMs);
}

void Controller::startCheckout(uint32_t nowMs) {
  const Dose& d = doses_[selected_];
  if (!payment_.startCheckout(d.priceCents, d.name)) {
    enter(State::PaymentFailed, nowMs);
    return;
  }
  lastPollMs_ = nowMs;
  enter(State::AwaitingPayment, nowMs);
}

void Controller::cancel(uint32_t nowMs) {
  switch (state_) {
    case State::AwaitingPayment:
      payment_.cancelCheckout();
      enter(State::Idle, nowMs);
      break;
    case State::PaymentFailed:
      enter(State::Idle, nowMs);
      break;
    case State::ManualGrinding:
      enter(State::Idle, nowMs);
      break;
    case State::Grinding:
      // Paid but stopped early by the user: no refund logic, just stop.
      enter(State::Done, nowMs);
      break;
    default:
      break;
  }
}

void Controller::tick(uint32_t nowMs) {
  uint32_t elapsed = nowMs - stateSinceMs_;
  if (freeShot_ && !freeShotAvailable(nowMs)) freeShot_ = false;  // expired

  switch (state_) {
    case State::Idle:
    case State::Error:
      break;

    case State::AwaitingPayment: {
      if (nowMs - lastPollMs_ >= config_.paymentPollMs) {
        lastPollMs_ = nowMs;
        switch (payment_.pollStatus()) {
          case PaymentStatus::Succeeded:
            enter(State::Grinding, nowMs);
            ++doses_[selected_].count;
            store_.save(doses_, doseCount_);
            countShotToday();
            if (isMilestone(totalShots())) {
              milestone_ = totalShots();
              payment_.refundLastPayment();
            }
            if (salesLog_) salesLog_->recordSale(doses_[selected_], milestone_ != 0);
            return;
          case PaymentStatus::Failed:
            enter(State::PaymentFailed, nowMs);
            return;
          case PaymentStatus::Pending:
            break;
        }
      }
      if (elapsed >= config_.paymentTimeoutMs) {
        // A payment can still complete just after this; SumUpPayment keeps
        // checking the cancelled transaction and refunds it if it was paid.
        payment_.cancelCheckout();
        enter(State::PaymentFailed, nowMs);
        timedOut_ = true;
      }
      break;
    }

    case State::Grinding:
      if (elapsed >= grindDurationMs()) {
        enter(State::Done, nowMs);
      } else if (elapsed >= config_.maxRunMs) {
        // Unreachable while grindDurationMs() is clamped; kept as a backstop.
        enter(State::Error, nowMs);
      }
      break;

    case State::ManualGrinding:
      // Backstop in case the UI never reports the release.
      if (elapsed >= config_.maxRunMs) enter(State::Idle, nowMs);
      break;

    case State::Done:
      if (elapsed >= config_.doneDisplayMs) enter(State::Idle, nowMs);
      break;

    case State::PaymentFailed:
      if (elapsed >= (timedOut_ ? config_.timedOutDisplayMs : config_.failedDisplayMs))
        enter(State::Idle, nowMs);
      break;
  }
}

}  // namespace grinder
