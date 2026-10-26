#include <unity.h>

#include <cstring>

#include "Controller.h"
#include "SaleJson.h"

using namespace grinder;

namespace {

struct FakePayment : PaymentProvider {
  bool startOk = true;
  PaymentStatus status = PaymentStatus::Pending;
  int started = 0, polls = 0, cancels = 0, refunds = 0;
  uint32_t lastAmount = 0;

  bool startCheckout(uint32_t amountCents, const char*) override {
    ++started;
    lastAmount = amountCents;
    return startOk;
  }
  PaymentStatus pollStatus() override {
    ++polls;
    return status;
  }
  void cancelCheckout() override { ++cancels; }
  void refundLastPayment() override { ++refunds; }
};

struct FakeMotor : Motor {
  bool on = true;  // start "on" to prove begin() turns it off
  int onCount = 0;
  void setOn(bool v) override {
    if (v) ++onCount;
    on = v;
  }
};

struct FakeStore : SettingsStore {
  int saves = 0;
  bool hasSaved = false;
  Dose saved[2] = {};
  Stats stats;
  bool game = true;
  void loadStats(Stats& s) override { s = stats; }
  void saveStats(const Stats& s) override { stats = s; }
  void loadGameEnabled(bool& on) override { on = game; }
  void saveGameEnabled(bool on) override { game = on; }

  void load(Dose* doses, size_t count) override {
    if (!hasSaved) return;
    for (size_t i = 0; i < count; ++i) doses[i] = saved[i];
  }
  void save(const Dose* doses, size_t count) override {
    ++saves;
    for (size_t i = 0; i < count; ++i) saved[i] = doses[i];
  }
};

Dose doses[2];
void resetDoses() {
  doses[0] = {"single", 150, 4000, 0};
  doses[1] = {"huge", 999, 60000, 0};  // longer than maxRunMs
}

Config testConfig() {
  Config c;
  c.maxRunMs = 30000;
  c.paymentTimeoutMs = 60000;
  c.paymentPollMs = 1000;
  c.failedDisplayMs = 3000;
  c.timedOutDisplayMs = 1000;
  c.doneDisplayMs = 6000;
  return c;
}

FakePayment* pay;
FakeMotor* motor;
FakeStore* store;
Controller* ctl;

void run(uint32_t fromMs, uint32_t toMs) {
  for (uint32_t t = fromMs; t <= toMs; t += 100) ctl->tick(t);
}

}  // namespace

void setUp() {
  pay = new FakePayment();
  motor = new FakeMotor();
  store = new FakeStore();
  resetDoses();
  ctl = new Controller(*pay, *motor, *store, doses, 2, testConfig());
  ctl->begin(0);
}

void tearDown() {
  delete ctl;
  delete store;
  delete motor;
  delete pay;
}

void test_begin_turns_motor_off() {
  TEST_ASSERT_FALSE(motor->on);
  TEST_ASSERT_EQUAL(State::Idle, ctl->state());
}

void test_happy_path_grinds_for_dose_time() {
  ctl->selectDose(0, 0);
  TEST_ASSERT_EQUAL(State::AwaitingPayment, ctl->state());
  TEST_ASSERT_EQUAL_UINT32(150, pay->lastAmount);
  TEST_ASSERT_FALSE(motor->on);

  pay->status = PaymentStatus::Succeeded;
  run(100, 1000);
  TEST_ASSERT_EQUAL(State::Grinding, ctl->state());
  TEST_ASSERT_TRUE(motor->on);

  run(1100, 4900);
  TEST_ASSERT_TRUE(motor->on);
  ctl->tick(5000);
  TEST_ASSERT_FALSE(motor->on);
  TEST_ASSERT_EQUAL(State::Done, ctl->state());

  run(5100, 10900);
  TEST_ASSERT_EQUAL(State::Done, ctl->state());  // done screen stays longer
  ctl->tick(11000);
  TEST_ASSERT_EQUAL(State::Idle, ctl->state());
  TEST_ASSERT_EQUAL(1, motor->onCount);
}

