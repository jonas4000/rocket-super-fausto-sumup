#pragma once

// libcurl HTTP on the Mac: the SumUpPayment transport (sumup-cli, ui-sim
// --sumup) and Supabase inserts (ui-sim --sumup). Uses the system CA store for
// TLS. Link with -lcurl; call curl_global_init() once before use.

#include <curl/curl.h>

#include <string>
#include <vector>

#include "SumUp.h"

namespace grinder {

// Blocking; a fresh connection per request, 10 s timeout. Adds
// Content-Type: application/json when there is a body.
inline HttpResponse curlRequest(const char* method, const std::string& url,
                                const std::vector<std::string>& extraHeaders,
                                const std::string& body) {
  CURL* curl = curl_easy_init();
  if (!curl) return {-1, "curl_easy_init failed"};
  HttpResponse r;
  curl_slist* headers = nullptr;
  for (const std::string& h : extraHeaders) headers = curl_slist_append(headers, h.c_str());
  if (!body.empty()) headers = curl_slist_append(headers, "Content-Type: application/json");
  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method);
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
  if (!body.empty()) {
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
  }
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &r.body);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,
                   +[](char* p, size_t size, size_t n, void* out) {
                     static_cast<std::string*>(out)->append(p, size * n);
                     return size * n;
                   });
  CURLcode rc = curl_easy_perform(curl);
  if (rc == CURLE_OK) {
    long code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
    r.status = static_cast<int>(code);
  } else {
    r.status = -1;
    r.body = curl_easy_strerror(rc);
  }
  curl_slist_free_all(headers);
  curl_easy_cleanup(curl);
  return r;
}

class CurlHttp : public HttpTransport {
 public:
  HttpResponse send(const char* method, const std::string& url, const std::string& bearer,
                    const std::string& body) override {
    return curlRequest(method, url, {"Authorization: Bearer " + bearer}, body);
  }
};

}  // namespace grinder
