#include "SumUp.h"

#include <ArduinoJson.h>

#include <cstdio>
#include <cstring>

namespace grinder {

namespace {

bool isOk(int status) { return status >= 200 && status < 300; }
// No response, timeout, rate limit or server error: the request may succeed
// later (and, for a checkout, may even have reached the reader).
bool isTransient(int status) {
  return status <= 0 || status == 408 || status == 429 || status >= 500;
}

bool empty(const char* s) { return !s || !*s; }

}  // namespace

std::string buildCheckoutJson(const SumUpConfig& config, uint32_t amountCents,
                              const char* description, const std::string& foreignId) {
  JsonDocument doc;
  JsonObject amount = doc["total_amount"].to<JsonObject>();
  amount["currency"] = config.currency;
  amount["minor_unit"] = 2;
  amount["value"] = amountCents;
  if (!empty(description)) doc["description"] = description;
  JsonObject affiliate = doc["affiliate"].to<JsonObject>();
  affiliate["app_id"] = config.affiliateAppId;
  affiliate["key"] = config.affiliateKey;
  affiliate["foreign_transaction_id"] = foreignId;
  std::string out;
  serializeJson(doc, out);
  return out;
}

bool parseClientTransactionId(const std::string& body, std::string& out) {
  JsonDocument doc;
  if (deserializeJson(doc, body)) return false;
  const char* id = doc["data"]["client_transaction_id"];
  if (empty(id)) return false;
  out = id;
  return true;
}

TxnStatus parseTransaction(const std::string& body, std::string& id) {
  // The response carries events, links etc.; keep only what we need.
  JsonDocument filter;
  filter["id"] = true;
  filter["status"] = true;
  JsonDocument doc;
  if (deserializeJson(doc, body, DeserializationOption::Filter(filter)))
    return TxnStatus::Unknown;
  const char* txnId = doc["id"];
  id = txnId ? txnId : "";
  const char* status = doc["status"];
  if (!status) return TxnStatus::Unknown;
  if (!std::strcmp(status, "SUCCESSFUL")) return TxnStatus::Successful;
  if (!std::strcmp(status, "PENDING")) return TxnStatus::Pending;
  if (!std::strcmp(status, "FAILED") || !std::strcmp(status, "CANCELLED") ||
      !std::strcmp(status, "REFUNDED"))
    return TxnStatus::Failed;
  return TxnStatus::Unknown;
}

SumUpPayment::SumUpPayment(const SumUpConfig& config, uint32_t idSeed, Log log)
    : config_(config), log_(std::move(log)), idSeed_(idSeed) {}

void SumUpPayment::log(const std::string& msg) const {
  if (log_) log_("sumup: " + msg);
}

std::string SumUpPayment::merchantUrl() const {
  return std::string(config_.baseUrl) + "/v0.1/merchants/" + config_.merchantCode;
}

std::string SumUpPayment::transactionUrl(const std::string& clientTxnId) const {
  // IDs are UUIDs, nothing to escape.
  return std::string(config_.baseUrl) + "/v2.1/merchants/" + config_.merchantCode +
         "/transactions?client_transaction_id=" + clientTxnId;
}

bool SumUpPayment::startCheckout(uint32_t amountCents, const char* description) {
  if (empty(config_.apiKey) || empty(config_.merchantCode) || empty(config_.readerId) ||
      empty(config_.affiliateAppId) || empty(config_.affiliateKey)) {
    log("not configured (see secrets.example.h)");
    return false;
  }
  char foreignId[40];
  std::lock_guard<std::mutex> lock(mutex_);
  std::snprintf(foreignId, sizeof foreignId, "grinder-%08x-%u",
                static_cast<unsigned>(idSeed_), static_cast<unsigned>(++idCounter_));
  ++gen_;
  status_ = PaymentStatus::Pending;
  clientTxnId_.clear();
  pollWanted_ = false;
  lastTxnId_.clear();
  checkoutBody_ = buildCheckoutJson(config_, amountCents, description, foreignId);
  checkoutPending_ = true;
  log("checkout queued " + checkoutBody_.substr(0, checkoutBody_.find("\"affiliate\"")) +
      "...");
  return true;
}

PaymentStatus SumUpPayment::pollStatus() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (status_ == PaymentStatus::Pending && !clientTxnId_.empty()) pollWanted_ = true;
  return status_;
}

