// Terminal simulator: runs the real Controller against a fake card reader and
// a fake motor with a simulated clock. Reads commands from stdin, so it can be
// used interactively or scripted:  printf "select 1\npay\napprove\nwait 10\n" | program

#include <cstdio>
#include <iostream>
#include <sstream>
#include <string>

#include "Controller.h"
#include "Doses.h"
#include "SaleJson.h"

using namespace grinder;

namespace {

uint32_t gNowMs = 0;

void log(const std::string& msg) {
  std::printf("[%7.1fs] %s\n", gNowMs / 1000.0, msg.c_str());
}

class FakeReader : public PaymentProvider {
 public:
  PaymentStatus next = PaymentStatus::Pending;
  bool active = false;

  bool startCheckout(uint32_t amountCents, const char* description) override {
    char buf[96];
    std::snprintf(buf, sizeof buf, "reader: checkout %u.%02u EUR \"%s\" (approve/decline)",
                  amountCents / 100, amountCents % 100, description);
    log(buf);
    active = true;
    next = PaymentStatus::Pending;
    return true;
  }
  PaymentStatus pollStatus() override { return next; }
  void cancelCheckout() override {
    if (active) log("reader: checkout cancelled");
    active = false;
  }
  void refundLastPayment() override { log("reader: refund last payment (milestone)"); }
};

class NullStore : public SettingsStore {
 public:
  void load(Dose*, size_t) override {}
  void save(const Dose*, size_t) override {}
};

class PrintSalesLog : public SalesLog {
 public:
  void recordSale(const Dose& d, bool refunded) override {
    char json[192];
    formatSaleJson(json, sizeof json, d, refunded);
    log(std::string("supabase: POST /rest/v1/coffee_log ") + json);
  }
};

class FakeMotor : public Motor {
 public:
  bool on = false;
  void setOn(bool v) override {
    if (v != on) log(v ? "MOTOR ON" : "MOTOR OFF");
    on = v;
  }
};

void printMenu(const Controller& c) {
  for (size_t i = 0; i < c.doseCount(); ++i) {
    const Dose& d = c.dose(i);
    std::printf("  %zu: %-8s %u.%02u EUR  %.1fs  sold %u\n", i + 1, d.name,
                d.priceCents / 100, d.priceCents % 100, d.grindMs / 1000.0,
                d.count);
  }
}

void printHelp() {
  std::puts(
      "commands:\n"
      "  menu              show doses\n"
      "  select N | s N    pick dose N (sends checkout to the reader)\n"
      "  approve | a       customer pays on reader\n"
      "  decline | d       card declined\n"
      "  cancel | c        cancel button on screen\n"
      "  wait SEC | w SEC  advance simulated time\n"
      "  shots N           set the Single counter to N (try 24 for a milestone)\n"
      "  status            show state\n"
      "  quit | q");
}

}  // namespace

int main() {
  FakeReader reader;
  FakeMotor motor;
  NullStore store;
  PrintSalesLog sales;
  Controller ctl(reader, motor, store, gDoses, kDoseCount);
  ctl.setSalesLog(&sales);
  ctl.begin(gNowMs);

  State last = ctl.state();
  auto report = [&] {
    if (ctl.state() != last) {
      last = ctl.state();
      log(std::string("state -> ") + stateName(last));
    }
  };

  std::puts("coffee grinder simulator. type 'help'.");
  printMenu(ctl);

  std::string line;
  while (std::printf("> "), std::fflush(stdout), std::getline(std::cin, line)) {
    std::istringstream in(line);
    std::string cmd;
    in >> cmd;
    if (cmd.empty()) continue;

    if (cmd == "q" || cmd == "quit") break;
    if (cmd == "help" || cmd == "h") {
      printHelp();
    } else if (cmd == "menu" || cmd == "m") {
      printMenu(ctl);
    } else if (cmd == "s" || cmd == "select") {
      size_t n = 0;
      in >> n;
      ctl.selectDose(n - 1, gNowMs);
    } else if (cmd == "a" || cmd == "approve") {
      reader.next = PaymentStatus::Succeeded;
    } else if (cmd == "d" || cmd == "decline") {
      reader.next = PaymentStatus::Failed;
    } else if (cmd == "c" || cmd == "cancel") {
      ctl.cancel(gNowMs);
    } else if (cmd == "w" || cmd == "wait") {
      double sec = 1;
      in >> sec;
      uint32_t end = gNowMs + static_cast<uint32_t>(sec * 1000);
      while (gNowMs < end) {
        gNowMs += 100;
        ctl.tick(gNowMs);
        report();
      }
    } else if (cmd == "shots") {
      in >> gDoses[0].count;
      printMenu(ctl);
    } else if (cmd == "status") {
      std::string s = std::string("state=") + stateName(ctl.state());
      if (const Dose* d = ctl.selectedDose()) s += std::string(" dose=") + d->name;
      if (ctl.state() == State::Grinding)
        s += " remaining=" + std::to_string(ctl.grindRemainingMs(gNowMs)) + "ms";
      s += motor.on ? " motor=ON" : " motor=off";
      log(s);
    } else {
      std::puts("unknown command, type 'help'");
    }
    ctl.tick(gNowMs);
    report();
  }
  motor.setOn(false);
  return 0;
}