void test_select_starts_payment_once() {
  ctl->selectDose(1, 0);
  TEST_ASSERT_EQUAL(State::AwaitingPayment, ctl->state());
  TEST_ASSERT_EQUAL(1, pay->started);
  TEST_ASSERT_EQUAL_UINT32(999, pay->lastAmount);
  ctl->selectDose(0, 100);  // second tap while paying is ignored
  TEST_ASSERT_EQUAL(1, pay->started);
  ctl->selectDose(5, 200);  // out of range
  TEST_ASSERT_EQUAL(1, pay->started);
}

void test_payment_remaining_counts_down() {
  TEST_ASSERT_EQUAL_UINT32(0, ctl->paymentRemainingMs(0));
  ctl->selectDose(0, 1000);
  TEST_ASSERT_EQUAL_UINT32(60000, ctl->paymentRemainingMs(1000));
  TEST_ASSERT_EQUAL_UINT32(45000, ctl->paymentRemainingMs(16000));
  TEST_ASSERT_EQUAL_UINT32(0, ctl->paymentRemainingMs(99000));
}

void test_retry_after_failed_payment() {
  ctl->selectDose(1, 0);
  pay->status = PaymentStatus::Failed;
  run(100, 1000);
  TEST_ASSERT_EQUAL(State::PaymentFailed, ctl->state());
  pay->status = PaymentStatus::Pending;
  ctl->retry(1100);
  TEST_ASSERT_EQUAL(State::AwaitingPayment, ctl->state());
  TEST_ASSERT_EQUAL(2, pay->started);
  TEST_ASSERT_EQUAL_UINT32(999, pay->lastAmount);  // same dose
  ctl->retry(1200);  // only valid after a failure
  TEST_ASSERT_EQUAL(2, pay->started);
}

void test_cancel_after_failed_payment() {
  ctl->selectDose(0, 0);
  pay->status = PaymentStatus::Failed;
  run(100, 1000);
  ctl->cancel(1100);
  TEST_ASSERT_EQUAL(State::Idle, ctl->state());
}

void test_done_remaining_counts_down() {
  ctl->selectDose(0, 0);
  pay->status = PaymentStatus::Succeeded;
  run(100, 5000);  // paid at 1000, done at 5000
  TEST_ASSERT_EQUAL(State::Done, ctl->state());
  TEST_ASSERT_EQUAL_UINT32(6000, ctl->doneRemainingMs(5000));
  TEST_ASSERT_EQUAL_UINT32(1000, ctl->doneRemainingMs(10000));
}

void test_declined_payment_never_starts_motor() {
  ctl->selectDose(0, 0);
  pay->status = PaymentStatus::Failed;
  run(100, 2000);
  TEST_ASSERT_EQUAL(State::PaymentFailed, ctl->state());
  TEST_ASSERT_EQUAL(0, motor->onCount);
}

void test_checkout_start_failure() {
  pay->startOk = false;
  ctl->selectDose(0, 0);
  TEST_ASSERT_EQUAL(State::PaymentFailed, ctl->state());
}

void test_payment_timeout_cancels_and_never_grinds() {
  ctl->selectDose(0, 0);
  run(100, 60000);
  TEST_ASSERT_EQUAL(State::PaymentFailed, ctl->state());
  TEST_ASSERT_EQUAL(1, pay->cancels);
  TEST_ASSERT_EQUAL(0, motor->onCount);
}

void test_polls_are_rate_limited() {
  ctl->selectDose(0, 0);
  run(100, 10000);
  TEST_ASSERT_EQUAL(10, pay->polls);
}

void test_cancel_while_awaiting_payment() {
  ctl->selectDose(0, 0);
  ctl->cancel(500);
  TEST_ASSERT_EQUAL(State::Idle, ctl->state());
  TEST_ASSERT_EQUAL(1, pay->cancels);
}

