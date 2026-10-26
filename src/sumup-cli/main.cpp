// Talks to the real SumUp Cloud API from the Mac, with the same SumUpPayment
// code the firmware uses (libcurl instead of HTTPClient). Reads the keys from
// include/secrets.h. Real payments: use small amounts.
//
//   .pio/build/sumup-cli/program me                 merchant profile (merchant_code)
//   .pio/build/sumup-cli/program readers            paired readers and their status
//   .pio/build/sumup-cli/program pay CENTS          checkout on the Solo, wait for the result
//   .pio/build/sumup-cli/program pay CENTS refund   ...and refund it (milestone path)
//   .pio/build/sumup-cli/program status CLIENT_ID   transaction by client_transaction_id
//   .pio/build/sumup-cli/program terminate          cancel whatever the Solo shows

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <thread>

#include "CurlHttp.h"
#include "SumUp.h"

#if __has_include("secrets.h")
#include "secrets.h"
#else
#error "include/secrets.h missing (see secrets.example.h)"
#endif

using namespace grinder;

namespace {

const SumUpConfig kConfig = {SUMUP_API_KEY, SUMUP_MERCHANT_CODE, SUMUP_READER_ID,
                             SUMUP_AFFILIATE_APP_ID, SUMUP_AFFILIATE_KEY};
const std::string kBase = "https://api.sumup.com";

uint32_t nowMs() {
  using namespace std::chrono;
  static const auto start = steady_clock::now();
  return static_cast<uint32_t>(
      duration_cast<milliseconds>(steady_clock::now() - start).count());
}

void sleepMs(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

int printResponse(const HttpResponse& r) {
  std::printf("HTTP %d\n%s\n", r.status, r.body.c_str());
  return r.status >= 200 && r.status < 300 ? 0 : 1;
}

int usage() {
  std::fprintf(stderr,
               "usage: program me | readers | pay CENTS [refund] | status CLIENT_ID | "
               "terminate\n");
  return 2;
}

// Same timing as the controller: poll every 2 s, give up after 60 s.
int pay(CurlHttp& http, uint32_t cents, bool refund) {
  auto log = [](const std::string& msg) {
    std::printf("[%6.1fs] %s\n", nowMs() / 1000.0, msg.c_str());
  };
  SumUpPayment payment(kConfig, std::random_device{}(), log);
  auto runWork = [&] {
    while (payment.work(http, nowMs())) {
    }
  };

  if (!payment.startCheckout(cents, "Grinder test")) return 1;
  std::printf("Pay %u.%02u EUR on the Solo now (60 s)...\n", cents / 100, cents % 100);
  PaymentStatus status = PaymentStatus::Pending;
  const uint32_t start = nowMs();
  uint32_t lastPoll = start;
  while (status == PaymentStatus::Pending) {
    runWork();
    if (nowMs() - lastPoll >= 2000) {
      lastPoll = nowMs();
      status = payment.pollStatus();
    }
    if (status == PaymentStatus::Pending && nowMs() - start >= 60000) {
      log("timeout, cancelling");
      payment.cancelCheckout();
      status = PaymentStatus::Failed;
    }
    sleepMs(50);
  }
  log(status == PaymentStatus::Succeeded ? "RESULT: paid (the grinder would start now)"
                                         : "RESULT: not paid (no grinding)");
  if (status == PaymentStatus::Succeeded && refund) payment.refundLastPayment();

  // Finish terminate / late-payment checks / refunds before exiting.
  while (!payment.idle()) {
    runWork();
    sleepMs(100);
  }
  return status == PaymentStatus::Succeeded ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) return usage();
  curl_global_init(CURL_GLOBAL_DEFAULT);
  CurlHttp http;
  const std::string cmd = argv[1];
  const std::string merchant = kBase + "/v0.1/merchants/" + SUMUP_MERCHANT_CODE;
  int rc;
  if (cmd == "me") {
    rc = printResponse(http.send("GET", kBase + "/v0.1/me", SUMUP_API_KEY, ""));
  } else if (cmd == "readers") {
    rc = printResponse(http.send("GET", merchant + "/readers", SUMUP_API_KEY, ""));
  } else if (cmd == "terminate") {
    rc = printResponse(http.send("POST", merchant + "/readers/" + SUMUP_READER_ID + "/terminate",
                                 SUMUP_API_KEY, "{}"));
  } else if (cmd == "status" && argc >= 3) {
    HttpResponse r = http.send("GET",
                               kBase + "/v2.1/merchants/" + SUMUP_MERCHANT_CODE +
                                   "/transactions?client_transaction_id=" + argv[2],
                               SUMUP_API_KEY, "");
    rc = printResponse(r);
    std::string id;
    const char* names[] = {"Unknown", "Pending", "Successful", "Failed"};
    if (r.status == 200)
      std::printf("parsed: %s (id %s)\n", names[static_cast<int>(parseTransaction(r.body, id))],
                  id.c_str());
  } else if (cmd == "pay" && argc >= 3) {
    long cents = std::strtol(argv[2], nullptr, 10);
    if (cents <= 0 || cents > 10000) {
      std::fprintf(stderr, "CENTS must be 1..10000\n");
      return 2;
    }
    rc = pay(http, static_cast<uint32_t>(cents), argc >= 4 && !std::strcmp(argv[3], "refund"));
  } else {
    rc = usage();
  }
  curl_global_cleanup();
  return rc;
}
