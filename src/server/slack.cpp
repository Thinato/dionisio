// Slack: signed slash command + interactivity, Web API calls (dry-run when SLACK_BOT_TOKEN is empty).
#include <httplib.h>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>

#include <cstdio>
#include <cstdlib>

#include "server.hpp"

using httplib::Request;
using httplib::Response;

static const char* kRetry = "Não consegui abrir o formulário, rode /dionisio de novo.";
static const char* kHelp =
    "/dionisio: abre o formulário de preferências\n"
    "/dionisio list: suas preferências ativas\n"
    "/dionisio drop <n>: remove a preferência n da lista";

// HMAC-SHA256(secret, "v0:" + ts + ":" + raw body), 5 minute window, constant-time compare.
static bool signed_ok(const Request& req) {
  static const std::string secret = env("SLACK_SIGNING_SECRET");
  std::string ts = req.get_header_value("X-Slack-Request-Timestamp");
  std::string sig = req.get_header_value("X-Slack-Signature");
  if (secret.empty() || ts.empty() || ts.size() > 12 || ts.find_first_not_of("0123456789") != std::string::npos)
    return false;
  long long now = std::chrono::duration_cast<std::chrono::seconds>(
                      std::chrono::system_clock::now().time_since_epoch()).count();
  if (std::llabs(now - std::stoll(ts)) > 300) return false;
  std::string base = "v0:" + ts + ":" + req.body;
  unsigned char mac[EVP_MAX_MD_SIZE];
  unsigned len = 0;
  HMAC(EVP_sha256(), secret.data(), (int)secret.size(), (const unsigned char*)base.data(), base.size(), mac, &len);
  std::string want = "v0=";
  char hex[3];
  for (unsigned i = 0; i < len; ++i) {
    std::snprintf(hex, sizeof hex, "%02x", mac[i]);
    want += hex;
  }
  return sig.size() == want.size() && CRYPTO_memcmp(sig.data(), want.data(), want.size()) == 0;
}

static json post_json(const std::string& url, const json& payload, const std::string& token) {
  std::vector<std::string> h = {"Content-Type: application/json; charset=utf-8"};
  if (!token.empty()) h.push_back("Authorization: Bearer " + token);
  auto r = http("POST", url, h, payload.dump(-1, ' ', false, json::error_handler_t::replace));
  if (r.status != 200) return {{"ok", false}, {"error", "http " + std::to_string(r.status) + " " + r.body}};
  auto j = json::parse(r.body, nullptr, false);
  return j.is_object() ? j : json{{"ok", r.body == "ok"}};  // response_url answers plain "ok"
}

static json slack_api(const std::string& method, const json& payload) {
  static const std::string token = env("SLACK_BOT_TOKEN"), base = env("SLACK_API_BASE", "https://slack.com/api");
  if (token.empty()) {
    std::printf("[slack dry-run] %s %s\n", method.c_str(), payload.dump().c_str());
    return {{"ok", true}};
  }
  auto r = post_json(base + "/" + method, payload, token);
  if (!r.value("ok", false)) std::fprintf(stderr, "slack %s failed: %s\n", method.c_str(), r.dump().c_str());
  return r;
}

bool slack_dm(const std::string& user, const std::string& text, const json& blocks) {
  json p = {{"channel", user}, {"text", text}};
  if (!blocks.is_null()) p["blocks"] = blocks;
  return slack_api("chat.postMessage", p).value("ok", false);
}

static std::string trim(const std::string& s) {
  auto b = s.find_first_not_of(" \t\r\n"), e = s.find_last_not_of(" \t\r\n");
  return b == std::string::npos ? "" : s.substr(b, e - b + 1);
}

// First n UTF-8 code points (Slack limits count characters).
static std::string utf8_prefix(const std::string& s, size_t n) {
  size_t i = 0, cps = 0;
  for (; i < s.size(); ++i)
    if (((unsigned char)s[i] & 0xC0) != 0x80 && cps++ == n) break;
  return s.substr(0, i);
}

static std::string escape(const std::string& s) {  // Slack mrkdwn control characters
  std::string out;
  for (char c : s) out += c == '&' ? "&amp;" : c == '<' ? "&lt;" : c == '>' ? "&gt;" : std::string(1, c);
  return out;
}

static json opt(const std::string& text, const std::string& value) {
  return {{"text", {{"type", "plain_text"}, {"text", text}}}, {"value", value}};
}

