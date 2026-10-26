#include <unity.h>

#include <cstring>

#include "Controller.h"
#include "FlappyGame.h"
#include "Leaderboard.h"

using namespace grinder;

namespace {

// --- Leaderboard ---

void fill(Leaderboard& lb, size_t n) {  // scores 100, 90, 80, ...
  for (size_t i = 0; i < n; ++i) lb.insert("P", 100 - 10 * i, 20261017);
}

void test_rank_empty_board() {
  Leaderboard lb;
  TEST_ASSERT_EQUAL(0, lb.rankFor(1));
  TEST_ASSERT_EQUAL(-1, lb.rankFor(0));  // 0 points never make the list
  TEST_ASSERT_EQUAL(-1, lb.insert("A", 0, 1));
  TEST_ASSERT_EQUAL_UINT32(0, lb.count);
}

void test_insert_keeps_order_and_ties_rank_below() {
  Leaderboard lb;
  TEST_ASSERT_EQUAL(0, lb.insert("A", 5, 1));
  TEST_ASSERT_EQUAL(0, lb.insert("B", 9, 2));
  TEST_ASSERT_EQUAL(2, lb.insert("C", 5, 3));  // tie: below the older 5
  TEST_ASSERT_EQUAL_UINT32(3, lb.count);
  TEST_ASSERT_EQUAL_STRING("B", lb.entries[0].name);
  TEST_ASSERT_EQUAL_STRING("A", lb.entries[1].name);
  TEST_ASSERT_EQUAL_STRING("C", lb.entries[2].name);
  TEST_ASSERT_EQUAL_UINT32(3, lb.entries[2].day);
}

void test_full_board_drops_last() {
  Leaderboard lb;
  fill(lb, 5);  // 100 .. 60
  TEST_ASSERT_EQUAL_UINT32(5, lb.count);
  TEST_ASSERT_EQUAL(-1, lb.rankFor(60));  // tie with the last: not enough
  TEST_ASSERT_EQUAL(-1, lb.insert("X", 5, 1));
  TEST_ASSERT_EQUAL(4, lb.insert("Y", 61, 1));
  TEST_ASSERT_EQUAL(0, lb.insert("Z", 500, 1));
  TEST_ASSERT_EQUAL_UINT32(5, lb.count);
  TEST_ASSERT_EQUAL_UINT32(500, lb.entries[0].score);
  TEST_ASSERT_EQUAL_UINT32(70, lb.entries[4].score);  // 60 and 61 fell off
}

void test_clean_name() {
  char out[ScoreEntry::kNameMax + 1];
  Leaderboard::cleanName("  jo  lu!z ", out);
  TEST_ASSERT_EQUAL_STRING("JO LUZ", out);
  Leaderboard::cleanName("ABCDEFGHIJKLMNOP", out);
  TEST_ASSERT_EQUAL_STRING("ABCDEFGHIJ", out);
  Leaderboard::cleanName("ABCDEFGHI JK", out);  // no trailing space at the limit
  TEST_ASSERT_EQUAL_STRING("ABCDEFGHI", out);
  Leaderboard::cleanName("?!", out);
  TEST_ASSERT_EQUAL_STRING("BEAN", out);
  Leaderboard::cleanName(nullptr, out);
  TEST_ASSERT_EQUAL_STRING("BEAN", out);
}

void test_sanitize_repairs_stored_data() {
  Leaderboard lb;
  fill(lb, 4);
  lb.count = 200;
  std::memset(lb.entries[1].name, 'q', sizeof lb.entries[1].name);  // unterminated
  lb.entries[3].score = 999;  // out of order: list ends before it
  lb.sanitize();
  TEST_ASSERT_EQUAL_UINT32(3, lb.count);
  TEST_ASSERT_EQUAL_STRING("QQQQQQQQQQ", lb.entries[1].name);
  TEST_ASSERT_EQUAL_UINT32(0, lb.entries[3].score);
}

// --- Controller ---

struct NullPayment : PaymentProvider {
  bool startCheckout(uint32_t, const char*) override { return true; }
  PaymentStatus pollStatus() override { return PaymentStatus::Pending; }
  void cancelCheckout() override {}
  void refundLastPayment() override {}
};
struct NullMotor : Motor {
  void setOn(bool) override {}
};
struct BoardStore : SettingsStore {
  Leaderboard saved;
  int saves = 0;
  void load(Dose*, size_t) override {}
  void save(const Dose*, size_t) override {}
  Stats savedStats;
  void saveStats(const Stats& s) override { savedStats = s; }
  void loadLeaderboard(Leaderboard& lb) override { lb = saved; }
  void saveLeaderboard(const Leaderboard& lb) override {
    saved = lb;
    ++saves;
  }
};
struct FixedClock : WallClock {
  bool known = true;
  bool now(LocalTime& out) override {
    out = {20261017, 12, 0};
    return known;
  }
};

void test_controller_submits_and_persists() {
  NullPayment pay;
  NullMotor motor;
  BoardStore store;
  store.saved.insert("old", 7, 20260101);
  FixedClock clock;
  Dose doses[1] = {{"Single", 50, 4000, 0}};
  Controller ctl(pay, motor, store, doses, 1);
  ctl.setWallClock(&clock);
  ctl.begin(0);
  TEST_ASSERT_EQUAL_UINT32(1, ctl.leaderboard().count);  // loaded

  TEST_ASSERT_EQUAL(0, ctl.leaderboardRank(8));
  TEST_ASSERT_EQUAL(0, ctl.submitScore("new", 8, 0));
  TEST_ASSERT_EQUAL(1, store.saves);
  TEST_ASSERT_EQUAL_STRING("NEW", store.saved.entries[0].name);
  TEST_ASSERT_EQUAL_UINT32(20261017, store.saved.entries[0].day);

  clock.known = false;
  TEST_ASSERT_EQUAL(2, ctl.submitScore("x", 3, 0));
  TEST_ASSERT_EQUAL_UINT32(0, ctl.leaderboard().entries[2].day);  // date unknown

  TEST_ASSERT_EQUAL(-1, ctl.submitScore("zero", 0, 0));
  TEST_ASSERT_EQUAL(2, store.saves);  // nothing saved for a rejected score
}

struct CountingPayment : NullPayment {
  int started = 0;
  bool startCheckout(uint32_t, const char*) override {
    ++started;
    return true;
  }
};
struct OnMotor : Motor {
  bool on = false;
  void setOn(bool v) override { on = v; }
};
struct Net : Connectivity {
  bool up = true;
  bool online() override { return up; }
};

struct RecordingLog : SalesLog {
  int sales = 0;
  bool refunded = false;
  void recordSale(const Dose&, bool r) override {
    ++sales;
    refunded = r;
  }
};

struct FreeShotFixture {
  RecordingLog log;
  CountingPayment pay;
  OnMotor motor;
  BoardStore store;
  Net net;
  Dose doses[1] = {{"Single", 50, 4000, 0}};
  Controller ctl{pay, motor, store, doses, 1};
  FreeShotFixture() {
    ctl.setConnectivity(&net);
    ctl.setSalesLog(&log);
    ctl.begin(0);
  }
};

void test_disabling_game_drops_free_shot() {
  FreeShotFixture f;
  f.ctl.submitScore("A", 12, 0);
  TEST_ASSERT_TRUE(f.ctl.freeShotAvailable(0));
  TEST_ASSERT_TRUE(f.ctl.setGameEnabled(false));
  TEST_ASSERT_FALSE(f.ctl.freeShotAvailable(0));
}

void test_top5_earns_one_free_shot() {
  FreeShotFixture f;
  f.ctl.submitScore("zero", 0, 0);  // 0 points never rank
  TEST_ASSERT_FALSE(f.ctl.freeShotAvailable(0));
  // Top 5 on an empty board, but below the minimum: listed, no coffee.
  TEST_ASSERT_EQUAL(0, f.ctl.submitScore("low", 9, 0));
  TEST_ASSERT_FALSE(f.ctl.earnsFreeShot(9));
  TEST_ASSERT_FALSE(f.ctl.freeShotAvailable(0));
  // Full board 19..15: a tie with #5 ranks below it and earns nothing.
  const char* names[] = {"A", "B", "C", "D", "E"};
  for (uint32_t i = 0; i < 5; ++i) f.ctl.submitScore(names[i], 19 - i, 0);
  TEST_ASSERT_TRUE(f.ctl.freeShotAvailable(0));
  const uint32_t t0 = f.ctl.config().freeShotMs;
  f.ctl.tick(t0);  // that one expires unused
  TEST_ASSERT_EQUAL(-1, f.ctl.submitScore("F", 15, t0));
  TEST_ASSERT_FALSE(f.ctl.freeShotAvailable(t0));
  TEST_ASSERT_EQUAL(4, f.ctl.submitScore("G", 16, t0 + 1000));  // below the older 16: last place counts
  TEST_ASSERT_TRUE(f.ctl.freeShotAvailable(t0 + 1000));

  const uint32_t t1 = t0 + 2000;
  f.net.up = false;  // needs no payment, so it works offline too
  f.ctl.selectDose(0, t1);
  TEST_ASSERT_TRUE(f.ctl.state() == State::Grinding);
  TEST_ASSERT_TRUE(f.motor.on);
  TEST_ASSERT_TRUE(f.ctl.freeGrind());
  TEST_ASSERT_EQUAL(0, f.pay.started);
  TEST_ASSERT_EQUAL_UINT32(0, f.doses[0].count);  // not a paid shot
  TEST_ASSERT_EQUAL(1, f.log.sales);  // but logged, as refunded
  TEST_ASSERT_TRUE(f.log.refunded);
  TEST_ASSERT_FALSE(f.ctl.freeShotAvailable(t1));  // used up

  for (uint32_t t = t1; t <= t1 + 10000; t += 100) f.ctl.tick(t);  // grind, Done, Idle
  TEST_ASSERT_TRUE(f.ctl.state() == State::Idle);
  f.net.up = true;
  f.ctl.selectDose(0, t1 + 11000);  // back to paying
  TEST_ASSERT_TRUE(f.ctl.state() == State::AwaitingPayment);
  TEST_ASSERT_FALSE(f.ctl.freeGrind());
  TEST_ASSERT_EQUAL(1, f.pay.started);
}

void test_free_shot_expires() {
  FreeShotFixture f;
  f.ctl.submitScore("A", 15, 0);
  f.ctl.submitScore("B", 19, 0);
  const uint32_t ms = f.ctl.config().freeShotMs;
  TEST_ASSERT_EQUAL_UINT32(ms, f.ctl.freeShotRemainingMs(0));
  f.ctl.tick(ms);
  TEST_ASSERT_FALSE(f.ctl.freeShotAvailable(ms));
  f.ctl.selectDose(0, ms);
  TEST_ASSERT_TRUE(f.ctl.state() == State::AwaitingPayment);  // pays again
}

void test_reset_clears_leaderboard_and_free_shot() {
  FreeShotFixture f;
  f.ctl.submitScore("A", 15, 0);
  f.ctl.submitScore("B", 19, 0);
  TEST_ASSERT_TRUE(f.ctl.resetSettings());
  TEST_ASSERT_EQUAL_UINT32(0, f.ctl.leaderboard().count);
  TEST_ASSERT_EQUAL_UINT32(0, f.store.saved.count);  // persisted
  TEST_ASSERT_FALSE(f.ctl.freeShotAvailable(0));
}

void test_stats_count_games_free_shots_and_grind_time() {
  FreeShotFixture f;
  f.ctl.recordGamePlayed();
  f.ctl.recordGamePlayed();
  TEST_ASSERT_EQUAL_UINT32(2, f.ctl.gamesPlayed());

  f.ctl.submitScore("A", 15, 0);
  f.ctl.submitScore("B", 19, 0);
  f.ctl.selectDose(0, 1000);  // free shot, 4 s
  for (uint32_t t = 1000; t <= 12000; t += 100) f.ctl.tick(t);
  TEST_ASSERT_EQUAL_UINT32(1, f.ctl.freeShotsUsed());
  TEST_ASSERT_UINT32_WITHIN(100, 4000, f.ctl.totalGrindMs());

  f.ctl.startManualGrind(20000);  // manual runs count too
  f.ctl.stopManualGrind(21500);
  TEST_ASSERT_UINT32_WITHIN(100, 5500, f.ctl.totalGrindMs());
  TEST_ASSERT_EQUAL_UINT32(f.ctl.totalGrindMs(), f.store.savedStats.grindMs);  // persisted

  TEST_ASSERT_TRUE(f.ctl.resetSettings());
  TEST_ASSERT_EQUAL_UINT32(0, f.ctl.gamesPlayed());
  TEST_ASSERT_EQUAL_UINT32(0, f.ctl.freeShotsUsed());
  TEST_ASSERT_EQUAL_UINT32(0, f.ctl.totalGrindMs());
  TEST_ASSERT_EQUAL_UINT32(0, f.store.savedStats.grindMs);  // saved without a clock too
}

// --- Game ---

void run(FlappyGame& g, uint32_t ms) {
  for (uint32_t t = 0; t < ms; t += 16) g.step(16);
}

// Flaps whenever the bean sinks below the opening it's heading for.
void autopilot(FlappyGame& g, uint32_t ms) {
  const FlappyConfig& c = g.config();
  for (uint32_t t = 0; t < ms && g.phase() != FlappyGame::Phase::Dead; t += 16) {
    float target = c.height / 2;
    float nearest = 1e9f;
    for (size_t i = 0; i < FlappyGame::kJars; ++i) {
      const auto& j = g.jar(i);
      if (j.x + c.jarWidth > c.beanX - c.beanRadius && j.x < nearest) {
        nearest = j.x;
        target = j.gapY + 18;
      }
    }
    if (g.beanY() > target && g.beanVy() > 0) g.flap();
    g.step(16);
  }
}

void test_ready_hovers_until_first_tap() {
  FlappyGame g;
  g.reset(42);
  run(g, 5000);
  TEST_ASSERT_TRUE(g.phase() == FlappyGame::Phase::Ready);
  TEST_ASSERT_FLOAT_WITHIN(5, g.config().height / 2, g.beanY());
  g.flap();
  TEST_ASSERT_TRUE(g.phase() == FlappyGame::Phase::Playing);
  TEST_ASSERT_TRUE(g.beanVy() < 0);
}

void test_no_taps_falls_to_ground() {
  FlappyGame g;
  g.reset(42);
  g.flap();
  run(g, 3000);
  TEST_ASSERT_TRUE(g.phase() == FlappyGame::Phase::Dead);
  TEST_ASSERT_TRUE(g.landed());
  TEST_ASSERT_EQUAL_UINT32(0, g.score());
  TEST_ASSERT_FLOAT_WITHIN(0.01f, g.config().height - g.config().beanRadius, g.beanY());
  float y = g.beanY();
  g.flap();  // ignored once dead
  run(g, 500);
  TEST_ASSERT_EQUAL_FLOAT(y, g.beanY());
}

void test_flying_through_gaps_scores() {
  FlappyGame g;
  g.reset(7);
  g.flap();
  autopilot(g, 60000);
  TEST_ASSERT_TRUE(g.score() >= 20);
}

void test_hovering_at_ceiling_hits_jar() {
  FlappyGame g;
  g.reset(3);
  for (int i = 0; i < 400 && g.phase() != FlappyGame::Phase::Dead; ++i) {
    g.flap();  // keeps the bean at the top, where every upper jar reaches
    g.step(50);
  }
  TEST_ASSERT_TRUE(g.phase() == FlappyGame::Phase::Dead);
  TEST_ASSERT_FALSE(g.landed());  // hit a jar, now falls
  run(g, 3000);
  TEST_ASSERT_TRUE(g.landed());
}

void test_openings_stay_in_bounds_and_same_seed_repeats() {
  FlappyGame a, b;
  a.reset(99);
  b.reset(99);
  a.flap();
  b.flap();
  autopilot(a, 20000);
  autopilot(b, 20000);
  TEST_ASSERT_EQUAL_UINT32(a.score(), b.score());
  const FlappyConfig& c = a.config();
  for (size_t i = 0; i < FlappyGame::kJars; ++i) {
    TEST_ASSERT_EQUAL_FLOAT(a.jar(i).gapY, b.jar(i).gapY);
    TEST_ASSERT_TRUE(a.jar(i).gapY - c.gap / 2 >= c.gapMargin - 0.01f);
    TEST_ASSERT_TRUE(a.jar(i).gapY + c.gap / 2 <= c.height - c.gapMargin + 0.01f);
  }
}

}  // namespace

void setUp() {}
void tearDown() {}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_rank_empty_board);
  RUN_TEST(test_insert_keeps_order_and_ties_rank_below);
  RUN_TEST(test_full_board_drops_last);
  RUN_TEST(test_clean_name);
  RUN_TEST(test_sanitize_repairs_stored_data);
  RUN_TEST(test_controller_submits_and_persists);
  RUN_TEST(test_top5_earns_one_free_shot);
  RUN_TEST(test_free_shot_expires);
  RUN_TEST(test_disabling_game_drops_free_shot);
  RUN_TEST(test_reset_clears_leaderboard_and_free_shot);
  RUN_TEST(test_stats_count_games_free_shots_and_grind_time);
  RUN_TEST(test_ready_hovers_until_first_tap);
  RUN_TEST(test_no_taps_falls_to_ground);
  RUN_TEST(test_flying_through_gaps_scores);
  RUN_TEST(test_hovering_at_ceiling_hits_jar);
  RUN_TEST(test_openings_stay_in_bounds_and_same_seed_repeats);
  return UNITY_END();
}