void test_grind_clamped_to_max_run() {
  ctl->selectDose(1, 0);
  pay->status = PaymentStatus::Succeeded;
  run(100, 1000);
  TEST_ASSERT_TRUE(motor->on);
  run(1100, 31000);
  TEST_ASSERT_FALSE(motor->on);
}

void test_millis_wraparound() {
  uint32_t start = 0xFFFFFFFFu - 1000;
  ctl->begin(start);
  ctl->selectDose(0, start);
  pay->status = PaymentStatus::Succeeded;
  ctl->tick(start + 1000);
  TEST_ASSERT_EQUAL(State::Grinding, ctl->state());
  ctl->tick(start + 1000 + 3900);  // wrapped past 0
  TEST_ASSERT_TRUE(motor->on);
  ctl->tick(start + 1000 + 4000);
  TEST_ASSERT_EQUAL(State::Done, ctl->state());
}

void test_counts_and_saves_paid_grinds() {
  ctl->selectDose(0, 0);
  pay->status = PaymentStatus::Succeeded;
  run(100, 1000);
  TEST_ASSERT_EQUAL_UINT32(1, ctl->dose(0).count);
  TEST_ASSERT_EQUAL_UINT32(0, ctl->dose(1).count);
  TEST_ASSERT_EQUAL_UINT32(1, store->saved[0].count);
}

void test_declined_payment_not_counted() {
  ctl->selectDose(0, 0);
  pay->status = PaymentStatus::Failed;
  run(100, 2000);
  TEST_ASSERT_EQUAL_UINT32(0, ctl->dose(0).count);
}

void test_settings_are_clamped_and_saved() {
  TEST_ASSERT_TRUE(ctl->setPriceCents(0, 120));
  TEST_ASSERT_EQUAL_UINT32(120, ctl->dose(0).priceCents);
  TEST_ASSERT_EQUAL_UINT32(120, store->saved[0].priceCents);

  ctl->setPriceCents(0, 0);
  TEST_ASSERT_EQUAL_UINT32(100, ctl->dose(0).priceCents);
  ctl->setGrindMs(0, 100);
  TEST_ASSERT_EQUAL_UINT32(500, ctl->dose(0).grindMs);
  ctl->setGrindMs(0, 999999);
  TEST_ASSERT_EQUAL_UINT32(30000, ctl->dose(0).grindMs);
}

void test_settings_rejected_unless_idle() {
  ctl->selectDose(0, 0);
  TEST_ASSERT_FALSE(ctl->setPriceCents(0, 200));
  TEST_ASSERT_EQUAL_UINT32(150, ctl->dose(0).priceCents);
}

void test_reset_settings_restores_defaults_and_clears_counts() {
  store->hasSaved = true;
  store->saved[0] = {"single", 170, 5000, 42};
  store->saved[1] = {"huge", 200, 9000, 7};
  ctl->begin(0);
  TEST_ASSERT_TRUE(ctl->resetSettings());
  TEST_ASSERT_EQUAL_UINT32(150, ctl->dose(0).priceCents);
  TEST_ASSERT_EQUAL_UINT32(4000, ctl->dose(0).grindMs);
  TEST_ASSERT_EQUAL_UINT32(0, ctl->dose(0).count);
  TEST_ASSERT_EQUAL_UINT32(0, ctl->dose(1).count);
  TEST_ASSERT_EQUAL_UINT32(30000, ctl->dose(1).grindMs);  // still clamped
  TEST_ASSERT_EQUAL_UINT32(150, store->saved[0].priceCents);
  TEST_ASSERT_EQUAL_UINT32(0, store->saved[0].count);

  ctl->selectDose(0, 0);
  TEST_ASSERT_FALSE(ctl->resetSettings());
}

struct FakeWallClock : WallClock {
  bool known = true;
  LocalTime t = {20261016, 10, 0};
  bool now(LocalTime& out) override {
    out = t;
    return known;
  }
};

