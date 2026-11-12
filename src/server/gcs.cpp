// SQLite snapshot persistence: one GCS object, whole-file upload per change, ifGenerationMatch guards writers.
#include <unistd.h>

#include <cstdio>
#include <fstream>
#include <optional>
#include <sstream>

#include "server.hpp"

static const long kTimeoutMs = 10000;
static const char* kObject = "dionisio.db";
static std::string g_gen = "0";  // generation of the object we last read or wrote; guarded by g_mu (or startup)

static std::string bucket() { return env("GCS_BUCKET"); }
static std::string endpoint() { return env("GCS_ENDPOINT", "https://storage.googleapis.com"); }

// Metadata-server token, fetched on every call (no caching). Empty header list when GCS_ENDPOINT is set (fake GCS).
static std::optional<std::vector<std::string>> auth() {
  if (!env("GCS_ENDPOINT").empty()) return std::vector<std::string>{};
  auto r = http("GET", "http://metadata.google.internal/computeMetadata/v1/instance/service-accounts/default/token",
                {"Metadata-Flavor: Google"}, "", 2000);
  if (r.status != 200) return std::nullopt;
  auto j = json::parse(r.body, nullptr, false);
  if (!j.is_object() || !j.contains("access_token")) return std::nullopt;
  return std::vector<std::string>{"Authorization: Bearer " + j["access_token"].get<std::string>()};
}

static HttpResp download() {
  auto h = auth();
  if (!h) return {0, "no token", {}};
  return http("GET", endpoint() + "/storage/v1/b/" + bucket() + "/o/" + kObject + "?alt=media", *h, "", kTimeoutMs);
}

static bool write_file(const std::string& path, const std::string& bytes) {
  std::ofstream f(path, std::ios::binary | std::ios::trunc);
  f.write(bytes.data(), (std::streamsize)bytes.size());
  return f.good();
}

static std::string read_file(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  std::stringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

static std::string gen_of(const HttpResp& r) {
  auto it = r.headers.find("x-goog-generation");
  return it == r.headers.end() ? "0" : it->second;
}

bool gcs_restore() {
  if (bucket().empty()) return true;
  auto r = download();
  if (r.status == 404) {
    g_gen = "0";
    return true;
  }
  if (r.status != 200) {
    std::fprintf(stderr, "gcs restore: status %ld %s\n", r.status, r.status == 0 ? r.body.c_str() : "");
    return false;
  }
  std::string tmp = g_db_path + ".restore";
  if (!write_file(tmp, r.body) || std::rename(tmp.c_str(), g_db_path.c_str()) != 0) return false;
  g_gen = gen_of(r);
  return true;
}

// Copy src's main database into dst's main database (sqlite3_backup keeps dst's connection valid).
static bool copy_db(sqlite3* src, sqlite3* dst) {
  sqlite3_backup* b = sqlite3_backup_init(dst, "main", src, "main");
  if (!b) return false;
  int rc = sqlite3_backup_step(b, -1);
  return sqlite3_backup_finish(b) == SQLITE_OK && rc == SQLITE_DONE;
}

// Someone else wrote first: replace the live DB contents in place with the latest object.
static void reload() {
  auto r = download();
  if (r.status != 200) return;
  std::string tmp = g_db_path + ".reload";
  sqlite3* src = nullptr;
  if (write_file(tmp, r.body) && sqlite3_open_v2(tmp.c_str(), &src, SQLITE_OPEN_READONLY, nullptr) == SQLITE_OK &&
      copy_db(src, g_db)) {
    g_gen = gen_of(r);
    g_saved = sqlite3_total_changes(g_db);  // local == remote again
    ++g_reloads;
  }
  sqlite3_close(src);
  std::remove(tmp.c_str());
}

bool persist() {
  if (bucket().empty()) {
    g_saved = sqlite3_total_changes(g_db);
    return true;
  }
  std::string tmp = g_db_path + ".snap";
  sqlite3* dst = nullptr;
  bool ok = sqlite3_open(tmp.c_str(), &dst) == SQLITE_OK && copy_db(g_db, dst);
  sqlite3_close(dst);
  std::string bytes = ok ? read_file(tmp) : "";
  std::remove(tmp.c_str());
  if (!ok || bytes.empty()) return false;

  // One attempt, fail fast: on any failure g_saved stays behind, so the next mut request (even a no-op retry)
  // uploads again.
  auto h = auth();
  if (!h) return false;
  h->push_back("Content-Type: application/octet-stream");
  auto r = http("POST",
                endpoint() + "/upload/storage/v1/b/" + bucket() + "/o?uploadType=media&name=" + kObject +
                    "&ifGenerationMatch=" + g_gen,
                *h, bytes, kTimeoutMs);
  if (r.status == 412) {
    reload();
    return false;  // this request's write is lost; the client retries
  }
  if (r.status != 200) return false;
  auto j = json::parse(r.body, nullptr, false);
  if (!j.is_object() || !j.contains("generation") || !j["generation"].is_string()) return false;
  g_gen = j["generation"].get<std::string>();
  g_saved = sqlite3_total_changes(g_db);
  return true;
}