void SumUpPayment::cancelCheckout() {
  std::lock_guard<std::mutex> lock(mutex_);
  ++gen_;  // results still in flight for this checkout are now ignored
  // Queued but never sent: nothing reached the reader. If the request is in
  // flight right now, finish() sees the stale generation and cleans up.
  checkoutPending_ = false;
  if (status_ == PaymentStatus::Pending && !clientTxnId_.empty()) {
    log("cancel " + clientTxnId_);
    abandon(clientTxnId_, lastWorkMs_);
  }
  status_ = PaymentStatus::Failed;
  clientTxnId_.clear();
  pollWanted_ = false;
}

void SumUpPayment::refundLastPayment() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (lastTxnId_.empty()) {
    log("refund requested, but no transaction ID known");
    return;
  }
  log("refund queued " + lastTxnId_);
  refunds_.push_back(lastTxnId_);
  lastTxnId_.clear();
}

// Terminates the checkout on the reader and keeps an eye on its transaction,
// in case it was paid anyway. Caller holds the lock.
void SumUpPayment::abandon(const std::string& clientTxnId, uint32_t nowMs) {
  queueTerminate(nowMs);
  if (!orphan_.clientTxnId.empty() && orphan_.clientTxnId != clientTxnId)
    log("stopped watching " + orphan_.clientTxnId);
  orphan_ = {clientTxnId, kOrphanChecks, nowMs + kOrphanCheckMs};
}

// Caller holds the lock.
void SumUpPayment::queueTerminate(uint32_t nowMs) {
  terminatePending_ = true;
  terminateTries_ = 0;
  terminateRetryAtMs_ = nowMs;
}

bool SumUpPayment::idle() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return !terminatePending_ && !checkoutPending_ && !pollWanted_ &&
         orphan_.clientTxnId.empty() && refunds_.empty();
}

SumUpPayment::Job SumUpPayment::takeJob(uint32_t nowMs) {
  Job job;
  job.gen = gen_;
  // Terminate first: after a cancel + retry it must hit the old checkout,
  // not the new one. While it waits to be retried, nothing else goes out.
  if (terminatePending_ && !due(nowMs, terminateRetryAtMs_)) {
    job.kind = JobKind::None;
  } else if (terminatePending_) {
    job.kind = JobKind::Terminate;
    job.method = "POST";
    job.url = merchantUrl() + "/readers/" + config_.readerId + "/terminate";
    job.body = "{}";
  } else if (checkoutPending_) {
    checkoutPending_ = false;
    job.kind = JobKind::Checkout;
    job.method = "POST";
    job.url = merchantUrl() + "/readers/" + config_.readerId + "/checkout";
    job.body = checkoutBody_;
  } else if (pollWanted_) {
    pollWanted_ = false;
    job.kind = JobKind::Poll;
    job.url = transactionUrl(clientTxnId_);
    job.ref = clientTxnId_;
  } else if (!orphan_.clientTxnId.empty() && due(nowMs, orphan_.nextMs)) {
    job.kind = JobKind::OrphanPoll;
    job.url = transactionUrl(orphan_.clientTxnId);
    job.ref = orphan_.clientTxnId;
  } else if (!refunds_.empty() && due(nowMs, refundRetryAtMs_)) {
    job.kind = JobKind::Refund;
    job.method = "POST";
    job.ref = refunds_.front();
    job.url = std::string(config_.baseUrl) + "/v1.0/merchants/" + config_.merchantCode +
              "/payments/" + job.ref + "/refunds";
    job.body = "{}";  // no amount: full refund
  }
  return job;
}