void test_reset_settings_records_date() {
  FakeWallClock clock;
  ctl->setWallClock(&clock);
  ctl->begin(0);
  TEST_ASSERT_EQUAL_UINT32(0, ctl->lastResetDay());
  TEST_ASSERT_TRUE(ctl->resetSettings());
  TEST_ASSERT_EQUAL_UINT32(20261016, ctl->lastResetDay());
  TEST_ASSERT_EQUAL_UINT32(20261016, store->stats.lastReset);  // persisted

  clock.known = false;  // no NTP yet: keep the previous date
  clock.t.day = 20261020;
  TEST_ASSERT_TRUE(ctl->resetSettings());
  TEST_ASSERT_EQUAL_UINT32(20261016, ctl->lastResetDay());
}

void test_game_switch_is_saved_and_reset_turns_it_on() {
  TEST_ASSERT_TRUE(ctl->gameEnabled());
  TEST_ASSERT_TRUE(ctl->setGameEnabled(false));
  TEST_ASSERT_FALSE(store->game);
  Controller reloaded(*pay, *motor, *store, doses, 2, testConfig());
  reloaded.begin(0);
  TEST_ASSERT_FALSE(reloaded.gameEnabled());
  TEST_ASSERT_TRUE(reloaded.resetSettings());
  TEST_ASSERT_TRUE(reloaded.gameEnabled());
  TEST_ASSERT_TRUE(store->game);
}

void test_game_switch_only_while_idle() {
  ctl->selectDose(0, 0);
  TEST_ASSERT_FALSE(ctl->setGameEnabled(false));
  TEST_ASSERT_TRUE(ctl->gameEnabled());
}

void test_reset_settings_clears_all_stats() {
  FakeWallClock clock;
  ctl->setWallClock(&clock);
  store->stats = {20261016, 7, 9, 20261001, 12, 2, 60000};
  ctl->begin(0);
  clock.known = false;  // the previous reset date stays
  TEST_ASSERT_TRUE(ctl->resetSettings());
  const Stats& s = store->stats;
  TEST_ASSERT_EQUAL_UINT32(0, s.day);
  TEST_ASSERT_EQUAL_UINT32(0, s.today);
  TEST_ASSERT_EQUAL_UINT32(0, s.bestDay);
  TEST_ASSERT_EQUAL_UINT32(0, s.gamesPlayed);
  TEST_ASSERT_EQUAL_UINT32(0, s.freeShots);
  TEST_ASSERT_EQUAL_UINT32(0, s.grindMs);
  TEST_ASSERT_EQUAL_UINT32(20261001, s.lastReset);
  TEST_ASSERT_EQUAL_UINT32(0, ctl->shotsToday());
}

struct FakeConnectivity : Connectivity {
  bool up = true;
  bool online() override { return up; }
};

void test_offline_blocks_new_payments() {
  FakeConnectivity net;
  ctl->setConnectivity(&net);
  net.up = false;
  ctl->selectDose(0, 0);
  TEST_ASSERT_EQUAL(State::Idle, ctl->state());
  TEST_ASSERT_EQUAL(0, pay->started);
  net.up = true;
  ctl->selectDose(0, 0);
  TEST_ASSERT_EQUAL(State::AwaitingPayment, ctl->state());
}

void test_retry_while_offline_returns_to_idle() {
  FakeConnectivity net;
  ctl->setConnectivity(&net);
  ctl->selectDose(0, 0);
  pay->status = PaymentStatus::Failed;
  ctl->tick(2000);
  TEST_ASSERT_EQUAL(State::PaymentFailed, ctl->state());
  net.up = false;
  ctl->retry(3000);
  TEST_ASSERT_EQUAL(State::Idle, ctl->state());
  TEST_ASSERT_EQUAL(1, pay->started);
}

