// Dionisio admin TUI: talks only to the server's /api over HTTP (libcurl).
#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <map>
#include <nlohmann/json.hpp>
#include <sstream>

#include "core.hpp"
#include "ftxui/component/component.hpp"
#include "ftxui/component/screen_interactive.hpp"
#include "ftxui/dom/elements.hpp"

using json = nlohmann::json;
using namespace ftxui;

static std::string g_url, g_token;

static HttpResp call(const std::string& path, const std::string* body = nullptr) {
  // 30 s: Cloud Run cold start
  return http(body ? "POST" : "GET", g_url + path, {"Authorization: Bearer " + g_token, "Content-Type: application/json"}, body ? *body : "", 30000);
}

static std::string slurp(const std::string& f) {
  std::ifstream in(f);
  std::stringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

static int do_import(const std::string& file) {
  std::string raw = slurp(file);
  if (raw.empty()) { puts("error: cannot read file"); return 1; }
  HttpResp r = call("/api/orders", &raw);
  if (r.status == 201) printf("created: order %d\n", json::parse(r.body, nullptr, false).value("id", 0));
  else if (r.status == 409) puts("rejected: already imported");
  else if (r.status == 400) puts("rejected: unknown export format");
  else { printf("error: HTTP %ld %s\n", r.status, r.body.c_str()); return 1; }
  return r.status == 201 ? 0 : 2;
}

static const char* kTabs[] = {"Home", "Order", "Catalogue", "New items", "Past cycles"};

struct Tui {
  int tab = 0, cur[5] = {0, 0, 0, 0, 0};
  std::string status = "ready";
  bool status_err = false;
  json home = json::object(), items = json::array(), reqs = json::array(), cycles = json::array();
  // order screen
  json order;
  std::map<int, std::string> dirty;  // line id -> arrival
  json fields = json::object();       // unsaved boleto edits
  // text prompt
  bool editing = false;
  std::string prompt, buf;
  std::function<void(const std::string&)> on_commit;

  void setdate(const char* k, const std::string& v) {
    if (!v.empty() && !parse_date(v)) { status_err = true; status = "invalid date (YYYY-MM-DD)"; return; }
    fields[k] = v.empty() ? json(nullptr) : json(v);
    discard_armed = false;
  }

  std::string bstr(const char* k) const {  // null -> ""
    auto& v = order["boleto"][k];
    return v.is_string() ? v.get<std::string>() : "";
  }

  bool unsaved() const { return !dirty.empty() || !fields.empty(); }

  json get(const std::string& path) {
    HttpResp r = call(path);
    if (r.status != 200) {
      fail(path, r);
      return nullptr;
    }
    json j = json::parse(r.body, nullptr, false);
    if (j.is_discarded()) {
      status_err = true;
      status = path + ": invalid JSON from server";
      return nullptr;
    }
    return j;
  }
  void fail(const std::string& what, const HttpResp& r) {
    status_err = true;
    std::string msg = r.status ? "HTTP " + std::to_string(r.status) : "network";
    json j = json::parse(r.body, nullptr, false);
    std::string d = j.is_object() && j.contains("error") ? j["error"].dump() : r.body.substr(0, 80);
    status = what + ": " + msg + " " + d;
  }
  void ok(const std::string& m) { status_err = false; status = m; }

  void refresh() {
    json h = get("/api/home");
    if (h.is_null()) return;  // keep the error on screen
    home = h;
    auto i = get("/api/items"); if (i.is_array()) items = i;
    auto q = get("/api/requests"); if (q.is_array()) reqs = q;
    auto c = get("/api/cycles"); if (c.is_array()) cycles = c;
    ok("refreshed");
  }

  void open_order(int id) {
    json o = get("/api/orders/" + std::to_string(id));
    if (o.is_null()) return;
    order = o; dirty.clear(); fields = json::object(); cur[1] = 0; tab = 1;
    ok("order " + order.value("external_id", ""));
  }

  std::string arrival(const json& l) const {
    auto it = dirty.find(l["id"].get<int>());
    return it != dirty.end() ? it->second : l.value("arrival", "pending");
  }
  // all pending -> order total, else sum of arrived price_cents
  long long amount() const {
    std::vector<LineState> ls;
    for (auto& l : order["lines"]) ls.push_back({l.value("price_cents", 0), arrival(l)});
    return amount_due(order.value("total_cents", 0), ls);
  }

  bool save() {
    if (!order.is_object() || !unsaved()) return true;
    json b = fields;
    if (!dirty.empty()) for (auto& [id, a] : dirty) b["lines"][std::to_string(id)] = a;
    std::string s = b.dump();
    HttpResp r = call("/api/orders/" + std::to_string(order["id"].get<int>()), &s);
    if (r.status / 100 != 2) { fail("save", r); return false; }
    dirty.clear(); fields = json::object(); discard_armed = false;
    json o = get("/api/orders/" + std::to_string(order["id"].get<int>()));
    if (!o.is_null()) order = o;
    ok("saved");
    refresh();
    return true;
  }

  // Leaving the order screen saves. If saving fails, a second attempt discards the edits instead.
  bool discard_armed = false;
  bool leave_order() {
    if (tab != 1 || save()) {
      discard_armed = false;
      return true;
    }
    if (discard_armed) {
      dirty.clear(); fields = json::object(); discard_armed = false;
      ok("unsaved changes discarded");
      return true;
    }
    discard_armed = true;
    status += "  (press again to discard changes)";
    return false;
  }

  void goto_tab(int t) {
    if (!leave_order()) return;
    tab = t;
    if (t != 1) refresh();
  }

  void ask(const std::string& p, const std::string& init, std::function<void(const std::string&)> f) {
    editing = true; prompt = p; buf = init; on_commit = std::move(f);
  }

  void post_item(const json& item, const json& body) {
    std::string s = body.dump();
    HttpResp r = call("/api/items/" + std::to_string(item["id"].get<int>()), &s);
    if (r.status / 100 != 2) return fail("item", r);
    refresh(); ok("saved item " + item.value("name", ""));
  }

  int list_size() const {
    switch (tab) {
      case 0: return (int)home.value("orders", json::array()).size();
      case 1: return order.is_object() ? (int)order["lines"].size() : 0;
      case 2: return (int)items.size();
      case 3: return (int)reqs.size();
      default: { int n = 0; for (auto& c : cycles) if (past(c)) n += (int)c["orders"].size(); return n; }
    }
  }

  bool past(const json& c) const { return c.value("start", "") != home.value("cycle", json::object()).value("start", ""); }

  json cycle_order(int idx) const {
    for (auto& c : cycles) if (past(c)) for (auto& o : c["orders"]) if (idx-- == 0) return o;
    return nullptr;
  }

  bool key(const Event& e) {
    if (editing) {
      if (e == Event::Return) { editing = false; on_commit(buf); }
      else if (e == Event::Escape) editing = false;
      else if (e == Event::Backspace) { if (!buf.empty()) buf.pop_back(); }
      else if (e.is_character()) buf += e.character();
      return true;
    }
    int& c = cur[tab];
    int n = list_size();
    if (e == Event::ArrowDown || e == Event::Character('j')) c = std::min(c + 1, std::max(n - 1, 0));
    else if (e == Event::ArrowUp || e == Event::Character('k')) c = std::max(c - 1, 0);
    else if (e == Event::Tab) goto_tab((tab + 1) % 5);
    else if (e == Event::TabReverse) goto_tab((tab + 4) % 5);
    else if (e.is_character() && e.character() >= "1" && e.character() <= "5") goto_tab(e.character()[0] - '1');
    else if (e == Event::Character('r')) { if (tab != 1) refresh(); }
    else if (e == Event::Character('s') && tab == 1) save();
    else if (e == Event::Character('q')) return !leave_order();
    else if (tab == 0) {
      auto orders = home.value("orders", json::array());
      if (e == Event::Return && c < (int)orders.size()) open_order(orders[c]["id"].get<int>());
    } else if (tab == 1 && order.is_object() && n > 0) {
      int id = order["lines"][c]["id"].get<int>();
      const char* mark = e == Event::Return ? "arrived"
                         : e == Event::Backspace || e == Event::Delete ? "missing"
                         : e == Event::Character('p') ? "pending" : nullptr;
      if (mark) { dirty[id] = mark; discard_armed = false; }
      else if (e == Event::Escape) goto_tab(0);
      else if (e == Event::Character('d')) ask("due date YYYY-MM-DD", bstr("due_date"), [this](auto& v) { setdate("due_date", v); });
      else if (e == Event::Character('l')) ask("linha digitavel", bstr("linha_digitavel"), [this](auto& v) {
        if (v.size() > 64 || v.find_first_not_of("0123456789. ") != std::string::npos) {
          status_err = true; status = "linha: up to 64 digits, dots and spaces"; return;
        }
        fields["linha_digitavel"] = v.empty() ? json(nullptr) : json(v);
        discard_armed = false;
      });
      else if (e == Event::Character('b')) ask("paid on YYYY-MM-DD", bstr("paid_on"), [this](auto& v) { setdate("paid_on", v); });
    } else if (tab == 2 && n > 0) {
      json it = items[c];
      auto num = [this, it](const char* key, const std::string& what) {
        ask(what, it[key].is_null() ? "" : std::to_string(it[key].get<int>()), [this, it, key](auto& v) {
          char* end; long x = strtol(v.c_str(), &end, 10);
          if (v.empty() || *end || x < 0) { status_err = true; status = "invalid number"; return; }
          post_item(it, json{{key, x}});
        });
      };
      if (e == Event::Character('p')) num("pack_size", "pack size");
      else if (e == Event::Character('m')) num("admin_min_packs", "admin min packs");
      else if (e == Event::Character('a') || e == Event::Return) post_item(it, json{{"active", !it.value("active", true)}});
    } else if (tab == 4 && e == Event::Return) {
      json o = cycle_order(c);
      if (o.is_object()) open_order(o["id"].get<int>());
    } else if (e == Event::Escape) return true;
    return true;
  }

  static Element row(Element e, bool sel) { return sel ? (e | inverted | focus) : e; }

  std::string money(const json& v) const { return v.is_number() ? brl(v.get<long long>()) : "-"; }

  Element boleto(const json& b) const {
    std::string due = b.contains("due_date") && !b["due_date"].is_null() ? b["due_date"].get<std::string>() : "no due date";
    std::string s = money(b.value("amount_due_cents", json())) + "  due " + due;
    if (b.contains("paid_on") && !b["paid_on"].is_null()) return text(s + "  paid " + b["paid_on"].get<std::string>()) | color(Color::Green);
    if (b.value("overdue", false)) return text(s + "  OVERDUE") | color(Color::Red);
    return text(s);
  }

  Element body() const {
    Elements rows;
    int c = cur[tab];
    if (tab == 0) {
      auto cy = home.value("cycle", json::object());
      rows.push_back(text("Cycle " + cy.value("start", "?") + " -> " + cy.value("next_boundary", "?")) | bold);
      rows.push_back(text("Suggestions") | underlined);
      int hidden = 0;
      for (auto& s : home.value("suggestions", json::array())) {
        if (s.value("suggested", 0) == 0 && !s.value("missing_last_delivery", false)) { hidden++; continue; }
        std::string l = "  " + s.value("item", "") + " [" + s.value("supplier", "") + "] units " + std::to_string(s.value("units", 0)) +
                        " -> order " + std::to_string(s.value("suggested", 0));
        if (s.value("unknown_pack", false)) l += "  (pack unknown)";
        if (s.value("missing_last_delivery", false)) l += "  (missing last delivery)";
        rows.push_back(text(l));
      }
      if (hidden) rows.push_back(text("  (+" + std::to_string(hidden) + " sem demanda)") | dim);
      rows.push_back(text("Orders (Enter opens)") | underlined);
      int i = 0;
      for (auto& o : home.value("orders", json::array())) {
        bool past = o.value("cycle_start", "") != cy.value("start", "");
        auto line = hbox({text(" " + o.value("supplier", "") + " " + o.value("external_id", "") + "  " + std::to_string(o.value("lines_done", 0)) + "/" +
                            std::to_string(o.value("lines_total", 0)) + (past ? "  (cycle " + o.value("cycle_start", "") + ")" : "") + "  "),
                         boleto(o.value("boleto", json::object()))});
        rows.push_back(row(line, i++ == c));
      }
    } else if (tab == 1) {
      if (!order.is_object()) return text("Open an order from Home or Past cycles.");
      rows.push_back(text(order.value("supplier", "") + " " + order.value("external_id", "") + "  delivery " +
                       (order["delivery_date"].is_null() ? "-" : order["delivery_date"].get<std::string>())) | bold);
      auto b = order["boleto"];
      auto fv = [&](const char* k, const char* fk) {
        if (fields.contains(fk)) return (fields[fk].is_string() ? fields[fk].get<std::string>() : "(clear)") + " *";
        return b.contains(k) && !b[k].is_null() ? b[k].get<std::string>() : std::string("-");
      };
      rows.push_back(text("due(d) " + fv("due_date", "due_date") + "   linha(l) " + fv("linha_digitavel", "linha_digitavel") + "   paid(b) " + fv("paid_on", "paid_on")));
      int i = 0;
      for (auto& l : order["lines"]) {
        std::string a = arrival(l);
        auto mark = a == "arrived" ? text("[x]") | color(Color::Green) : a == "missing" ? text("[-]") | color(Color::Red) : text("[ ]") | dim;
        rows.push_back(row(hbox({text(" "), mark, text(" " + std::to_string(l.value("qty", 0)) + "x " + l.value("name", "") + "  " + brl(l.value("price_cents", 0)))}), i++ == c));
      }
    } else if (tab == 2) {
      rows.push_back(text("p pack size, m min packs, a/Enter toggle active") | dim);
      int i = 0;
      for (auto& it : items) {
        std::string l = " " + it.value("supplier", "") + "  " + it.value("name", "") + "  pack " +
                        (it["pack_size"].is_null() ? "?" : std::to_string(it["pack_size"].get<int>())) + "  min " + std::to_string(it.value("admin_min_packs", 0)) +
                        (it.value("active", true) ? "" : "  [archived]");
        rows.push_back(row(text(l), i++ == c));
      }
    } else if (tab == 3) {
      int i = 0;
      for (auto& r : reqs) rows.push_back(row(text(" " + std::to_string(r.value("requesters", 0)) + "x  " + r.value("text", "")), i++ == c));
      if (reqs.empty()) rows.push_back(text("No requests."));
    } else {
      int i = 0;
      for (auto& cy : cycles) {
        if (!past(cy)) continue;
        rows.push_back(text("Cycle " + cy.value("start", "")) | bold);
        for (auto& o : cy["orders"]) {
          rows.push_back(row(text(" " + o.value("supplier", "") + "  total " + money(o["total_cents"]) + "  due " + money(o["boleto"]["amount_due_cents"]) +
                               (o["boleto"]["paid_on"].is_null() ? "" : "  paid " + o["boleto"]["paid_on"].get<std::string>())), i++ == c));
        }
      }
    }
    return vbox(rows) | vscroll_indicator | yframe | flex;
  }

  Element view() const {
    Elements tabs;
    for (int i = 0; i < 5; i++) tabs.push_back(text(" " + std::to_string(i + 1) + " " + kTabs[i] + " ") | (i == tab ? inverted : nothing));
    Element foot = text("");
    if (tab == 1 && order.is_object())
      foot = text("Arrived total: " + brl(amount()) + (unsaved() ? " *" : "") + "   Enter=arrived Bksp=missing p=pending s=save Esc=back");
    else foot = text("1-5/Tab switch  j/k move  Enter open  r refresh  q quit");
    Element st = editing ? hbox({text(prompt + ": "), text(buf), text("_") | blink}) | bold
                         : text(status) | (status_err ? color(Color::Red) : color(Color::GrayLight));
    return vbox({hbox(tabs), separator(), body(), separator(), foot, st});
  }
};

int main(int argc, char** argv) {
  const char* u = getenv("DIONISIO_URL");
  const char* tk = getenv("DIONISIO_TOKEN");
  g_url = u ? u : "http://localhost:8080";
  while (!g_url.empty() && g_url.back() == '/') g_url.pop_back();
  g_token = tk ? tk : "";
  if (argc >= 3 && std::string(argv[1]) == "import") return do_import(argv[2]);
  if (argc > 1) { puts("usage: dionisio [import FILE]"); return 1; }

  Tui app;
  app.refresh();
  auto screen = ScreenInteractive::Fullscreen();
  auto ui = Renderer([&] {
    try {
      return app.view();
    } catch (const std::exception& x) {  // unexpected JSON shape: show, don't die
      return text(std::string("render error: ") + x.what()) | color(Color::Red);
    }
  }) | CatchEvent([&](Event e) {
    try {
      if (!app.key(e)) screen.Exit();
    } catch (const std::exception& x) {  // unexpected JSON shape: show, don't die
      app.status_err = true; app.status = std::string("internal: ") + x.what();
    }
    return true;
  });
  screen.Loop(ui);
  return 0;
}
