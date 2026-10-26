#include <unity.h>

#include <deque>
#include <functional>
#include <string>
#include <vector>

#include "SumUp.h"

using namespace grinder;

namespace {

struct Request {
  std::string method, url, bearer, body;
};

// Answers requests from a queue (404 when empty) and records them. `during`
// runs inside send(), i.e. while the request is "in flight".
struct FakeHttp : HttpTransport {
  std::deque<HttpResponse> replies;
  std::vector<Request> sent;
  std::function<void()> during;

  HttpResponse send(const char* method, const std::string& url, const std::string& bearer,
                    const std::string& body) override {
    sent.push_back({method, url, bearer, body});
    if (during) {
      auto f = during;
      during = nullptr;
      f();
    }
    if (replies.empty()) return {404, ""};
    HttpResponse r = replies.front();
    replies.pop_front();
    return r;
  }
};

const SumUpConfig kConfig = {"sup_sk_test", "MC123", "rdr_1", "com.test.grinder", "aff_key"};
const std::string kMerchant = "https://api.sumup.com/v0.1/merchants/MC123";

HttpResponse accepted(const char* id) {
  return {201, std::string("{\"data\":{\"client_transaction_id\":\"") + id + "\"}}"};
}
HttpResponse txn(const char* status, const char* id = "txn-1") {
  return {200, std::string("{\"id\":\"") + id + "\",\"status\":\"" + status +
                   "\",\"events\":[{\"status\":\"SUCCESSFUL\"}]}"};
}

FakeHttp* http;
SumUpPayment* pay;
uint32_t now;

// Runs work() until nothing is due right now.
void drain() {
  while (pay->work(*http, now)) {
  }
}

// Starts a checkout and lets it be accepted as `id`.
void startAccepted(const char* id = "ctx-1") {
  TEST_ASSERT_TRUE(pay->startCheckout(50, "Single"));
  http->replies.push_back(accepted(id));
  drain();
}

}  // namespace

void setUp() {
  http = new FakeHttp();
  pay = new SumUpPayment(kConfig, 0xABCD);
  now = 1000;
}

void tearDown() {
  delete pay;
  delete http;
}

void test_checkout_json() {
  std::string j = buildCheckoutJson(kConfig, 90, "Dou\"ble", "grinder-1");
  TEST_ASSERT_EQUAL_STRING(
      "{\"total_amount\":{\"currency\":\"EUR\",\"minor_unit\":2,\"value\":90},"
      "\"description\":\"Dou\\\"ble\",\"affiliate\":{\"app_id\":\"com.test.grinder\","
      "\"key\":\"aff_key\",\"foreign_transaction_id\":\"grinder-1\"}}",
      j.c_str());
}

void test_parse_transaction_uses_top_level_status_only() {
  std::string id;
  TEST_ASSERT_EQUAL(TxnStatus::Pending, parseTransaction(txn("PENDING").body, id));
  TEST_ASSERT_EQUAL(TxnStatus::Successful, parseTransaction(txn("SUCCESSFUL").body, id));
  TEST_ASSERT_EQUAL_STRING("txn-1", id.c_str());
  TEST_ASSERT_EQUAL(TxnStatus::Failed, parseTransaction(txn("CANCELLED").body, id));
  TEST_ASSERT_EQUAL(TxnStatus::Unknown, parseTransaction("{\"events\":[{\"status\":\"SUCCESSFUL\"}]}", id));
  TEST_ASSERT_EQUAL(TxnStatus::Unknown, parseTransaction("not json", id));
}

void test_happy_path_and_refund() {
  TEST_ASSERT_TRUE(pay->startCheckout(50, "Single"));
  TEST_ASSERT_EQUAL(PaymentStatus::Pending, pay->pollStatus());
  http->replies.push_back(accepted("ctx-1"));
  drain();
  TEST_ASSERT_EQUAL(1, http->sent.size());
  TEST_ASSERT_EQUAL_STRING("POST", http->sent[0].method.c_str());
  TEST_ASSERT_EQUAL_STRING((kMerchant + "/readers/rdr_1/checkout").c_str(),
                           http->sent[0].url.c_str());
  TEST_ASSERT_EQUAL_STRING("sup_sk_test", http->sent[0].bearer.c_str());

  // Polls happen only when the controller asks.
  TEST_ASSERT_EQUAL(PaymentStatus::Pending, pay->pollStatus());
  http->replies.push_back(txn("PENDING"));
  drain();
  TEST_ASSERT_EQUAL_STRING(
      "https://api.sumup.com/v2.1/merchants/MC123/transactions?client_transaction_id=ctx-1",
      http->sent[1].url.c_str());
  TEST_ASSERT_EQUAL(PaymentStatus::Pending, pay->pollStatus());
  http->replies.push_back(txn("SUCCESSFUL", "txn-9"));
  drain();
  TEST_ASSERT_EQUAL(PaymentStatus::Succeeded, pay->pollStatus());
  TEST_ASSERT_TRUE(pay->idle());

  pay->refundLastPayment();
  http->replies.push_back({204, ""});
  drain();
  TEST_ASSERT_EQUAL_STRING(
      "https://api.sumup.com/v1.0/merchants/MC123/payments/txn-9/refunds",
      http->sent.back().url.c_str());
  TEST_ASSERT_TRUE(pay->idle());
}