void test_manual_grind_runs_until_stopped() {
  TEST_ASSERT_TRUE(ctl->startManualGrind(0));
  TEST_ASSERT_EQUAL(State::ManualGrinding, ctl->state());
  TEST_ASSERT_TRUE(motor->on);
  run(100, 5000);
  TEST_ASSERT_TRUE(motor->on);
  ctl->stopManualGrind(5000);
  TEST_ASSERT_EQUAL(State::Idle, ctl->state());
  TEST_ASSERT_FALSE(motor->on);
  TEST_ASSERT_EQUAL_UINT32(0, ctl->totalShots());
  TEST_ASSERT_EQUAL(0, pay->started);
  TEST_ASSERT_EQUAL_UINT32(0, ctl->shotsToday());
}

void test_manual_grind_capped_at_max_run() {
  ctl->startManualGrind(0);
  run(100, 29900);
  TEST_ASSERT_TRUE(motor->on);
  ctl->tick(30000);
  TEST_ASSERT_EQUAL(State::Idle, ctl->state());
  TEST_ASSERT_FALSE(motor->on);
}

void test_manual_grind_only_from_idle() {
  ctl->selectDose(0, 0);
  TEST_ASSERT_FALSE(ctl->startManualGrind(0));
  TEST_ASSERT_EQUAL(State::AwaitingPayment, ctl->state());
  TEST_ASSERT_FALSE(motor->on);
  ctl->cancel(0);
  ctl->startManualGrind(0);
  TEST_ASSERT_FALSE(ctl->setPriceCents(0, 80));  // settings locked while running
  ctl->cancel(100);
  TEST_ASSERT_FALSE(motor->on);
}

void test_begin_loads_and_sanitizes_saved_settings() {
  store->hasSaved = true;
  store->saved[0] = {"single", 170, 5000, 42};
  store->saved[1] = {"huge", 0, 1, 7};  // corrupt values
  ctl->begin(0);
  TEST_ASSERT_EQUAL_UINT32(170, ctl->dose(0).priceCents);
  TEST_ASSERT_EQUAL_UINT32(42, ctl->dose(0).count);
  TEST_ASSERT_EQUAL_UINT32(100, ctl->dose(1).priceCents);
  TEST_ASSERT_EQUAL_UINT32(500, ctl->dose(1).grindMs);
}

struct FakeSalesLog : SalesLog {
  int sales = 0;
  uint32_t lastPrice = 0;
  bool lastRefunded = false;
  void recordSale(const Dose& d, bool refunded) override {
    ++sales;
    lastPrice = d.priceCents;
    lastRefunded = refunded;
  }
};

void test_sale_logged_once_on_success_only() {
  FakeSalesLog log;
  ctl->setSalesLog(&log);
  ctl->selectDose(0, 0);
  pay->status = PaymentStatus::Failed;
  run(100, 2000);
  TEST_ASSERT_EQUAL(0, log.sales);

  run(2100, 6000);  // back to Idle
  ctl->selectDose(0, 6000);
  pay->status = PaymentStatus::Succeeded;
  run(6100, 12000);
  TEST_ASSERT_EQUAL(1, log.sales);
  TEST_ASSERT_EQUAL_UINT32(150, log.lastPrice);
}

void test_sale_json() {
  Dose d = {"Single", 50, 4000, 3};
  char buf[192];
  TEST_ASSERT_TRUE(formatSaleJson(buf, sizeof buf, d, false));
  TEST_ASSERT_EQUAL_STRING("{\"dose\":\"Single\",\"price_cents\":50,\"refunded\":false}",
                           buf);
  TEST_ASSERT_TRUE(formatSaleJson(buf, sizeof buf, d, true));
  TEST_ASSERT_NOT_NULL(std::strstr(buf, "\"refunded\":true"));

  char tiny[16];
  TEST_ASSERT_FALSE(formatSaleJson(tiny, sizeof tiny, d, false));
}

void payOnce(uint32_t& t, size_t dose) {
  ctl->selectDose(dose, t);
  pay->status = PaymentStatus::Succeeded;
  run(t + 100, t + 1000);  // payment confirmed, grinding starts
  t += 1000;
}

