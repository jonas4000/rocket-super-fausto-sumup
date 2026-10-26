// Desktop display simulator: the real Controller and Ui in an SDL window.
// Left: the 320x240 device screen (mouse = finger). Right: a fake SumUp
// reader with Approve / Decline buttons, a motor indicator and a Wi-Fi switch
// (off = the device is offline; with --sumup, API calls then fail like on the
// ESP32 without Wi-Fi).
// With --sumup, the real Solo from include/secrets.h takes the payments
// instead (real money; SumUpPayment over libcurl in a background thread), and
// the panel shows the latest SumUp log line. Sales then also go to Supabase.
// Settings and counters persist in .pio/ui-sim-settings.txt.

#include <SDL2/SDL.h>
#include <lvgl.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "Controller.h"
#include "Doses.h"
#include "SaleJson.h"
#include "Ui.h"

#if __has_include("secrets.h")
#include "CurlHttp.h"
#include "SumUp.h"
#include "secrets.h"
#define HAVE_SUMUP 1
#else
#define HAVE_SUMUP 0
#endif
#if HAVE_SUMUP && defined(SUPABASE_URL) && defined(SUPABASE_KEY)
#define HAVE_SUPABASE 1
#else
#define HAVE_SUPABASE 0
#endif

using namespace grinder;

namespace {

constexpr int32_t kScreenW = 320;
constexpr int32_t kScreenH = 240;
constexpr int32_t kPanelW = 140;
constexpr const char* kSettingsPin = "1234";
constexpr const char* kStorePath = ".pio/ui-sim-settings.txt";
constexpr const char* kStatsPath = ".pio/ui-sim-stats.txt";
constexpr const char* kFlappyPath = ".pio/ui-sim-flappy.txt";
constexpr const char* kGamePath = ".pio/ui-sim-game.txt";  // 1 = game on, 0 = off

uint32_t nowMs() { return SDL_GetTicks(); }

// The panel's Wi-Fi switch.
std::atomic<bool> gOnline{true};

// The Mac's local time.
class SystemClock : public WallClock {
 public:
  bool now(LocalTime& out) override {
    std::time_t t = std::time(nullptr);
    const std::tm* lt = std::localtime(&t);
    out.day = (lt->tm_year + 1900) * 10000 + (lt->tm_mon + 1) * 100 + lt->tm_mday;
    out.hour = lt->tm_hour;
    out.minute = lt->tm_min;
    return true;
  }
};

class FakeReader : public PaymentProvider {
 public:
  PaymentStatus next = PaymentStatus::Pending;
  lv_obj_t* status = nullptr;

