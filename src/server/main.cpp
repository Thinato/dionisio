#include <curl/curl.h>
#include <httplib.h>
#include <openssl/crypto.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <set>
#include <sstream>

#include "server.hpp"

using httplib::Request;
using httplib::Response;

Date g_anchor = kAnchor;

std::string env(const char* key, const char* def) {
  const char* v = std::getenv(key);
  return v && *v ? v : def;
}

std::string trim(const std::string& s) {
  auto b = s.find_first_not_of(" \t\r\n"), e = s.find_last_not_of(" \t\r\n");
  return b == std::string::npos ? "" : s.substr(b, e - b + 1);
}

void reply(Response& res, int status, const json& body) {
  res.status = status;
  res.set_content(body.dump(-1, ' ', false, json::error_handler_t::replace), "application/json");
}

Date today_of(const Request& req) {
  if (env("ALLOW_TODAY_OVERRIDE") == "1" && req.has_param("today"))
    if (auto d = parse_date(req.get_param_value("today"))) return *d;
  return today_brt();
}

// Bearer or X-Api-Token, constant-time compare. Never from the query string.
static bool token_ok(const Request& req) {
  static const std::string want = env("API_TOKEN");
  std::string got = req.get_header_value("Authorization");
  got = got.rfind("Bearer ", 0) == 0 ? got.substr(7) : req.get_header_value("X-Api-Token");
  return !want.empty() && got.size() == want.size() && CRYPTO_memcmp(got.data(), want.data(), want.size()) == 0;
}

// Wrap a mutating route: persist anything not yet uploaded (even on error paths) before responding.
// Comparing against g_saved, not the request's own baseline, makes a retried no-op upload a change an
// earlier failed persist left behind. A 412 reload during this request may have wiped its write -> 503.
static httplib::Server::Handler mut(httplib::Server::Handler h) {
  return [h](const Request& req, Response& res) {
    auto [before, reloads] = db([] { return std::pair{sqlite3_total_changes(g_db), g_reloads}; });
    auto persisted = [&] {
      std::lock_guard<std::mutex> lock(g_mu);
      bool changed = sqlite3_total_changes(g_db) != before;
      bool ok = sqlite3_total_changes(g_db) == g_saved || persist();
      return ok && !(changed && g_reloads != reloads);
    };
    try {
      h(req, res);
    } catch (...) {
      persisted();
      throw;
    }
    if (!persisted()) reply(res, 503, {{"error", "persist failed, retry"}});
  };
}

// null / "" clear the field; otherwise must pass `ok`. nullopt = invalid.
template <class Ok>
static std::optional<json> nullable(const json& v, Ok ok) {
  if (v.is_null() || (v.is_string() && v.get_ref<const std::string&>().empty())) return json(nullptr);
  if (v.is_string() && ok(v.get_ref<const std::string&>())) return v;
  return std::nullopt;
}
static bool valid_date(const std::string& s) { return parse_date(s).has_value(); }
static bool valid_linha(const std::string& s) {
  return s.size() <= 64 && s.find_first_not_of("0123456789. ") == std::string::npos;
}

static void get_order(const Request& req, Response& res) {
  auto o = db([&] { return order_json(std::stoll(req.matches[1]), today_of(req), true); });
  if (o.is_null()) return reply(res, 404, {{"error", "order not found"}});
  reply(res, 200, o);
}

static void post_orders(const Request& req, Response& res) {
  ParsedOrder o;
  try {
    o = parse_export(req.body);
  } catch (const std::exception& e) {
    return reply(res, 400, {{"error", std::string("unknown export format: ") + e.what()}});
  }
  auto [status, body] = db([&] { return import_order(o, today_of(req)); });
  reply(res, status, body);
}

