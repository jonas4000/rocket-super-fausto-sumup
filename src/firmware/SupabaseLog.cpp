#include "SupabaseLog.h"

#include <Arduino.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>

#include <cstring>

#include "SaleJson.h"

namespace grinder {

namespace {

constexpr UBaseType_t kQueueLength = 32;
constexpr uint32_t kRetryMs = 10000;

}  // namespace

void SupabaseLog::begin() {
  queue_ = xQueueCreate(kQueueLength, sizeof(Item));
  if (url_) xTaskCreate(taskMain, "supabase", 8192, this, 1, nullptr);
}

void SupabaseLog::recordSale(const Dose& dose, bool refunded) {
  Item item;
  if (!formatSaleJson(item.json, sizeof item.json, dose, refunded)) {
    Serial.println("supabase: sale JSON too long, dropped");
    return;
  }
  Serial.printf("supabase: queue %s\n", item.json);
  if (!url_) return;
  if (xQueueSend(queue_, &item, 0) != pdTRUE)
    Serial.println("supabase: queue full, sale dropped");
}

void SupabaseLog::taskMain(void* arg) {
  auto* self = static_cast<SupabaseLog*>(arg);
  Item item;
  for (;;) {
    // Peek so the sale stays queued until it is sent (or rejected for good).
    xQueuePeek(self->queue_, &item, portMAX_DELAY);
    Result r = WiFi.isConnected() ? self->post(item.json) : Result::Retry;
    if (r == Result::Retry) {
      vTaskDelay(pdMS_TO_TICKS(kRetryMs));
      continue;
    }
    xQueueReceive(self->queue_, &item, 0);
  }
}

SupabaseLog::Result SupabaseLog::post(const char* json) {
  WiFiClientSecure client;
  // TODO: verify the server certificate (CA bundle) instead of skipping it.
  client.setInsecure();
  HTTPClient http;
  String endpoint = String(url_) + "/rest/v1/coffee_log";
  if (!http.begin(client, endpoint)) return Result::Retry;
  http.setTimeout(10000);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("Prefer", "return=minimal");
  http.addHeader("apikey", key_);
  // Legacy anon keys are JWTs and also go in Authorization; new publishable
  // keys (sb_publishable_...) must not.
  if (std::strncmp(key_, "sb_", 3) != 0)
    http.addHeader("Authorization", String("Bearer ") + key_);

  int code = http.POST(reinterpret_cast<uint8_t*>(const_cast<char*>(json)),
                       std::strlen(json));
  String body = code > 0 ? http.getString() : String();
  http.end();

  if (code >= 200 && code < 300) {
    Serial.println("supabase: sale sent");
    return Result::Sent;
  }
  Serial.printf("supabase: HTTP %d %s\n", code, body.c_str());
  // Network errors, rate limits and server errors are worth retrying; other
  // 4xx (bad key, schema mismatch) would fail forever and block the queue.
  if (code <= 0 || code == 408 || code == 429 || code >= 500) return Result::Retry;
  return Result::Drop;
}

}  // namespace grinder