  bool startCheckout(uint32_t amountCents, const char*) override {
    next = PaymentStatus::Pending;
    char buf[48];
    std::snprintf(buf, sizeof buf, "Checkout\n%u.%02u EUR", amountCents / 100,
                  amountCents % 100);
    lv_label_set_text(status, buf);
    return true;
  }
  PaymentStatus pollStatus() override { return next; }
  void cancelCheckout() override { lv_label_set_text(status, "Cancelled"); }
  void refundLastPayment() override {
    lv_label_set_text(status, "Refunded\n(milestone)");
    std::printf("[%7.1fs] reader: refund last payment (milestone)\n", nowMs() / 1000.0);
    std::fflush(stdout);
  }
};

class FakeMotor : public Motor {
 public:
  lv_obj_t* led = nullptr;
  void setOn(bool on) override {
    if (led) on ? lv_led_on(led) : lv_led_off(led);
    std::printf("[%7.1fs] MOTOR %s\n", nowMs() / 1000.0, on ? "ON" : "off");
    std::fflush(stdout);
  }
};

// One line per dose: price grindMs count.
class FileStore : public SettingsStore {
 public:
  void load(Dose* doses, size_t count) override {
    FILE* f = std::fopen(kStorePath, "r");
    if (!f) return;
    for (size_t i = 0; i < count; ++i) {
      unsigned price, grind, sold;
      if (std::fscanf(f, "%u %u %u", &price, &grind, &sold) != 3) break;
      doses[i].priceCents = price;
      doses[i].grindMs = grind;
      doses[i].count = sold;
    }
    std::fclose(f);
  }
  void save(const Dose* doses, size_t count) override {
    FILE* f = std::fopen(kStorePath, "w");
    if (!f) return;
    for (size_t i = 0; i < count; ++i)
      std::fprintf(f, "%u %u %u\n", doses[i].priceCents, doses[i].grindMs,
                   doses[i].count);
    std::fclose(f);
  }
  // One line: day today bestDay lastReset gamesPlayed freeShots
  // grindMs (missing trailing fields stay 0).
  void loadStats(Stats& s) override {
    FILE* f = std::fopen(kStatsPath, "r");
    if (!f) return;
    unsigned v[7] = {};
    int n = std::fscanf(f, "%u %u %u %u %u %u %u", &v[0], &v[1], &v[2], &v[3], &v[4],
                        &v[5], &v[6]);
    if (n >= 2) s = {v[0], v[1], v[2], v[3], v[4], v[5], v[6]};
    std::fclose(f);
  }
  void saveStats(const Stats& s) override {
    FILE* f = std::fopen(kStatsPath, "w");
    if (!f) return;
    std::fprintf(f, "%u %u %u %u %u %u %u\n", s.day, s.today, s.bestDay, s.lastReset,
                 s.gamesPlayed, s.freeShots, s.grindMs);
    std::fclose(f);
  }
  // Flappy Beans top 5, one line per entry: score day name (name last, it
  // may contain spaces). The controller sanitizes what is loaded.
  void loadLeaderboard(Leaderboard& lb) override {
    FILE* f = std::fopen(kFlappyPath, "r");
    if (!f) return;
    lb = Leaderboard();
    unsigned score, day;
    char name[64];
    while (lb.count < Leaderboard::kSize &&
           std::fscanf(f, "%u %u %63[^\n]", &score, &day, name) == 3) {
      ScoreEntry& e = lb.entries[lb.count++];
      std::snprintf(e.name, sizeof e.name, "%s", name);
      e.score = score;
      e.day = day;
    }
    std::fclose(f);
  }
  void saveLeaderboard(const Leaderboard& lb) override {
    FILE* f = std::fopen(kFlappyPath, "w");
    if (!f) return;
    for (size_t i = 0; i < lb.count; ++i)
      std::fprintf(f, "%u %u %s\n", lb.entries[i].score, lb.entries[i].day,
                   lb.entries[i].name);
    std::fclose(f);
  }
  void loadGameEnabled(bool& on) override {
    FILE* f = std::fopen(kGamePath, "r");
    if (!f) return;
    unsigned v;
    if (std::fscanf(f, "%u", &v) == 1) on = v != 0;
    std::fclose(f);
  }
  void saveGameEnabled(bool on) override {
    FILE* f = std::fopen(kGamePath, "w");
    if (!f) return;
    std::fprintf(f, "%u\n", on ? 1u : 0u);
    std::fclose(f);
  }
};

void logLine(const std::string& msg) {
  std::printf("[%7.1fs] %s\n", nowMs() / 1000.0, msg.c_str());
  std::fflush(stdout);
}

// Prints each sale; with --sumup it also inserts it into Supabase like the
// firmware (background thread, retried every 10 s on network errors, 408, 429
// and 5xx, other errors dropped). Fake payments are never sent.
class PrintSalesLog : public SalesLog {
 public:
  bool send = false;

  void recordSale(const Dose& d, bool refunded) override {
    char json[192];
    formatSaleJson(json, sizeof json, d, refunded);
    logLine(std::string("supabase: POST /rest/v1/coffee_log ") + json);
#if HAVE_SUPABASE
    if (send) std::thread(post, std::string(json)).detach();
#endif
  }

 private:
#if HAVE_SUPABASE
  static constexpr int kTries = 6;
  static constexpr uint32_t kRetryMs = 10000;