// {"lines":{"<id>":"arrived|missing|pending"}, "due_date", "linha_digitavel", "paid_on"}; every key optional.
static void post_order(const Request& req, Response& res) {
  int64_t id = std::stoll(req.matches[1]);
  json b = json::parse(req.body);
  if (!b.is_object()) return reply(res, 400, {{"error", "expected a JSON object"}});
  std::vector<std::pair<std::string, json>> fields;
  using Check = std::pair<const char*, bool (*)(const std::string&)>;
  for (auto [key, ok] : {Check{"due_date", valid_date}, Check{"paid_on", valid_date}, Check{"linha_digitavel", valid_linha}}) {
    if (!b.contains(key)) continue;
    auto v = nullable(b[key], ok);
    if (!v) return reply(res, 400, {{"error", std::string("invalid ") + key}});
    fields.emplace_back(key, *v);
  }
  json lines = b.value("lines", json::object());
  if (!lines.is_object()) return reply(res, 400, {{"error", "lines must be an object"}});
  for (auto& [lid, st] : lines.items())
    if (!st.is_string() || (st != "pending" && st != "arrived" && st != "missing") || lid.empty() ||
        lid.find_first_not_of("0123456789") != std::string::npos || lid.size() > 18)
      return reply(res, 400, {{"error", "invalid line " + lid}});

  auto out = db([&]() -> std::pair<int, json> {
    if (q("SELECT 1 FROM orders WHERE id = ?", {id}).empty()) return {404, {{"error", "order not found"}}};
    std::set<int64_t> mine;
    for (auto& r : q("SELECT id FROM order_lines WHERE order_id = ?", {id})) mine.insert(r["id"].get<int64_t>());
    for (auto& [lid, st] : lines.items())
      if (!mine.count(std::stoll(lid))) return {400, {{"error", "line " + lid + " is not in this order"}}};
    tx([&] {
      for (auto& [lid, st] : lines.items())
        q("UPDATE order_lines SET arrival = ? WHERE id = ? AND arrival <> ?", {st, std::stoll(lid), st});
      for (auto& [k, v] : fields) q("UPDATE orders SET " + k + " = ? WHERE id = ? AND " + k + " IS NOT ?", {v, id, v});
    });
    return {200, order_json(id, today_of(req), true)};
  });
  reply(res, out.first, out.second);
}

// {pack_size?: int>0|null, admin_min_packs?: int>=0, active?: bool}
static void post_item(const Request& req, Response& res) {
  int64_t id = std::stoll(req.matches[1]);
  json b = json::parse(req.body);
  if (!b.is_object()) return reply(res, 400, {{"error", "expected a JSON object"}});
  std::vector<std::pair<std::string, json>> fields;
  if (b.contains("pack_size")) {
    auto& v = b["pack_size"];
    if (!v.is_null() && !(v.is_number_integer() && v.get<int64_t>() > 0 && v.get<int64_t>() < 100000))
      return reply(res, 400, {{"error", "pack_size must be a positive integer or null"}});
    fields.emplace_back("pack_size", v);
  }
  if (b.contains("admin_min_packs")) {
    auto& v = b["admin_min_packs"];
    if (!(v.is_number_integer() && v.get<int64_t>() >= 0 && v.get<int64_t>() < 100000))
      return reply(res, 400, {{"error", "admin_min_packs must be an integer >= 0"}});
    fields.emplace_back("admin_min_packs", v);
  }
  if (b.contains("active")) {
    if (!b["active"].is_boolean()) return reply(res, 400, {{"error", "active must be a boolean"}});
    fields.emplace_back("active", b["active"].get<bool>() ? 1 : 0);
  }
  auto out = db([&]() -> std::pair<int, json> {
    if (q("SELECT 1 FROM items WHERE id = ?", {id}).empty()) return {404, {{"error", "item not found"}}};
    tx([&] {
      for (auto& [k, v] : fields) q("UPDATE items SET " + k + " = ? WHERE id = ? AND " + k + " IS NOT ?", {v, id, v});
    });
    for (auto& it : items_json())
      if (it["id"] == id) return {200, it};
    return {404, {{"error", "item not found"}}};
  });
  reply(res, out.first, out.second);
}

