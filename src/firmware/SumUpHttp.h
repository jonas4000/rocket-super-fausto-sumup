#pragma once

#include "SumUp.h"

namespace grinder {

// HTTPS transport for SumUpPayment on the ESP32 (HTTPClient over
// WiFiClientSecure, certificate checked against the pinned roots in
// SumUpCa.h). Fails immediately while Wi-Fi is down.
class ArduinoHttps : public HttpTransport {
 public:
  HttpResponse send(const char* method, const std::string& url, const std::string& bearer,
                    const std::string& body) override;
};

// Runs payment.work() in a background FreeRTOS task, so API calls never block
// the controller or UI loop.
void startSumUpTask(SumUpPayment& payment);

}  // namespace grinder