  static void post(std::string json) {
    const std::string key = SUPABASE_KEY;
    std::vector<std::string> headers = {"apikey: " + key, "Prefer: return=minimal"};
    // Legacy anon keys (JWTs) also go in Authorization; sb_publishable_ keys must not.
    if (key.compare(0, 3, "sb_") != 0) headers.push_back("Authorization: Bearer " + key);
    for (int i = 0; i < kTries; ++i) {
      HttpResponse r = gOnline ? curlRequest("POST", std::string(SUPABASE_URL) +
                                                         "/rest/v1/coffee_log",
                                             headers, json)
                               : HttpResponse{-1, "Wi-Fi off"};
      if (r.status >= 200 && r.status < 300) {
        logLine("supabase: sale sent");
        return;
      }
      logLine("supabase: HTTP " + std::to_string(r.status) + " " + r.body);
      if (r.status > 0 && r.status != 408 && r.status != 429 && r.status < 500) return;
      SDL_Delay(kRetryMs);
    }
    logLine("supabase: giving up, sale not logged");
  }
#endif
};

FakeReader reader;
FakeMotor motor;

class SimConnectivity : public Connectivity {
 public:
  bool online() override { return gOnline; }
};
SimConnectivity connectivity;

// Latest SumUp log line, written by the background thread and shown on the
// panel by the main loop (LVGL is not thread-safe).
std::mutex gLogMutex;
std::string gLogLine;
std::atomic<unsigned> gLogVersion{0};
PrintSalesLog sales;
FileStore store;

lv_obj_t* panelButton(lv_obj_t* parent, const char* text, lv_color_t color,
                      PaymentStatus result) {
  lv_obj_t* b = lv_button_create(parent);
  lv_obj_set_width(b, lv_pct(100));
  lv_obj_set_style_bg_color(b, color, 0);
  lv_obj_add_event_cb(
      b,
      [](lv_event_t* e) {
        auto r = static_cast<PaymentStatus>(
            reinterpret_cast<intptr_t>(lv_event_get_user_data(e)));
        reader.next = r;
        lv_label_set_text(reader.status,
                          r == PaymentStatus::Succeeded ? "Approved" : "Declined");
      },
      LV_EVENT_CLICKED, reinterpret_cast<void*>(static_cast<intptr_t>(result)));
  lv_obj_t* l = lv_label_create(b);
  lv_label_set_text(l, text);
  lv_obj_center(l);
  return b;
}

void buildReaderPanel(lv_obj_t* screen, bool real) {
  lv_obj_t* p = lv_obj_create(screen);
  lv_obj_set_size(p, kPanelW, kScreenH);
  lv_obj_set_pos(p, kScreenW, 0);
  lv_obj_set_style_radius(p, 0, 0);
  lv_obj_set_style_border_width(p, 0, 0);
  lv_obj_set_style_bg_color(p, lv_color_hex(0xDDDDDD), 0);
  lv_obj_set_style_pad_all(p, 8, 0);
  lv_obj_set_style_pad_row(p, 8, 0);
  lv_obj_set_flex_flow(p, LV_FLEX_FLOW_COLUMN);
  lv_obj_remove_flag(p, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t* title = lv_label_create(p);
  lv_label_set_text(title, real ? "REAL PAYMENTS\nSumUp Solo" : "SIMULATION\nSumUp reader");
  if (real) lv_obj_set_style_text_color(title, lv_color_hex(0xB03A2E), 0);

  reader.status = lv_label_create(p);
  lv_obj_set_width(reader.status, lv_pct(100));
  lv_label_set_long_mode(reader.status, LV_LABEL_LONG_WRAP);
  lv_label_set_text(reader.status, real ? "Pay on the Solo" : "Idle");

  if (!real) {
    panelButton(p, "Approve", lv_color_hex(0x3A9D5D), PaymentStatus::Succeeded);
    panelButton(p, "Decline", lv_color_hex(0xB03A2E), PaymentStatus::Failed);
  }

  lv_obj_t* row = lv_obj_create(p);
  lv_obj_remove_style_all(row);
  lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(row, 8, 0);
  motor.led = lv_led_create(row);
  lv_obj_set_size(motor.led, 16, 16);
  lv_led_set_color(motor.led, lv_color_hex(0xFF8800));
  lv_led_off(motor.led);
  lv_obj_t* l = lv_label_create(row);
  lv_label_set_text(l, "Motor");

  lv_obj_t* net = lv_obj_create(p);
  lv_obj_remove_style_all(net);
  lv_obj_set_size(net, lv_pct(100), LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(net, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(net, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(net, 8, 0);
  lv_obj_t* sw = lv_switch_create(net);
  lv_obj_set_size(sw, 36, 20);
  lv_obj_add_state(sw, LV_STATE_CHECKED);
  lv_obj_add_event_cb(
      sw,
      [](lv_event_t* e) {
        gOnline = lv_obj_has_state(lv_event_get_target_obj(e), LV_STATE_CHECKED);
        std::printf("[%7.1fs] Wi-Fi %s\n", nowMs() / 1000.0, gOnline ? "on" : "OFF");
        std::fflush(stdout);
      },
      LV_EVENT_VALUE_CHANGED, nullptr);
  lv_obj_t* wl = lv_label_create(net);
  lv_label_set_text(wl, "Wi-Fi");
}

}  // namespace

int main(int argc, char** argv) {
  const bool real = argc > 1 && !std::strcmp(argv[1], "--sumup");
#if !HAVE_SUMUP
  if (real) {
    std::fprintf(stderr, "--sumup needs include/secrets.h (see secrets.example.h)\n");
    return 1;
  }
#endif

  lv_init();
  lv_tick_set_cb(SDL_GetTicks);

  lv_display_t* disp = lv_sdl_window_create(kScreenW + kPanelW, kScreenH);
  lv_sdl_window_set_zoom(disp, 2);
  lv_sdl_window_set_title(disp, real ? "Coffee grinder simulator - REAL SumUp payments - PIN 1234"
                                      : "Coffee grinder simulator - PIN 1234");
  lv_sdl_mouse_create();

  lv_obj_t* screen = lv_screen_active();
  lv_obj_t* device = lv_obj_create(screen);
  lv_obj_set_size(device, kScreenW, kScreenH);
  lv_obj_set_pos(device, 0, 0);
  lv_obj_set_style_radius(device, 0, 0);
  lv_obj_set_style_border_width(device, 0, 0);

  buildReaderPanel(screen, real);

  PaymentProvider* payment = &reader;
#if HAVE_SUMUP
  // Same timing as the device: the controller polls every 2 s, the thread
  // makes the API calls.
  static SumUpPayment sumup(
      {SUMUP_API_KEY, SUMUP_MERCHANT_CODE, SUMUP_READER_ID, SUMUP_AFFILIATE_APP_ID,
       SUMUP_AFFILIATE_KEY},
      std::random_device{}(), [](const std::string& msg) {
        std::printf("[%7.1fs] %s\n", nowMs() / 1000.0, msg.c_str());
        std::fflush(stdout);
        std::lock_guard<std::mutex> lock(gLogMutex);
        gLogLine = msg.substr(0, 90);
        ++gLogVersion;
      });
  if (real) {
    curl_global_init(CURL_GLOBAL_DEFAULT);
    payment = &sumup;
    sales.send = HAVE_SUPABASE;
    std::thread([] {
      // Fails like ArduinoHttps while the panel's Wi-Fi switch is off.
      struct SimHttp : CurlHttp {
        HttpResponse send(const char* method, const std::string& url, const std::string& bearer,
                          const std::string& body) override {
          if (!gOnline) return {-1, "Wi-Fi not connected"};
          return CurlHttp::send(method, url, bearer, body);
        }
      } http;
      for (;;) {
        while (sumup.work(http, nowMs())) {
        }
        SDL_Delay(50);
      }
    }).detach();
    std::printf("REAL SumUp payments on reader %s\n", SUMUP_READER_ID);
  }
#endif

  SystemClock wallClock;
  Controller ctl(*payment, motor, store, gDoses, kDoseCount);
  ctl.setSalesLog(&sales);
  ctl.setWallClock(&wallClock);
  ctl.setConnectivity(&connectivity);
  ctl.begin(nowMs());
  Ui ui(ctl, nowMs, kSettingsPin);
  ui.seedRandom(std::random_device{}());
  ui.begin(device);

  State last = ctl.state();
  while (true) {
    ctl.tick(nowMs());
    if (ctl.state() != last) {
      last = ctl.state();
      std::printf("[%7.1fs] state -> %s\n", nowMs() / 1000.0, stateName(last));
      std::fflush(stdout);
    }
    ui.update();
    static unsigned shownLog = 0;
    if (gLogVersion != shownLog) {
      shownLog = gLogVersion;
      std::lock_guard<std::mutex> lock(gLogMutex);
      lv_label_set_text(reader.status, gLogLine.c_str());
    }
    uint32_t wait = lv_timer_handler();
    SDL_Delay(wait < 10 ? wait : 10);
  }
}
