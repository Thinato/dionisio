// One header for the dionisio-server translation units.
#pragma once
#include <sqlite3.h>

#include <mutex>
#include <nlohmann/json.hpp>
#include <string>

#include "core.hpp"

namespace httplib {
class Server;
struct Request;
struct Response;
}  // namespace httplib
using json = nlohmann::json;

// main.cpp
std::string env(const char* key, const char* def = "");
std::string trim(const std::string& s);
extern Date g_anchor;  // CYCLE_ANCHOR
Date today_of(const httplib::Request& req);  // ?today= only when ALLOW_TODAY_OVERRIDE=1
void reply(httplib::Response& res, int status, const json& body);

// db.cpp. ponytail: one global lock around every DB access + persist; 25 users.
extern sqlite3* g_db;
extern std::string g_db_path;  // DB_PATH
extern std::mutex g_mu;
extern int g_saved;         // sqlite3_total_changes at the last successful persist or reload
extern unsigned g_reloads;  // bumped by every 412 reload
template <class F>
auto db(F&& f) {
  std::lock_guard<std::mutex> lock(g_mu);
  return f();
}
void open_db();  // opens g_db_path into g_db and applies the schema (idempotent). Throws on failure.
// Run sql with positional params (null/bool/int/string); returns rows as [{col: value}]. Throws on error.
json q(const std::string& sql, const json& params = json::array());
template <class F>
void tx(F&& f) {
  q("BEGIN");
  try {
    f();
    q("COMMIT");
  } catch (...) {
    sqlite3_exec(g_db, "ROLLBACK", nullptr, nullptr, nullptr);
    throw;
  }
}
json order_json(int64_t id, Date today, bool with_lines);  // null if missing
json home_json(Date today);
json items_json();
json requests_json(Date today);
json cycles_json(Date today);
std::pair<int, json> import_order(const ParsedOrder& o, Date today);  // 201 {id,...} | 409

// gcs.cpp. Without GCS_BUCKET: restore is a no-op, persist only advances g_saved.
bool gcs_restore();  // startup, before open_db(): fetch the DB object into g_db_path. false -> exit(1)
bool persist();      // g_mu held, total_changes != g_saved. Updates g_saved on success/reload. false -> 503

// slack.cpp. Slack HTTP calls never run under g_mu.
void slack_commands(const httplib::Request& req, httplib::Response& res);      // POST /slack/commands
void slack_interactions(const httplib::Request& req, httplib::Response& res);  // POST /slack/interactions
bool slack_dm(const std::string& user, const std::string& text, int64_t renew_pref = 0);  // >0 adds a Renovar button