void test_transaction_not_found_yet_keeps_waiting() {
  startAccepted();
  pay->pollStatus();
  drain();  // 404
  TEST_ASSERT_EQUAL(PaymentStatus::Pending, pay->pollStatus());
  http->replies.push_back({500, "oops"});
  drain();
  TEST_ASSERT_EQUAL(PaymentStatus::Pending, pay->pollStatus());
  http->replies.push_back(txn("FAILED"));
  drain();
  TEST_ASSERT_EQUAL(PaymentStatus::Failed, pay->pollStatus());
}

void test_rejected_checkout_fails_without_terminate() {
  pay->startCheckout(50, "Single");
  http->replies.push_back({422, "{\"detail\":\"The device is offline.\"}"});
  drain();
  TEST_ASSERT_EQUAL(PaymentStatus::Failed, pay->pollStatus());
  TEST_ASSERT_EQUAL(1, http->sent.size());
}

void test_checkout_without_answer_fails_and_terminates() {
  pay->startCheckout(50, "Single");
  http->replies.push_back({-1, ""});
  http->replies.push_back({204, ""});
  drain();
  TEST_ASSERT_EQUAL(PaymentStatus::Failed, pay->pollStatus());
  TEST_ASSERT_EQUAL(2, http->sent.size());
  TEST_ASSERT_EQUAL_STRING((kMerchant + "/readers/rdr_1/terminate").c_str(),
                           http->sent[1].url.c_str());
}

void test_cancel_terminates_and_refunds_late_payment() {
  startAccepted("ctx-1");
  pay->cancelCheckout();
  TEST_ASSERT_EQUAL(PaymentStatus::Failed, pay->pollStatus());
  http->replies.push_back({204, ""});
  drain();
  TEST_ASSERT_EQUAL_STRING((kMerchant + "/readers/rdr_1/terminate").c_str(),
                           http->sent.back().url.c_str());
  size_t before = http->sent.size();
  drain();  // the late check isn't due yet
  TEST_ASSERT_EQUAL(before, http->sent.size());

  now += SumUpPayment::kOrphanCheckMs;
  http->replies.push_back(txn("SUCCESSFUL", "txn-late"));
  http->replies.push_back({204, ""});
  drain();
  TEST_ASSERT_EQUAL_STRING(
      "https://api.sumup.com/v1.0/merchants/MC123/payments/txn-late/refunds",
      http->sent.back().url.c_str());
  TEST_ASSERT_EQUAL(PaymentStatus::Failed, pay->pollStatus());  // still no coffee
  TEST_ASSERT_TRUE(pay->idle());
}

void test_cancelled_checkout_watched_only_for_a_while() {
  startAccepted("ctx-1");
  pay->cancelCheckout();
  http->replies.push_back({204, ""});
  drain();
  for (int i = 0; i < SumUpPayment::kOrphanChecks; ++i) {
    now += SumUpPayment::kOrphanCheckMs;
    http->replies.push_back(txn("PENDING"));
    drain();
  }
  TEST_ASSERT_TRUE(pay->idle());
}

// The grinder lost Wi-Fi during the payment, but the customer paid on the Solo
// (it has its own connection). Once back online, the payment is refunded.
void test_payment_during_outage_refunded_when_back_online() {
  startAccepted("ctx-1");
  pay->cancelCheckout();  // controller timeout while offline
  // Offline for 2 minutes: terminate and late checks get no answer.
  for (uint32_t t = 0; t < 120000; t += 1000) {
    http->replies.push_back({-1, "Wi-Fi not connected"});
    http->replies.push_back({-1, "Wi-Fi not connected"});
    pay->work(*http, now);
    now += 1000;
  }
  TEST_ASSERT_FALSE(pay->idle());
  // Back online: terminate, then the check finds the payment and refunds it.
  http->replies.clear();
  http->replies.push_back({204, ""});
  http->replies.push_back(txn("SUCCESSFUL", "txn-offline"));
  http->replies.push_back({204, ""});
  now += SumUpPayment::kOrphanCheckMs;
  drain();
  TEST_ASSERT_EQUAL_STRING(
      "https://api.sumup.com/v1.0/merchants/MC123/payments/txn-offline/refunds",
      http->sent.back().url.c_str());
  TEST_ASSERT_TRUE(pay->idle());
}

