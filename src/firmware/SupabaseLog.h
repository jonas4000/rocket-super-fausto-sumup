#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

#include "Ports.h"

namespace grinder {

// Sends each paid sale to Supabase (`POST /rest/v1/coffee_log`) from a background
// FreeRTOS task, so HTTP never blocks the controller or UI loop. Sales are
// queued in RAM and retried while Wi-Fi/Supabase are unreachable; Supabase
// stamps created_at on arrival. The queue is lost on reboot; the per-dose
// counters in NVS remain the source of truth.
class SupabaseLog : public SalesLog {
 public:
  // url: "https://<project>.supabase.co"; key: publishable/anon key.
  // With url == nullptr, sales are only printed to Serial.
  SupabaseLog(const char* url, const char* key) : url_(url), key_(key) {}

  void begin();
  void recordSale(const Dose& dose, bool refunded) override;

 private:
  enum class Result { Sent, Retry, Drop };
  static constexpr size_t kJsonLen = 96;
  struct Item {
    char json[kJsonLen];
  };

  static void taskMain(void* arg);
  Result post(const char* json);

  const char* url_;
  const char* key_;
  QueueHandle_t queue_ = nullptr;
};

}  // namespace grinder