bool SumUpPayment::work(HttpTransport& http, uint32_t nowMs) {
  Job job;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    lastWorkMs_ = nowMs;
    job = takeJob(nowMs);
  }
  if (job.kind == JobKind::None) return false;
  HttpResponse r = http.send(job.method, job.url, config_.apiKey, job.body);
  std::lock_guard<std::mutex> lock(mutex_);
  finish(job, r, nowMs);
  return true;
}

void SumUpPayment::finish(const Job& job, const HttpResponse& r, uint32_t nowMs) {
  const bool current = job.gen == gen_;
  auto fail = [&](const char* what) {
    log(std::string(what) + ": HTTP " + std::to_string(r.status) + " " + r.body);
  };

  switch (job.kind) {
    case JobKind::None:
      break;

    case JobKind::Terminate:
      if (isTransient(r.status)) {
        // No answer (offline): keep trying until the connection is back.
        if (r.status <= 0 || ++terminateTries_ < kTerminateTries) {
          terminateRetryAtMs_ = nowMs + kTerminateRetryMs;
          break;
        }
      }
      terminatePending_ = false;
      // 4xx typically means there was nothing left to terminate.
      if (isOk(r.status)) log("checkout terminated");
      else fail("terminate");
      break;

    case JobKind::Checkout: {
      std::string id;
      const bool accepted = isOk(r.status) && parseClientTransactionId(r.body, id);
      if (accepted && current) {
        clientTxnId_ = id;
        log("checkout accepted " + id);
      } else if (accepted) {
        // Cancelled while the request was in flight: the reader shows it.
        log("checkout accepted after cancel, terminating " + id);
        abandon(id, nowMs);
      } else {
        fail("checkout rejected");
        if (current) status_ = PaymentStatus::Failed;
        // No answer: it may still have reached the reader. Clear it.
        if (isTransient(r.status)) queueTerminate(nowMs);
      }
      break;
    }

    case JobKind::Poll: {
      if (!current || status_ != PaymentStatus::Pending) break;  // cancel covers it
      if (r.status == 404) break;  // not created yet: keep waiting
      if (!isOk(r.status)) {
        fail("poll");
        break;
      }
      std::string txnId;
      switch (parseTransaction(r.body, txnId)) {
        case TxnStatus::Successful:
          status_ = PaymentStatus::Succeeded;
          lastTxnId_ = txnId;
          log("payment successful " + txnId);
          break;
        case TxnStatus::Failed:
          status_ = PaymentStatus::Failed;
          log("payment failed " + txnId);
          break;
        case TxnStatus::Pending:
        case TxnStatus::Unknown:
          break;
      }
      break;
    }

    case JobKind::OrphanPoll: {
      if (orphan_.clientTxnId != job.ref) break;  // replaced meanwhile
      if (isTransient(r.status)) {
        // No usable answer (offline): ask again later without using up a check.
        orphan_.nextMs = nowMs + kOrphanCheckMs;
        break;
      }
      std::string txnId;
      TxnStatus s = isOk(r.status) ? parseTransaction(r.body, txnId) : TxnStatus::Unknown;
      if (s == TxnStatus::Successful && !txnId.empty()) {
        log("cancelled checkout was paid anyway, refunding " + txnId);
        refunds_.push_back(txnId);
        orphan_ = {};
      } else if (s == TxnStatus::Failed || --orphan_.checksLeft <= 0) {
        orphan_ = {};
      } else {
        orphan_.nextMs = nowMs + kOrphanCheckMs;
      }
      break;
    }

    case JobKind::Refund:
      if (isOk(r.status)) {
        log("refunded " + job.ref);
      } else if (isTransient(r.status)) {
        refundRetryAtMs_ = nowMs + kRefundRetryMs;
        fail("refund (will retry)");
        break;
      } else {
        fail("refund failed for good");  // e.g. 403: key not allowed to refund
      }
      if (!refunds_.empty() && refunds_.front() == job.ref) refunds_.erase(refunds_.begin());
      break;
  }
}

}  // namespace grinder
