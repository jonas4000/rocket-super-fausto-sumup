// ESP32-S3 firmware: the touchscreen UI (src/ui) on the ST7789 display, plus
// single-key serial commands for testing. Payments go to the SumUp Solo via
// the Cloud API when secrets.h has the SumUp values; otherwise (or with
// -DFAKE_PAYMENT) a serial fake stands in. Paid sales are sent to Supabase.

#include <Arduino.h>
#include <Preferences.h>
#include <WiFi.h>

#include <cstdio>
#include <ctime>

#include "Controller.h"
#include "Display.h"
#include "Doses.h"
#include "SumUpHttp.h"
#include "SupabaseLog.h"
#include "Ui.h"
#include "pins.h"

#if __has_include("secrets.h")
#include "secrets.h"
#define HAVE_SECRETS 1
#else
#warning "include/secrets.h missing: Wi-Fi and Supabase disabled (see secrets.example.h)"
#define HAVE_SECRETS 0
#endif

#if HAVE_SECRETS && defined(SUMUP_AFFILIATE_KEY) && !defined(FAKE_PAYMENT)
#define USE_SUMUP 1
#else
#define USE_SUMUP 0
#endif

using namespace grinder;

namespace {

// Settings PIN from secrets.h; the fallback is only for builds without one.
#ifdef SETTINGS_PIN
static_assert(sizeof(SETTINGS_PIN) == 5, "SETTINGS_PIN must be 4 digits");
constexpr const char* kSettingsPin = SETTINGS_PIN;
#else
constexpr const char* kSettingsPin = "1234";
#endif

// POSIX TZ string from secrets.h; UTC without one.
#ifdef TIME_ZONE
constexpr const char* kTimeZone = TIME_ZONE;
#else
constexpr const char* kTimeZone = "UTC0";
#endif

uint8_t relayLevel(bool on) { return on == kRelayActiveHigh ? HIGH : LOW; }

class RelayMotor : public Motor {
 public:
  void setOn(bool on) override {
    if (kRelayPin >= 0) digitalWrite(kRelayPin, relayLevel(on));
    if (on != on_) Serial.println(on ? "MOTOR ON" : "MOTOR OFF");
    on_ = on;
  }

 private:
  bool on_ = false;
};

// Stand-in without SumUp credentials: 'a' approves, 'd' declines.
class SerialTestPayment : public PaymentProvider {
 public:
  PaymentStatus next = PaymentStatus::Pending;

  bool startCheckout(uint32_t amountCents, const char* description) override {
    Serial.printf("checkout %u.%02u EUR \"%s\" (a=approve, d=decline)\n",
                  amountCents / 100, amountCents % 100, description);
    next = PaymentStatus::Pending;
    return true;
  }
  PaymentStatus pollStatus() override { return next; }
  void cancelCheckout() override { Serial.println("checkout cancelled"); }
  void refundLastPayment() override { Serial.println("refund last payment (milestone)"); }
};

// Price, grind time and count per dose in NVS (survives reboot and reflash).
class NvsStore : public SettingsStore {
 public:
  void load(Dose* doses, size_t count) override {
    Preferences prefs;
    if (!prefs.begin(kNamespace, true)) return;  // nothing saved yet
    for (size_t i = 0; i < count; ++i) {
      doses[i].priceCents = prefs.getUInt(key("price", i), doses[i].priceCents);
      doses[i].grindMs = prefs.getUInt(key("grind", i), doses[i].grindMs);
      doses[i].count = prefs.getUInt(key("count", i), doses[i].count);
    }
    prefs.end();
  }

  void save(const Dose* doses, size_t count) override {
    Preferences prefs;
    if (!prefs.begin(kNamespace, false)) return;
    for (size_t i = 0; i < count; ++i) {
      prefs.putUInt(key("price", i), doses[i].priceCents);
      prefs.putUInt(key("grind", i), doses[i].grindMs);
      prefs.putUInt(key("count", i), doses[i].count);
    }
    prefs.end();
  }

  void loadStats(Stats& s) override {
    Preferences prefs;
    if (!prefs.begin(kNamespace, true)) return;
    s.day = prefs.getUInt("day", s.day);
    s.today = prefs.getUInt("dayShots", s.today);
    s.bestDay = prefs.getUInt("bestDay", s.bestDay);
    s.lastReset = prefs.getUInt("lastReset", s.lastReset);
    s.gamesPlayed = prefs.getUInt("games", s.gamesPlayed);
    s.freeShots = prefs.getUInt("freeShots", s.freeShots);
    s.grindMs = prefs.getUInt("grindMs", s.grindMs);
    prefs.end();
  }

  void saveStats(const Stats& s) override {
    Preferences prefs;
    if (!prefs.begin(kNamespace, false)) return;
    prefs.putUInt("day", s.day);
    prefs.putUInt("dayShots", s.today);
    prefs.putUInt("bestDay", s.bestDay);
    prefs.putUInt("lastReset", s.lastReset);
    prefs.putUInt("games", s.gamesPlayed);
    prefs.putUInt("freeShots", s.freeShots);
    prefs.putUInt("grindMs", s.grindMs);
    prefs.end();
  }

  // Flappy Beans top 5 as one blob. The key carries a layout version: bump
  // it if Leaderboard changes, so old blobs are ignored instead of misread.
  void loadLeaderboard(Leaderboard& lb) override {
    Preferences prefs;
    if (!prefs.begin(kNamespace, true)) return;
    if (prefs.getBytesLength(kLeaderboardKey) == sizeof lb)
      prefs.getBytes(kLeaderboardKey, &lb, sizeof lb);
    prefs.end();
  }