static json input(const char* block, const char* label, json element, bool optional = false) {
  element["action_id"] = "v";
  return {{"type", "input"},
          {"block_id", block},
          {"optional", optional},
          {"label", {{"type", "plain_text"}, {"text", label}}},
          {"element", element}};
}

// ponytail: static_select holds 100 options (99 items + Outro); external_select if the catalogue grows past that.
static json modal(const json& items) {
  json options = json::array();
  for (auto& it : items) options.push_back(opt(utf8_prefix(it["name"], 75), std::to_string(it["id"].get<int64_t>())));
  options.push_back(opt("Outro (escreva abaixo)", "other"));
  json counts = json::array();
  for (int i = 1; i <= 5; ++i) counts.push_back(opt(std::to_string(i), std::to_string(i)));
  return {{"type", "modal"},
          {"callback_id", "pref"},
          {"title", {{"type", "plain_text"}, {"text", "Dionisio"}}},
          {"submit", {{"type", "plain_text"}, {"text", "Salvar"}}},
          {"close", {{"type", "plain_text"}, {"text", "Cancelar"}}},
          {"blocks",
           {input("item", "Item", {{"type", "static_select"}, {"options", options}}),
            input("other", "Outro item", {{"type", "plain_text_input"}, {"max_length", 75}}, true),
            input("count", "Quantidade", {{"type", "static_select"}, {"options", counts}}),
            input("period", "Período",
                  {{"type", "radio_buttons"}, {"options", {opt("por dia", "day"), opt("por semana", "week")}}})}}};
}

// The user's active preferences; `list` and `drop <n>` share this ordering.
static json active_prefs(const std::string& user, Date today) {
  return q(
      "SELECT p.id, p.count, p.period, p.expires_on, COALESCE(i.name, p.other_text) AS name, i.active "
      "FROM preferences p LEFT JOIN items i ON i.id = p.item_id "
      "WHERE p.slack_user_id = ? AND p.expires_on >= ? ORDER BY p.expires_on, p.id",
      {user, iso(today)});
}

static std::string describe(const json& p) {
  return escape(p["name"].get<std::string>()) + " " + std::to_string(p["count"].get<int>()) + "x/" +
         (p["period"] == "day" ? "dia" : "semana") + " até " + p["expires_on"].get<std::string>() +
         (p["active"] == 0 ? " (arquivado)" : "");
}

static bool all_digits(const std::string& s, size_t max = 18) {
  return !s.empty() && s.size() <= max && s.find_first_not_of("0123456789") == std::string::npos;
}

void slack_commands(const Request& req, Response& res) {
  if (!signed_ok(req)) return reply(res, 401, {{"error", "bad signature"}});
  std::string user = req.get_param_value("user_id"), text = trim(req.get_param_value("text"));
  Date today = today_of(req);
  auto ephemeral = [&](const std::string& t) { reply(res, 200, {{"response_type", "ephemeral"}, {"text", t}}); };

  if (text.empty()) {
    auto items = db([] { return q("SELECT id, name FROM items WHERE active = 1 ORDER BY name LIMIT 99"); });
    auto r = slack_api("views.open", {{"trigger_id", req.get_param_value("trigger_id")}, {"view", modal(items)}});
    if (!r.value("ok", false)) return ephemeral(kRetry);
    res.status = 200;
    return;
  }
  if (text == "list") {
    auto rows = db([&] { return active_prefs(user, today); });
    if (rows.empty()) return ephemeral("Você não tem preferências ativas. Use /dionisio para criar uma.");
    std::string out;
    for (size_t i = 0; i < rows.size(); ++i) out += std::to_string(i + 1) + ". " + describe(rows[i]) + "\n";
    return ephemeral(out);
  }
  if (text.rfind("drop", 0) == 0) {
    std::string arg = trim(text.substr(4));
    if (!all_digits(arg, 4)) return ephemeral("Use /dionisio drop <n> (veja /dionisio list).");
    size_t n = std::stoul(arg);
    return ephemeral(db([&]() -> std::string {
      auto rows = active_prefs(user, today);
      if (n < 1 || n > rows.size()) return "Não existe a preferência " + arg + ". Veja /dionisio list.";
      q("DELETE FROM preferences WHERE id = ?", json::array({rows[n - 1]["id"]}));
      return "Removida: " + describe(rows[n - 1]);
    }));
  }
  ephemeral(kHelp);
}

