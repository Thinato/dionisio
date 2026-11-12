#include <curl/curl.h>

#include <algorithm>
#include <cctype>

#include "core.hpp"

static size_t on_body(char* p, size_t sz, size_t n, void* out) {
  static_cast<std::string*>(out)->append(p, sz * n);
  return sz * n;
}

static size_t on_header(char* p, size_t sz, size_t n, void* out) {
  std::string line(p, sz * n);
  auto colon = line.find(':');
  if (colon != std::string::npos) {
    std::string key = line.substr(0, colon), val = line.substr(colon + 1);
    std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    auto b = val.find_first_not_of(" \t"), e = val.find_last_not_of(" \t\r\n");
    (*static_cast<std::map<std::string, std::string>*>(out))[key] = b == std::string::npos ? "" : val.substr(b, e - b + 1);
  }
  return sz * n;
}

HttpResp http(const std::string& method, const std::string& url, const std::vector<std::string>& headers,
              const std::string& body, long timeout_ms) {
  HttpResp r;
  CURL* c = curl_easy_init();
  if (!c) return {0, "curl_easy_init failed", {}};
  curl_slist* hs = nullptr;
  for (const auto& h : headers) hs = curl_slist_append(hs, h.c_str());
  curl_easy_setopt(c, CURLOPT_URL, url.c_str());
  curl_easy_setopt(c, CURLOPT_CUSTOMREQUEST, method.c_str());
  if (method != "GET") {
    curl_easy_setopt(c, CURLOPT_POSTFIELDS, body.data());
    curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE_LARGE, (curl_off_t)body.size());
  }
  curl_easy_setopt(c, CURLOPT_HTTPHEADER, hs);
  curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, on_body);
  curl_easy_setopt(c, CURLOPT_WRITEDATA, &r.body);
  curl_easy_setopt(c, CURLOPT_HEADERFUNCTION, on_header);
  curl_easy_setopt(c, CURLOPT_HEADERDATA, &r.headers);
  curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT_MS, std::min(timeout_ms, 1000L));
  curl_easy_setopt(c, CURLOPT_TIMEOUT_MS, timeout_ms);
  curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
  CURLcode rc = curl_easy_perform(c);
  if (rc == CURLE_OK) curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &r.status);
  else r.body = curl_easy_strerror(rc);
  curl_slist_free_all(hs);
  curl_easy_cleanup(c);
  return r;
}