void test_offline_terminate_does_not_spin() {
  startAccepted("ctx-1");
  pay->cancelCheckout();
  http->replies.push_back({-1, "Wi-Fi not connected"});
  size_t before = http->sent.size();
  drain();
  TEST_ASSERT_EQUAL(before + 1, http->sent.size());  // waits instead of retrying at once
  now += SumUpPayment::kTerminateRetryMs;
  http->replies.push_back({204, ""});
  pay->work(*http, now);
  TEST_ASSERT_EQUAL_STRING((kMerchant + "/readers/rdr_1/terminate").c_str(),
                           http->sent.back().url.c_str());
}

void test_cancel_while_checkout_in_flight() {
  pay->startCheckout(50, "Single");
  http->replies.push_back(accepted("ctx-1"));
  http->during = [] { pay->cancelCheckout(); };
  drain();
  // The late acceptance is terminated and watched, never reported as paid.
  TEST_ASSERT_EQUAL(PaymentStatus::Failed, pay->pollStatus());
  TEST_ASSERT_EQUAL_STRING((kMerchant + "/readers/rdr_1/terminate").c_str(),
                           http->sent[1].url.c_str());
}

void test_terminate_runs_before_retried_checkout() {
  startAccepted("ctx-1");
  pay->cancelCheckout();
  pay->startCheckout(50, "Single");
  http->replies.push_back({204, ""});
  http->replies.push_back(accepted("ctx-2"));
  drain();
  TEST_ASSERT_EQUAL_STRING((kMerchant + "/readers/rdr_1/terminate").c_str(),
                           http->sent[1].url.c_str());
  TEST_ASSERT_EQUAL_STRING((kMerchant + "/readers/rdr_1/checkout").c_str(),
                           http->sent[2].url.c_str());
}

void test_stale_success_does_not_pay_new_checkout() {
  startAccepted("ctx-1");
  pay->pollStatus();
  // While the poll for ctx-1 is in flight, it is cancelled and a new one starts.
  http->during = [] {
    pay->cancelCheckout();
    pay->startCheckout(50, "Single");
  };
  http->replies.push_back(txn("SUCCESSFUL", "txn-old"));
  pay->work(*http, now);
  TEST_ASSERT_EQUAL(PaymentStatus::Pending, pay->pollStatus());
}

void test_refund_retries_transient_errors_and_drops_others() {
  startAccepted();
  pay->pollStatus();
  http->replies.push_back(txn("SUCCESSFUL", "txn-1"));
  drain();
  pay->refundLastPayment();
  http->replies.push_back({503, ""});
  drain();
  size_t sent = http->sent.size();
  now += SumUpPayment::kRefundRetryMs - 1;
  drain();
  TEST_ASSERT_EQUAL(sent, http->sent.size());  // waits before retrying
  now += 1;
  http->replies.push_back({403, "{\"detail\":\"forbidden\"}"});
  drain();
  TEST_ASSERT_EQUAL(sent + 1, http->sent.size());
  TEST_ASSERT_TRUE(pay->idle());  // dropped, not retried forever
}

void test_foreign_ids_are_unique() {
  pay->startCheckout(50, "Single");
  http->replies.push_back(accepted("a"));
  drain();
  pay->cancelCheckout();
  http->replies.push_back({204, ""});
  drain();
  pay->startCheckout(50, "Single");
  http->replies.push_back(accepted("b"));
  drain();
  std::string first = http->sent[0].body, second = http->sent.back().body;
  TEST_ASSERT_TRUE(first.find("grinder-0000abcd-1") != std::string::npos);
  TEST_ASSERT_TRUE(second.find("grinder-0000abcd-2") != std::string::npos);
}

void test_missing_config_refuses_checkout() {
  SumUpConfig c = kConfig;
  c.affiliateKey = "";
  SumUpPayment p(c, 1);
  TEST_ASSERT_FALSE(p.startCheckout(50, "Single"));
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_checkout_json);
  RUN_TEST(test_parse_transaction_uses_top_level_status_only);
  RUN_TEST(test_happy_path_and_refund);
  RUN_TEST(test_transaction_not_found_yet_keeps_waiting);
  RUN_TEST(test_rejected_checkout_fails_without_terminate);
  RUN_TEST(test_checkout_without_answer_fails_and_terminates);
  RUN_TEST(test_cancel_terminates_and_refunds_late_payment);
  RUN_TEST(test_cancelled_checkout_watched_only_for_a_while);
  RUN_TEST(test_payment_during_outage_refunded_when_back_online);
  RUN_TEST(test_offline_terminate_does_not_spin);
  RUN_TEST(test_cancel_while_checkout_in_flight);
  RUN_TEST(test_terminate_runs_before_retried_checkout);
  RUN_TEST(test_stale_success_does_not_pay_new_checkout);
  RUN_TEST(test_refund_retries_transient_errors_and_drops_others);
  RUN_TEST(test_foreign_ids_are_unique);
  RUN_TEST(test_missing_config_refuses_checkout);
  return UNITY_END();
}