void test_milestone_shot_is_refunded() {
  FakeSalesLog log;
  ctl->setSalesLog(&log);
  store->hasSaved = true;  // 24 shots so far, split over both doses
  store->saved[0] = {"single", 50, 4000, 20};
  store->saved[1] = {"huge", 999, 60000, 4};
  ctl->begin(0);

  uint32_t t = 0;
  payOnce(t, 1);  // shot #25
  TEST_ASSERT_EQUAL(State::Grinding, ctl->state());  // grinding not delayed
  TEST_ASSERT_EQUAL_UINT32(25, ctl->milestone());
  TEST_ASSERT_EQUAL(1, pay->refunds);
  TEST_ASSERT_TRUE(log.lastRefunded);
}

void test_regular_shot_not_refunded() {
  FakeSalesLog log;
  ctl->setSalesLog(&log);
  uint32_t t = 0;
  payOnce(t, 0);  // shot #1
  TEST_ASSERT_EQUAL_UINT32(0, ctl->milestone());
  TEST_ASSERT_EQUAL(0, pay->refunds);
  TEST_ASSERT_FALSE(log.lastRefunded);
}

void test_milestone_cleared_on_next_selection() {
  store->hasSaved = true;
  store->saved[0] = {"single", 50, 4000, 24};
  store->saved[1] = {"huge", 999, 60000, 0};
  ctl->begin(0);
  uint32_t t = 0;
  payOnce(t, 0);  // #25
  TEST_ASSERT_EQUAL_UINT32(25, ctl->milestone());
  run(t, t + 20000);  // grind, done, back to Idle
  t += 20000;
  ctl->selectDose(0, t);
  TEST_ASSERT_EQUAL_UINT32(0, ctl->milestone());
}

void test_milestone_list() {
  const uint32_t yes[] = {25, 50, 100, 200, 500, 1000};
  for (uint32_t n : yes) TEST_ASSERT_TRUE(Controller::isMilestone(n));
  const uint32_t no[] = {0, 1, 24, 26, 75, 150, 250, 999, 1001, 2000};
  for (uint32_t n : no) TEST_ASSERT_FALSE(Controller::isMilestone(n));
}

void test_shots_today_counts_and_resets_at_midnight() {
  FakeWallClock clock;
  ctl->setWallClock(&clock);
  uint32_t t = 0;
  payOnce(t, 0);
  run(t, t + 20000);
  t += 20000;
  payOnce(t, 1);
  TEST_ASSERT_EQUAL_UINT32(2, ctl->shotsToday());
  TEST_ASSERT_EQUAL_UINT32(2, store->stats.today);  // persisted

  clock.t.day = 20261017;  // next morning, nothing ground yet
  TEST_ASSERT_EQUAL_UINT32(0, ctl->shotsToday());
  run(t, t + 40000);
  t += 40000;
  payOnce(t, 0);
  TEST_ASSERT_EQUAL_UINT32(1, ctl->shotsToday());
  TEST_ASSERT_EQUAL_UINT32(20261017, store->stats.day);
}

void test_shots_today_survives_reboot() {
  FakeWallClock clock;
  store->stats = {20261016, 7, 7, 0};
  ctl->setWallClock(&clock);
  ctl->begin(0);
  TEST_ASSERT_EQUAL_UINT32(7, ctl->shotsToday());
}

void test_shots_today_without_clock_keeps_counting() {
  FakeWallClock clock;
  clock.known = false;  // no NTP yet
  store->stats = {20261016, 3, 3, 0};
  ctl->setWallClock(&clock);
  ctl->begin(0);
  uint32_t t = 0;
  payOnce(t, 0);
  TEST_ASSERT_EQUAL_UINT32(4, ctl->shotsToday());
}

void test_best_day_is_kept_across_days() {
  FakeWallClock clock;
  store->stats = {20261015, 0, 5, 0};  // record of 5 from an earlier day
  ctl->setWallClock(&clock);
  ctl->begin(0);
  uint32_t t = 0;
  for (int i = 0; i < 6; ++i) {  // six shots today beat it
    payOnce(t, 0);
    run(t, t + 20000);
    t += 20000;
  }
  TEST_ASSERT_EQUAL_UINT32(6, ctl->bestDay());
  clock.t.day = 20261017;
  payOnce(t, 0);
  TEST_ASSERT_EQUAL_UINT32(1, ctl->shotsToday());
  TEST_ASSERT_EQUAL_UINT32(6, ctl->bestDay());  // record stays
  TEST_ASSERT_EQUAL_UINT32(6, store->stats.bestDay);
}

