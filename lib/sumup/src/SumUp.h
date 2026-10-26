#pragma once

#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include "Ports.h"

namespace grinder {

struct HttpResponse {
  int status = 0;  // HTTP status code; <= 0 means no response (network/TLS error)
  std::string body;
};

// Blocking HTTPS transport (HTTPClient on the ESP32, libcurl in sumup-cli).
class HttpTransport {
 public:
  virtual ~HttpTransport() = default;
  // Sends `body` as JSON (none if empty) with "Authorization: Bearer <bearer>".
  virtual HttpResponse send(const char* method, const std::string& url,
                            const std::string& bearer, const std::string& body) = 0;
};

struct SumUpConfig {
  const char* apiKey;
  const char* merchantCode;
  const char* readerId;
  const char* affiliateAppId;
  const char* affiliateKey;
  const char* currency = "EUR";  // amounts are sent with minor_unit 2 (cents)
  const char* baseUrl = "https://api.sumup.com";
};

// Top-level `status` of a transaction (GET /v2.1/merchants/{mc}/transactions).
enum class TxnStatus { Unknown, Pending, Successful, Failed };

// JSON helpers, exposed for the tests.
std::string buildCheckoutJson(const SumUpConfig& config, uint32_t amountCents,
                              const char* description, const std::string& foreignId);
// {"data":{"client_transaction_id":"..."}} from the reader checkout.
bool parseClientTransactionId(const std::string& body, std::string& out);
// Only the top-level "status" counts (nested event statuses are ignored).
// `id` is the transaction ID that refunds need.
TxnStatus parseTransaction(const std::string& body, std::string& id);

// PaymentProvider for a Solo paired via the SumUp Cloud API (Readers API).
//
// The PaymentProvider methods are called from the controller loop and never
// touch the network: they record what is wanted and return the last known
// status. A background task calls work() repeatedly; each call makes at most
// one blocking API request. All state is guarded by a mutex.
//
// Flow: POST .../readers/{id}/checkout -> client_transaction_id, then
// GET /v2.1/merchants/{mc}/transactions?client_transaction_id=... each time the
// controller polls. Only a top-level "SUCCESSFUL" for the current checkout
// yields Succeeded. Results of a cancelled or replaced checkout are ignored.
//
// Cancelling sends .../terminate and keeps checking the abandoned transaction
// for a while: if it was paid anyway (card tapped just as the timeout hit),
// it is refunded automatically, since the customer got no coffee. Requests
// that get no usable answer (offline, timeout, 5xx) don't use up the terminate
// tries or the late-payment checks: they wait for the connection, so a card
// tapped while the grinder was offline is still refunded once it is back.
class SumUpPayment : public PaymentProvider {
 public:
  using Log = std::function<void(const std::string&)>;

  // idSeed makes foreign_transaction_ids unique across reboots (esp_random()).
  SumUpPayment(const SumUpConfig& config, uint32_t idSeed, Log log = nullptr);

  bool startCheckout(uint32_t amountCents, const char* description) override;
  PaymentStatus pollStatus() override;
  void cancelCheckout() override;
  void refundLastPayment() override;

  // Background side: performs at most one pending API call (blocking) and
  // returns true if it made one. Call in a loop from a task.
  bool work(HttpTransport& http, uint32_t nowMs);
  // Nothing queued or being watched (used by sumup-cli to know when to exit).
  bool idle() const;

  static constexpr uint32_t kOrphanCheckMs = 5000;  // re-check an abandoned payment
  static constexpr int kOrphanChecks = 6;           // ...answers, about 30 s online
  static constexpr uint32_t kRefundRetryMs = 10000;
  static constexpr int kTerminateTries = 3;          // HTTP error answers
  static constexpr uint32_t kTerminateRetryMs = 2000;  // after no/transient answer

 private:
  enum class JobKind { None, Terminate, Checkout, Poll, OrphanPoll, Refund };
  struct Job {
    JobKind kind = JobKind::None;
    uint32_t gen = 0;  // checkout generation the job belongs to
    const char* method = "GET";
    std::string url, body, ref;  // ref: transaction (or client transaction) ID
  };
  struct Orphan {
    std::string clientTxnId;  // empty: none
    int checksLeft = 0;
    uint32_t nextMs = 0;
  };

  Job takeJob(uint32_t nowMs);
  void finish(const Job& job, const HttpResponse& r, uint32_t nowMs);
  void abandon(const std::string& clientTxnId, uint32_t nowMs);
  void queueTerminate(uint32_t nowMs);
  std::string merchantUrl() const;
  std::string transactionUrl(const std::string& clientTxnId) const;
  void log(const std::string& msg) const;
  static bool due(uint32_t nowMs, uint32_t atMs) {
    return static_cast<int32_t>(nowMs - atMs) >= 0;  // wraparound-safe
  }

  SumUpConfig config_;
  Log log_;
  uint32_t idSeed_;
  uint32_t idCounter_ = 0;

  mutable std::mutex mutex_;
  uint32_t gen_ = 0;  // bumped by every start and cancel
  PaymentStatus status_ = PaymentStatus::Failed;
  bool checkoutPending_ = false;  // queued, not sent yet
  std::string checkoutBody_;
  std::string clientTxnId_;  // of the current checkout, once accepted
  bool pollWanted_ = false;
  bool terminatePending_ = false;
  int terminateTries_ = 0;
  uint32_t terminateRetryAtMs_ = 0;
  std::string lastTxnId_;  // transaction ID of the last successful payment
  Orphan orphan_;
  std::vector<std::string> refunds_;  // transaction IDs to refund, in order
  uint32_t refundRetryAtMs_ = 0;
  uint32_t lastWorkMs_ = 0;
};

}  // namespace grinder
