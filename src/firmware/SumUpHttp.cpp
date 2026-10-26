#include "SumUpHttp.h"

#include <Arduino.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>

#include "SumUpCa.h"

namespace grinder {

namespace {

constexpr uint32_t kTimeoutMs = 10000;
constexpr uint32_t kIdleDelayMs = 50;
constexpr uint32_t kStackBytes = 12288;  // TLS handshakes need the room

ArduinoHttps gHttp;

void taskMain(void* arg) {
  auto* payment = static_cast<SumUpPayment*>(arg);
  for (;;) {
    while (payment->work(gHttp, millis())) {
    }
    vTaskDelay(pdMS_TO_TICKS(kIdleDelayMs));
  }
}

}  // namespace

HttpResponse ArduinoHttps::send(const char* method, const std::string& url,
                                const std::string& bearer, const std::string& body) {
  if (!WiFi.isConnected()) return {-1, "Wi-Fi not connected"};
  WiFiClientSecure client;
  client.setCACert(kSumUpRootCa);
  HTTPClient http;
  if (!http.begin(client, url.c_str())) return {-1, "bad URL"};
  http.setTimeout(kTimeoutMs);
  http.addHeader("Authorization", String("Bearer ") + bearer.c_str());
  if (!body.empty()) http.addHeader("Content-Type", "application/json");
  int code = http.sendRequest(method, reinterpret_cast<uint8_t*>(const_cast<char*>(body.data())),
                              body.size());
  HttpResponse r;
  r.status = code;
  if (code > 0) {
    String s = http.getString();
    r.body.assign(s.c_str(), s.length());
  } else {
    r.body = HTTPClient::errorToString(code).c_str();
  }
  http.end();
  return r;
}

void startSumUpTask(SumUpPayment& payment) {
  xTaskCreate(taskMain, "sumup", kStackBytes, &payment, 1, nullptr);
}

}  // namespace grinder