void test_timeout_returns_to_start_quickly() {
  ctl->selectDose(0, 0);
  run(100, 60000);  // nobody pays
  TEST_ASSERT_EQUAL(State::PaymentFailed, ctl->state());
  run(60100, 60900);
  TEST_ASSERT_EQUAL(State::PaymentFailed, ctl->state());
  ctl->tick(61000);  // timedOutDisplayMs after the timeout
  TEST_ASSERT_EQUAL(State::Idle, ctl->state());
}

void test_declined_payment_waits_longer() {
  ctl->selectDose(0, 0);
  pay->status = PaymentStatus::Failed;
  run(100, 1000);  // declined at 1000
  run(1100, 3900);
  TEST_ASSERT_EQUAL(State::PaymentFailed, ctl->state());  // past the 1 s timeout value
  ctl->tick(4000);  // failedDisplayMs
  TEST_ASSERT_EQUAL(State::Idle, ctl->state());
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_begin_turns_motor_off);
  RUN_TEST(test_happy_path_grinds_for_dose_time);
  RUN_TEST(test_select_starts_payment_once);
  RUN_TEST(test_payment_remaining_counts_down);
  RUN_TEST(test_retry_after_failed_payment);
  RUN_TEST(test_cancel_after_failed_payment);
  RUN_TEST(test_done_remaining_counts_down);
  RUN_TEST(test_declined_payment_never_starts_motor);
  RUN_TEST(test_checkout_start_failure);
  RUN_TEST(test_payment_timeout_cancels_and_never_grinds);
  RUN_TEST(test_polls_are_rate_limited);
  RUN_TEST(test_cancel_while_awaiting_payment);
  RUN_TEST(test_grind_clamped_to_max_run);
  RUN_TEST(test_millis_wraparound);
  RUN_TEST(test_counts_and_saves_paid_grinds);
  RUN_TEST(test_declined_payment_not_counted);
  RUN_TEST(test_settings_are_clamped_and_saved);
  RUN_TEST(test_settings_rejected_unless_idle);
  RUN_TEST(test_begin_loads_and_sanitizes_saved_settings);
  RUN_TEST(test_sale_logged_once_on_success_only);
  RUN_TEST(test_sale_json);
  RUN_TEST(test_milestone_shot_is_refunded);
  RUN_TEST(test_regular_shot_not_refunded);
  RUN_TEST(test_milestone_cleared_on_next_selection);
  RUN_TEST(test_milestone_list);
  RUN_TEST(test_shots_today_counts_and_resets_at_midnight);
  RUN_TEST(test_shots_today_survives_reboot);
  RUN_TEST(test_shots_today_without_clock_keeps_counting);
  RUN_TEST(test_best_day_is_kept_across_days);
  RUN_TEST(test_timeout_returns_to_start_quickly);
  RUN_TEST(test_declined_payment_waits_longer);
  RUN_TEST(test_reset_settings_restores_defaults_and_clears_counts);
  RUN_TEST(test_reset_settings_clears_all_stats);
  RUN_TEST(test_game_switch_is_saved_and_reset_turns_it_on);
  RUN_TEST(test_game_switch_only_while_idle);
  RUN_TEST(test_reset_settings_records_date);
  RUN_TEST(test_offline_blocks_new_payments);
  RUN_TEST(test_retry_while_offline_returns_to_idle);
  RUN_TEST(test_manual_grind_runs_until_stopped);
  RUN_TEST(test_manual_grind_capped_at_max_run);
  RUN_TEST(test_manual_grind_only_from_idle);
  return UNITY_END();
}