// Daily 09:00 job: collect (under lock) -> send (no lock) -> record (under lock). Any failed send -> 500 so
// Scheduler retries; the notifications PK dedups what already went out.
static void daily(const Request& req, Response& res) {
  static std::atomic<bool> running{false};
  if (running.exchange(true)) return reply(res, 409, {{"error", "already running"}});
  struct Done { ~Done() { running = false; } } done;
  struct Dm { std::string kind, key, target, text; int64_t ref, renew; };
  Date today = today_of(req);
  std::vector<std::string> admins;
  std::stringstream ids(env("ADMIN_SLACK_IDS"));
  for (std::string id; std::getline(ids, id, ',');)
    if (std::string t = trim(id); !t.empty()) admins.push_back(t);

  auto todo = db([&] {
    std::vector<Dm> out;
    auto add = [&](Dm dm) {
      if (q("SELECT 1 FROM notifications WHERE kind = ? AND ref_id = ? AND on_date = ? AND target = ?",
            {dm.kind, dm.ref, dm.key, dm.target}).empty())
        out.push_back(std::move(dm));
    };
    for (auto& r : q("SELECT id FROM orders WHERE paid_on IS NULL ORDER BY id")) {
      json o = order_json(r["id"], today, false);
      const json& b = o["boleto"];
      int64_t amount = b["amount_due_cents"];
      auto due = b["due_date"].is_string() ? parse_date(b["due_date"].get<std::string>()) : std::nullopt;
      for (auto& admin : admins) {
        if (!due)
          add({"no_due_date", iso(today), admin, no_due_date_dm(o["supplier"].get<std::string>(),
                                                                o["external_id"].get<std::string>(), amount),
               o["id"], 0});
        else if (auto a = boleto_alert(*due, today))
          add({a->kind, iso(a->key), admin,
               boleto_dm(o["supplier"].get<std::string>(), amount, *due, today,
                         b["linha_digitavel"].is_string() ? b["linha_digitavel"].get<std::string>() : ""),
               o["id"], 0});
      }
    }
    for (auto& p : q("SELECT p.id, p.slack_user_id, p.count, p.period, p.expires_on, "
                     "COALESCE(i.name, p.other_text) AS name FROM preferences p LEFT JOIN items i ON i.id = p.item_id "
                     "WHERE p.expires_on >= ? AND (p.item_id IS NULL OR i.active = 1) ORDER BY p.id",
                     {iso(today)})) {
      auto exp = parse_date(p["expires_on"].get<std::string>());
      if (auto a = exp ? renewal_alert(*exp, today) : std::nullopt)
        add({a->kind, iso(a->key), p["slack_user_id"],
             renewal_dm(p["name"].get<std::string>(), p["count"], p["period"].get<std::string>(), *exp), p["id"],
             p["id"]});
    }
    return out;
  });

  std::vector<const Dm*> sent;
  for (auto& dm : todo)
    if (slack_dm(dm.target, dm.text, dm.renew)) sent.push_back(&dm);
  db([&] {
    for (auto* dm : sent)
      q("INSERT OR IGNORE INTO notifications (kind, ref_id, on_date, target) VALUES (?, ?, ?, ?)",
        {dm->kind, dm->ref, dm->key, dm->target});
  });
  int failed = (int)(todo.size() - sent.size());
  reply(res, failed ? 500 : 200, {{"sent", sent.size()}, {"failed", failed}});
}

int main() {
  curl_global_init(CURL_GLOBAL_DEFAULT);
  std::setvbuf(stdout, nullptr, _IOLBF, 0);
  g_db_path = env("DB_PATH", "/tmp/dionisio.db");
  auto anchor = parse_date(env("CYCLE_ANCHOR", "2026-11-09"));
  if (!anchor) {
    std::fprintf(stderr, "invalid CYCLE_ANCHOR\n");
    return 1;
  }
  g_anchor = *anchor;
  if (!gcs_restore()) {
    std::fprintf(stderr, "restore from GCS failed; refusing to start on an empty DB\n");
    return 1;
  }
  open_db();
  g_saved = sqlite3_total_changes(g_db);

  httplib::Server svr;
  svr.set_payload_max_length(1 << 20);
  svr.set_pre_routing_handler([](const Request& req, Response& res) {
    bool guarded = req.path.rfind("/api/", 0) == 0 || req.path.rfind("/jobs/", 0) == 0;
    if (guarded && !token_ok(req)) {
      reply(res, 401, {{"error", "unauthorized"}});
      return httplib::Server::HandlerResponse::Handled;
    }
    return httplib::Server::HandlerResponse::Unhandled;
  });
  svr.set_exception_handler([](const Request& req, Response& res, std::exception_ptr ep) {
    try {
      std::rethrow_exception(ep);
    } catch (const json::exception& e) {
      reply(res, 400, {{"error", std::string("bad request: ") + e.what()}});
    } catch (const std::exception& e) {
      std::fprintf(stderr, "%s %s: %s\n", req.method.c_str(), req.path.c_str(), e.what());
      reply(res, 500, {{"error", "internal error"}});
    }
  });

  svr.Get("/api/home", [](const Request& req, Response& res) {
    reply(res, 200, db([&] { return home_json(today_of(req)); }));
  });
  svr.Post("/api/orders", mut(post_orders));
  svr.Get(R"(/api/orders/(\d{1,18}))", get_order);
  svr.Post(R"(/api/orders/(\d{1,18}))", mut(post_order));
  svr.Get("/api/items", [](const Request&, Response& res) { reply(res, 200, db(items_json)); });
  svr.Post(R"(/api/items/(\d{1,18}))", mut(post_item));
  svr.Get("/api/requests", [](const Request& req, Response& res) {
    reply(res, 200, db([&] { return requests_json(today_of(req)); }));
  });
  svr.Post("/jobs/daily", mut(daily));
  svr.Post("/slack/commands", mut(slack_commands));
  svr.Post("/slack/interactions", mut(slack_interactions));
  svr.Get("/api/cycles", [](const Request& req, Response& res) {
    reply(res, 200, db([&] { return cycles_json(today_of(req)); }));
  });

  int port = std::atoi(env("PORT", "8080").c_str());
  std::printf("dionisio-server listening on :%d\n", port);
  if (!svr.listen("0.0.0.0", port)) {
    std::fprintf(stderr, "listen failed on :%d\n", port);
    return 1;
  }
}