  void saveLeaderboard(const Leaderboard& lb) override {
    Preferences prefs;
    if (!prefs.begin(kNamespace, false)) return;
    prefs.putBytes(kLeaderboardKey, &lb, sizeof lb);
    prefs.end();
  }

  void loadGameEnabled(bool& on) override {
    Preferences prefs;
    if (!prefs.begin(kNamespace, true)) return;
    on = prefs.getBool("game", on);
    prefs.end();
  }

  void saveGameEnabled(bool on) override {
    Preferences prefs;
    if (!prefs.begin(kNamespace, false)) return;
    prefs.putBool("game", on);
    prefs.end();
  }

 private:
  static constexpr const char* kNamespace = "grinder";
  static constexpr const char* kLeaderboardKey = "flappy2";  // 2: top 5
  const char* key(const char* field, size_t i) {
    std::snprintf(keyBuf_, sizeof keyBuf_, "%s%u", field, static_cast<unsigned>(i));
    return keyBuf_;
  }
  char keyBuf_[16];
};

// Local time from NTP (see configTzTime in setup); unknown until first sync.
class NtpClock : public WallClock {
 public:
  bool now(LocalTime& out) override {
    time_t now = time(nullptr);
    if (now < 1700000000) return false;  // clock not set yet
    struct tm t;
    localtime_r(&now, &t);
    out.day = (t.tm_year + 1900) * 10000 + (t.tm_mon + 1) * 100 + t.tm_mday;
    out.hour = t.tm_hour;
    out.minute = t.tm_min;
    return true;
  }
};

#if USE_SUMUP
// Online while Wi-Fi is connected. A drop shorter than kGraceMs (a reconnect)
// doesn't count, so the doses don't flicker; until the first connection after
// boot it is offline.
class WifiConnectivity : public Connectivity {
 public:
  bool online() override {
    const uint32_t now = millis();
    if (WiFi.isConnected()) {
      everConnected_ = true;
      lastConnectedMs_ = now;
      return true;
    }
    return everConnected_ && now - lastConnectedMs_ < kGraceMs;
  }

 private:
  static constexpr uint32_t kGraceMs = 5000;
  bool everConnected_ = false;
  uint32_t lastConnectedMs_ = 0;
};
#endif

RelayMotor motor;
NtpClock wallClock;
#if USE_SUMUP
SumUpPayment payment({SUMUP_API_KEY, SUMUP_MERCHANT_CODE, SUMUP_READER_ID,
                      SUMUP_AFFILIATE_APP_ID, SUMUP_AFFILIATE_KEY},
                     esp_random(), [](const std::string& msg) { Serial.println(msg.c_str()); });
#else
SerialTestPayment payment;
#endif
NvsStore store;
#if HAVE_SECRETS
SupabaseLog sales(SUPABASE_URL, SUPABASE_KEY);
#else
SupabaseLog sales(nullptr, nullptr);
#endif
Controller controller(payment, motor, store, gDoses, kDoseCount);
Ui ui(controller, [] { return static_cast<uint32_t>(millis()); }, kSettingsPin);
State lastState = State::Idle;
bool lastOnline = true;

void handleSerial(uint32_t now) {
  while (Serial.available()) {
    char c = Serial.read();
    if (c >= '1' && c <= '9') controller.selectDose(c - '1', now);
    else if (c == 'c') controller.cancel(now);
#if !USE_SUMUP
    else if (c == 'a') payment.next = PaymentStatus::Succeeded;
    else if (c == 'd') payment.next = PaymentStatus::Failed;
#endif
  }
}

}  // namespace

void setup() {
  // Motor off before anything else. Core 3 ignores digitalWrite before
  // pinMode; the pin is an input (relay off via its pull-down) until here.
  if (kRelayPin >= 0) {
    pinMode(kRelayPin, OUTPUT);
    digitalWrite(kRelayPin, relayLevel(false));
  }

  Serial.begin(115200);
  // Reboots the chip if loop() stops returning; the relay is off after reset.
  enableLoopWDT();

#if HAVE_SECRETS
  // Connects in the background and reconnects automatically.
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  // Local time for the daily shot count and leaderboard dates.
  configTzTime(kTimeZone, "pool.ntp.org", "time.google.com");
#endif
  sales.begin();
#if USE_SUMUP
  startSumUpTask(payment);
#endif

  controller.setSalesLog(&sales);
  controller.setWallClock(&wallClock);
#if USE_SUMUP
  // Only SumUp needs the network; the serial fake works offline.
  static WifiConnectivity connectivity;
  controller.setConnectivity(&connectivity);
#endif
  controller.begin(millis());

  lv_init();
  lv_tick_set_cb([] { return static_cast<uint32_t>(millis()); });
  lv_delay_set_cb([](uint32_t ms) { delay(ms); });
  displayBegin();
  ui.seedRandom(esp_random());
  ui.begin(lv_screen_active());
  lv_refr_now(nullptr);  // first frame drawn before the backlight comes on
  displayBacklight(true);

#if USE_SUMUP
  Serial.println("grinder ready: 1-2 select + pay on the SumUp Solo, c cancel");
#else
  Serial.println("grinder ready: 1-2 select+pay, a/d approve/decline, c cancel");
#endif
}

void loop() {
  uint32_t now = millis();
  handleSerial(now);
  controller.tick(now);
  if (controller.state() != lastState) {
    lastState = controller.state();
    Serial.printf("state -> %s\n", stateName(lastState));
  }
  if (controller.online() != lastOnline) {
    lastOnline = !lastOnline;
    Serial.println(lastOnline ? "online" : "offline: no new payments");
  }
  ui.update();
  const uint32_t wait = lv_timer_handler();
  delay(wait < 1 ? 1 : wait > 5 ? 5 : wait);
}