// view.state.values.<block>.v -> selected_option.value or value ("" if absent).
static std::string field(const json& values, const char* block) {
  if (!values.contains(block) || !values[block].contains("v")) return "";
  const json& e = values[block]["v"];
  if (e.contains("selected_option") && e["selected_option"].is_object()) return e["selected_option"].value("value", "");
  return e.contains("value") && e["value"].is_string() ? e["value"].get<std::string>() : "";
}

static void view_submission(const json& p, const std::string& user, Date today, Response& res) {
  const json& v = p.at("view").at("state").at("values");
  std::string item = field(v, "item"), other = utf8_prefix(trim(field(v, "other")), 75);
  std::string count = field(v, "count"), period = field(v, "period");
  json errors = json::object();
  if (item == "other" && norm_name(other).empty()) errors["other"] = "Escreva o nome do item.";
  if (item != "other" && !all_digits(item)) errors["item"] = "Escolha um item.";
  if (count.size() != 1 || count[0] < '1' || count[0] > '5') errors["count"] = "Escolha de 1 a 5.";
  if (period != "day" && period != "week") errors["period"] = "Escolha dia ou semana.";
  if (errors.empty()) {
    std::string exp = iso(next_boundary_after(today, g_anchor));
    bool ok = db([&] {
      if (item == "other") {
        q("INSERT INTO preferences (slack_user_id, other_text, other_norm, count, period, expires_on) "
          "VALUES (?, ?, ?, ?, ?, ?) ON CONFLICT (slack_user_id, other_norm) WHERE other_norm IS NOT NULL "
          "DO UPDATE SET other_text = excluded.other_text, count = excluded.count, period = excluded.period, "
          "expires_on = excluded.expires_on",
          {user, other, norm_name(other), count[0] - '0', period, exp});
        return true;
      }
      if (q("SELECT 1 FROM items WHERE id = ? AND active = 1", {std::stoll(item)}).empty()) return false;
      q("INSERT INTO preferences (slack_user_id, item_id, count, period, expires_on) VALUES (?, ?, ?, ?, ?) "
        "ON CONFLICT (slack_user_id, item_id) WHERE item_id IS NOT NULL "
        "DO UPDATE SET count = excluded.count, period = excluded.period, expires_on = excluded.expires_on",
        {user, std::stoll(item), count[0] - '0', period, exp});
      return true;
    });
    if (ok) {
      res.status = 200;  // empty body closes the modal
      return;
    }
    errors["item"] = "Item indisponível, escolha outro.";
  }
  reply(res, 200, {{"response_action", "errors"}, {"errors", errors}});
}

// "Renovar" button on a renewal DM: owner only; moves expires_on to the next boundary.
static void renew(const json& p, const std::string& user, Date today) {
  const json& actions = p.value("actions", json::array());
  if (actions.empty() || actions[0].value("action_id", "") != "renew") return;
  std::string id = actions[0].value("value", "");
  if (!all_digits(id)) return;
  std::string text = db([&]() -> std::string {
    auto r = q("SELECT p.*, COALESCE(i.name, p.other_text) AS name, i.active FROM preferences p "
               "LEFT JOIN items i ON i.id = p.item_id WHERE p.id = ?",
               {std::stoll(id)});
    if (r.empty()) return "Essa preferência não existe mais. Use /dionisio para criar outra.";
    if (r[0]["slack_user_id"] != user) return "";
    auto exp = parse_date(r[0]["expires_on"].get<std::string>());
    if (!exp) return "";
    r[0]["expires_on"] = iso(renew_to(*exp, today, g_anchor));
    q("UPDATE preferences SET expires_on = ? WHERE id = ?", {r[0]["expires_on"], std::stoll(id)});
    return "Renovada: " + describe(r[0]);
  });
  std::string url = p.value("response_url", "");
  if (!text.empty() && !url.empty()) post_json(url, {{"replace_original", true}, {"text", text}}, "");
}

void slack_interactions(const Request& req, Response& res) {
  if (!signed_ok(req)) return reply(res, 401, {{"error", "bad signature"}});
  json p = json::parse(req.get_param_value("payload"));
  std::string type = p.value("type", "");
  std::string user = p.contains("user") ? p["user"].value("id", "") : "";
  if (user.empty()) return reply(res, 400, {{"error", "missing user"}});
  if (type == "view_submission") return view_submission(p, user, today_of(req), res);
  if (type == "block_actions") renew(p, user, today_of(req));
  res.status = 200;
}
