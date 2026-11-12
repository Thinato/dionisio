// Dionisio admin TUI: talks only to the server's /api over HTTP (libcurl).
// Requests run on background threads; their results are applied on the UI thread via App::Post,
// so the screen never blocks: tabs paint from the last data and refresh behind a spinner.
#include <curl/curl.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <deque>
#include <fstream>
#include <functional>
#include <map>
#include <nlohmann/json.hpp>
#include <optional>
#include <set>
#include <sstream>
#include <thread>

#include "core.hpp"
#include "ftxui/component/component.hpp"
#include "ftxui/component/screen_interactive.hpp"
#include "ftxui/dom/elements.hpp"

using json = nlohmann::json;
using namespace ftxui;
using Clock = std::chrono::steady_clock;

static std::string g_url, g_token;
static App* g_app = nullptr;
static std::atomic<int> g_busy{0};  // requests in flight, read by the spinner ticker

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

// ---- look -------------------------------------------------------------------------------------
namespace th {
const Color acc = Color::RGB(245, 158, 76), blue = Color::RGB(127, 178, 255), green = Color::RGB(109, 209, 150),
            red = Color::RGB(255, 122, 112), yellow = Color::RGB(245, 192, 76), dim = Color::RGB(154, 163, 178),
            line = Color::RGB(58, 65, 78), sel = Color::RGB(28, 37, 51), ink = Color::RGB(14, 16, 20);
}
static const char* kGap = "   ";  // fixed space between table columns
static const char* kSpin[] = {"⠋", "⠙", "⠹", "⠸", "⠼", "⠴", "⠦", "⠧", "⠇", "⠏"};
static const char* kTabs[] = {"⌂ 1 Home", "☑ 2 Order", "▤ 3 Catalogue", "✚ 4 New items", "↺ 5 Past cycles"};

static size_t u8len(const std::string& s) {
  size_t n = 0;
  for (unsigned char c : s) n += (c & 0xC0) != 0x80;
  return n;
}
// Exactly w columns: cut with … or pad (right-aligned when asked). Assumes 1 column per code point.
static std::string fit(const std::string& s, int w, bool right = false) {
  size_t n = u8len(s);
  if ((int)n > w) {
    std::string out;
    int k = 0;
    for (size_t i = 0; i < s.size() && k < w - 1; k++) {
      size_t j = i + 1;
      while (j < s.size() && ((unsigned char)s[j] & 0xC0) == 0x80) j++;
      out.append(s, i, j - i);
      i = j;
    }
    return out + "…";
  }
  std::string pad(w - n, ' ');
  return right ? pad + s : s + pad;
}
static Element cell(const std::string& s, int w, bool right = false) { return text(fit(s, w, right)); }
static Element trow(Elements cs) {
  Elements out = {text(" ")};
  for (size_t i = 0; i < cs.size(); i++) {
    if (i) out.push_back(text(kGap));
    out.push_back(std::move(cs[i]));
  }
  return hbox(std::move(out));
}
static Element keycap(const std::string& k) { return text(" " + k + " ") | bgcolor(th::line) | color(Color::White); }
static Element bar(int done, int total, int width) {
  int f = total ? done * width / total : 0;
  std::string a, b;
  for (int i = 0; i < width; i++) (i < f ? a : b) += i < f ? "▰" : "▱";
  return hbox({text(a) | color(th::green), text(b) | color(th::line)});
}
// Read-only key lookup: a missing key or non-object gives null, so .get<> throws (caught) rather than asserting.
static const json& at(const json& j, const char* k) {
  static const json none;
  if (!j.is_object()) return none;
  auto it = j.find(k);
  return it == j.end() ? none : *it;
}
static std::string sstr(const json& j, const char* k, const std::string& dflt = "") {
  return j.contains(k) && j[k].is_string() ? j[k].get<std::string>() : dflt;
}
static std::string short_date(const std::string& d) {  // 2026-11-09 -> 09 Nov
  static const char* m[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
  int mo = d.size() == 10 ? std::atoi(d.substr(5, 2).c_str()) : 0;
  return mo >= 1 && mo <= 12 ? d.substr(8, 2) + " " + m[mo - 1] : d;
}

struct Banner {
  std::string title, detail;
  std::function<void()> retry;
};

struct Tui {
  int tab = 0, cur[5] = {0, 0, 0, 0, 0};
  json home = json::object(), items = json::array(), reqs = json::array(), cycles = json::array();
  std::set<std::string> loaded;
  std::optional<Clock::time_point> updated;

  // in-flight requests: key -> (start, generation). A result whose entry is gone or newer is dropped.
  struct Flight { Clock::time_point t0; int gen; };
  std::map<std::string, Flight> inflight;
  int gen = 0;

  std::string status = "ready";
  Color status_color = th::dim;
  std::optional<Banner> err;
  bool help = false, show_idle = false, only_unknown = false, only_archived = false;
  std::string filter[5];

  // order screen
  json order;
  std::map<int, std::string> dirty;  // line id -> arrival
  json fields = json::object();       // unsaved boleto edits
  std::function<void()> confirm;      // pending navigation, set while the unsaved-changes dialog is open
  std::function<void()> after_save;

  // catalogue editor
  struct Editor { int64_t id = 0; std::optional<int> pack; int min = 0; bool active = true; int field = 0; bool typed = false; };
  std::optional<Editor> ed;

  // one-line prompt
  bool editing = false;
  std::string prompt, buf;
  std::function<void(const std::string&)> on_commit, on_change;
  std::function<void()> on_cancel;

  // mouse targets, rebuilt every frame; deque keeps Box references stable while FTXUI renders
  mutable std::deque<Box> boxes;
  mutable std::vector<std::function<void()>> acts;
  mutable size_t modal_from = 0;  // with a dialog open, only targets registered after this index react
  std::function<void()> quit;

  void ok(const std::string& m, Color c = th::green) { status = m; status_color = c; }

  // ---- requests --------------------------------------------------------------------------------
  void request(const std::string& key, const std::string& path, std::optional<std::string> body, std::function<void(const HttpResp&)> done) {
    if (inflight.count(key)) return;
    int g = ++gen;
    inflight[key] = {Clock::now(), g};
    g_busy++;
    std::thread([this, key, path, body, done, g] {
      HttpResp r = call(path, body ? &*body : nullptr);
      g_app->Post(Closure([this, key, done, g, r] {
        g_busy--;
        auto it = inflight.find(key);
        if (it == inflight.end() || it->second.gen != g) return;  // cancelled
        inflight.erase(it);
        try {
          done(r);
        } catch (const std::exception& x) {
          ok(std::string("✗ internal: ") + x.what(), th::red);
        }
      }));
      g_app->PostEvent(Event::Custom);
    }).detach();
  }

  static std::string reason(const HttpResp& r) {
    if (r.status == 0) return "no answer from the server (" + r.body.substr(0, 60) + ")";
    if (r.status == 401) return "API token rejected: check DIONISIO_TOKEN";
    json j = json::parse(r.body, nullptr, false);
    std::string m = j.is_object() && j.contains("error") && j["error"].is_string() ? j["error"].get<std::string>() : r.body.substr(0, 80);
    return "HTTP " + std::to_string(r.status) + (m.empty() ? "" : ": " + m);
  }
  void failed(const std::string& what, const HttpResp& r, std::function<void()> retry) {
    err = Banner{what, reason(r), std::move(retry)};
  }

  void load(const std::string& key, const std::string& path, std::function<void(json)> apply) {
    request(key, path, std::nullopt, [=, this](const HttpResp& r) {
      json j = r.status == 200 ? json::parse(r.body, nullptr, false) : json();
      if (r.status != 200 || j.is_discarded()) {
        HttpResp e = r;
        if (r.status == 200) e.body = "{\"error\":\"invalid JSON from server\"}", e.status = 502;
        return failed("Couldn't load " + key, e, [=, this] { load(key, path, apply); load_tab(); if (tab != 0) load_home(); });
      }
      apply(std::move(j));
      loaded.insert(key);
      updated = Clock::now();
    });
  }
  void load_home() { load("home", "/api/home", [this](json j) { if (j.is_object()) home = std::move(j); }); }
  void load_items() { load("catalogue", "/api/items", [this](json j) { if (j.is_array()) items = std::move(j); }); }
  void load_reqs() { load("new items", "/api/requests", [this](json j) { if (j.is_array()) reqs = std::move(j); }); }
  void load_cycles() { load("past cycles", "/api/cycles", [this](json j) { if (j.is_array()) cycles = std::move(j); }); }
  void load_order(int id) {
    load("order", "/api/orders/" + std::to_string(id), [this](json j) { if (j.is_object()) order = std::move(j); });
  }
  void load_all() { load_home(); load_items(); load_reqs(); load_cycles(); }
  void load_tab() {
    if (tab == 0) load_home();
    else if (tab == 1) { if (order.is_object()) load_order(at(order, "id").get<int>()); }
    else if (tab == 2) load_items();
    else if (tab == 3) load_reqs();
    else { load_home(); load_cycles(); }
  }
  std::string tab_key() const {
    static const char* k[] = {"home", "order", "catalogue", "new items", "past cycles"};
    return k[tab];
  }

  // ---- navigation ------------------------------------------------------------------------------
  bool unsaved() const { return !dirty.empty() || !fields.empty(); }
  void navigate(std::function<void()> go) {
    if (inflight.count("save")) return ok("saving… one moment", th::blue);
    if (unsaved()) confirm = std::move(go);
    else go();
  }
  void goto_tab(int t) {
    if (ed && !ed_diff().empty()) return ok("● save (⏎) or cancel (Esc) the open item first", th::acc);
    navigate([this, t] { tab = t; ed.reset(); load_tab(); });
  }
  void open_order(int id) {
    navigate([this, id] {
      if (!order.is_object() || at(order, "id").get<int>() != id) { order = nullptr; loaded.erase("order"); }
      dirty.clear(); fields = json::object(); cur[1] = 0; tab = 1;
      inflight.erase("order");  // drop any load of a previous order
      load_order(id);
    });
  }

  // ---- order -----------------------------------------------------------------------------------
  std::string bstr(const char* k) const { return order.is_object() ? sstr(at(order, "boleto"), k) : ""; }
  std::string arrival(const json& l) const {
    auto it = dirty.find(at(l, "id").get<int>());
    return it != dirty.end() ? it->second : l.value("arrival", "pending");
  }
  long long amount() const {
    std::vector<LineState> ls;
    for (auto& l : at(order, "lines")) ls.push_back({l.value("price_cents", 0), arrival(l)});
    return amount_due(order.value("total_cents", 0), ls);
  }
  void mark(int i, const char* a) {
    if (!order.is_object() || i < 0 || i >= (int)at(order, "lines").size()) return;
    const json& l = at(order, "lines")[i];
    if (l.value("arrival", "pending") == a) dirty.erase(at(l, "id").get<int>());
    else dirty[at(l, "id").get<int>()] = a;
  }
  void cycle_line(int i) {
    if (!order.is_object() || i < 0 || i >= (int)at(order, "lines").size()) return;
    std::string a = arrival(at(order, "lines")[i]);
    mark(i, a == "pending" ? "arrived" : a == "arrived" ? "missing" : "pending");
    cur[1] = i;
  }
  void setdate(const char* k, const std::string& v) {
    if (!v.empty() && !parse_date(v)) return ok("▲ invalid date, use YYYY-MM-DD", th::yellow);
    fields[k] = v.empty() ? json(nullptr) : json(v);
  }
  void save() {
    if (!order.is_object() || !unsaved()) { if (after_save) navigate(std::exchange(after_save, nullptr)); return; }
    json b = fields;
    for (auto& [id, a] : dirty) b["lines"][std::to_string(id)] = a;
    int id = at(order, "id").get<int>();
    std::string ext = sstr(order, "external_id");
    request("save", "/api/orders/" + std::to_string(id), b.dump(), [this, b, id, ext](const HttpResp& r) {
      if (r.status / 100 != 2) { after_save = nullptr; return failed("Couldn't save the order", r, [this] { save(); }); }
      if (!order.is_object() || at(order, "id") != id) return ok("✓ Saved order " + ext);  // user moved on meanwhile
      for (auto& kv : b.items()) if (kv.key() != "lines" && fields.contains(kv.key()) && fields[kv.key()] == kv.value()) fields.erase(kv.key());
      if (b.contains("lines"))
        for (auto& kv : b["lines"].items()) {
          auto it = dirty.find(std::stoi(kv.key()));
          if (it != dirty.end() && kv.value() == it->second) dirty.erase(it);
        }
      json j = json::parse(r.body, nullptr, false);
      if (j.is_object() && j.contains("lines")) { order = std::move(j); inflight.erase("order"); }
      else { inflight.erase("order"); load_order(id); }
      ok("✓ Saved order " + ext);
      inflight.erase("home");
      load_home();
      if (after_save) navigate(std::exchange(after_save, nullptr));  // asks again if edits arrived mid-save
    });
  }
  void ask(const std::string& p, const std::string& init, std::function<void(const std::string&)> f, std::function<void(const std::string&)> live = nullptr) {
    editing = true; prompt = p; buf = init; on_commit = std::move(f); on_change = std::move(live);
  }
  void ask_due() { ask("due date YYYY-MM-DD (empty clears)", bstr("due_date"), [this](auto& v) { setdate("due_date", v); }); }
  void ask_paid() { ask("paid on YYYY-MM-DD (empty clears)", bstr("paid_on"), [this](auto& v) { setdate("paid_on", v); }); }
  void ask_linha() {
    ask("linha digitável", bstr("linha_digitavel"), [this](auto& v) {
      if (v.size() > 64 || v.find_first_not_of("0123456789. ") != std::string::npos) return ok("▲ linha: up to 64 digits, dots and spaces", th::yellow);
      fields["linha_digitavel"] = v.empty() ? json(nullptr) : json(v);
    });
  }

  // ---- catalogue -------------------------------------------------------------------------------
  json* item_by_id(int64_t id) {
    for (auto& it : items) if (at(it, "id").get<int64_t>() == id) return &it;
    return nullptr;
  }
  static std::optional<int> opt_pack(const json& it) {
    return at(it, "pack_size").is_number() ? std::optional<int>(at(it, "pack_size").get<int>()) : std::nullopt;
  }
  void open_editor(const json& it) {
    ed = Editor{at(it, "id").get<int64_t>(), opt_pack(it), it.value("admin_min_packs", 0), it.value("active", true), 0, false};
  }
  std::string ed_diff() const {
    const json* it = nullptr;
    for (auto& x : items) if (at(x, "id").get<int64_t>() == ed->id) it = &x;
    if (!it) return "";
    auto p = [](std::optional<int> v) { return v ? std::to_string(*v) : std::string("?"); };
    std::string d;
    if (opt_pack(*it) != ed->pack) d += "pack " + p(opt_pack(*it)) + " → " + p(ed->pack) + ", ";
    if (it->value("admin_min_packs", 0) != ed->min) d += "min " + std::to_string(it->value("admin_min_packs", 0)) + " → " + std::to_string(ed->min) + ", ";
    if (it->value("active", true) != ed->active) d += ed->active ? "restore, " : "archive, ";
    return d.empty() ? d : d.substr(0, d.size() - 2);
  }
  void ed_adjust(int delta) {
    if (ed->field == 0) ed->pack = std::max(1, ed->pack.value_or(delta > 0 ? 0 : 2) + delta);
    else ed->min = std::max(0, ed->min + delta);
    ed->typed = false;
  }
  void ed_digit(int d) {
    if (ed->field == 0) {
      int v = ed->typed && ed->pack ? std::min(*ed->pack * 10 + d, 9999) : d;
      ed->pack = v > 0 ? std::optional<int>(v) : std::nullopt;
    } else ed->min = ed->typed ? std::min(ed->min * 10 + d, 999) : d;
    ed->typed = true;
  }
  void ed_save() {
    json* it = item_by_id(ed->id);
    if (!it) return ed.reset();
    json b = json::object();
    if (opt_pack(*it) != ed->pack) b["pack_size"] = ed->pack ? json(*ed->pack) : json(nullptr);
    if (it->value("admin_min_packs", 0) != ed->min) b["admin_min_packs"] = ed->min;
    if (it->value("active", true) != ed->active) b["active"] = ed->active;
    if (b.empty()) return ed.reset();
    int64_t id = ed->id;
    std::string name = sstr(*it, "name"), diff = ed_diff();
    request("item", "/api/items/" + std::to_string(id), b.dump(), [this, id, b, name, diff](const HttpResp& r) {
      if (r.status / 100 != 2) return failed("Couldn't save " + name, r, [this, id] { if (ed && ed->id == id) ed_save(); });
      json j = json::parse(r.body, nullptr, false);
      if (json* x = item_by_id(id)) {
        if (j.is_object() && j.contains("id")) *x = std::move(j);
        else for (auto& kv : b.items()) (*x)[kv.key()] = kv.value();
      }
      if (ed && ed->id == id && ed_diff().empty()) ed.reset();
      ok("✓ Saved " + name + " · " + diff);
      inflight.erase("catalogue");
      inflight.erase("home");
      load_items();
      load_home();
    });
  }

  // ---- lists -----------------------------------------------------------------------------------
  bool match(int t, const std::string& s) const {
    if (filter[t].empty()) return true;
    auto low = [](std::string x) { for (auto& c : x) c = (char)std::tolower((unsigned char)c); return x; };
    return low(s).find(low(filter[t])) != std::string::npos;
  }
  std::vector<json> catalogue() const {
    std::vector<json> v;
    for (auto& it : items) {
      bool active = it.value("active", true);
      if (only_archived ? active : !active) continue;
      if (only_unknown && !at(it, "pack_size").is_null()) continue;
      if (match(2, sstr(it, "name") + " " + sstr(it, "supplier"))) v.push_back(it);
    }
    return v;
  }
  std::vector<json> new_items() const {
    std::vector<json> v;
    for (auto& r : reqs) if (match(3, sstr(r, "text"))) v.push_back(r);
    return v;
  }
  bool past(const json& c) const { return sstr(c, "start") != sstr(home.value("cycle", json::object()), "start"); }
  std::vector<std::pair<std::string, json>> past_orders() const {
    std::vector<std::pair<std::string, json>> v;
    for (auto& c : cycles)
      if (past(c))
        for (auto& o : at(c, "orders")) if (match(4, sstr(o, "supplier") + " " + sstr(o, "external_id"))) v.push_back({sstr(c, "start"), o});
    return v;
  }
  int list_size() const {
    switch (tab) {
      case 0: return (int)home.value("orders", json::array()).size();
      case 1: return order.is_object() && order.contains("lines") ? (int)at(order, "lines").size() : 0;
      case 2: return (int)catalogue().size();
      case 3: return (int)new_items().size();
      default: return (int)past_orders().size();
    }
  }
  void move(int d) {
    int n = list_size();
    cur[tab] = std::clamp(cur[tab] + d, 0, std::max(n - 1, 0));
  }
  void activate(int i) {  // Enter or click on row i of the current tab
    cur[tab] = i;
    if (tab == 0) {
      auto os = home.value("orders", json::array());
      if (i < (int)os.size()) open_order(at(os[i], "id").get<int>());
    } else if (tab == 2) {
      auto v = catalogue();
      if (ed && !ed_diff().empty()) return ok("● save (⏎) or cancel (Esc) the open item first", th::acc);
      if (i < (int)v.size()) open_editor(v[i]);
    } else if (tab == 4) {
      auto v = past_orders();
      if (i < (int)v.size()) open_order(at(v[i].second, "id").get<int>());
    }
  }

  // ---- input -----------------------------------------------------------------------------------
  bool mouse(Event& e) {
    auto& m = e.mouse();
    if (editing) return true;
    if (m.button == Mouse::WheelUp || m.button == Mouse::WheelDown) {
      if (!confirm && !help && !editing) move(m.button == Mouse::WheelUp ? -3 : 3);
      return true;
    }
    if (m.button != Mouse::Left || m.motion != Mouse::Pressed) return true;
    // innermost target wins (a checkbox inside a clickable row)
    std::optional<size_t> best;
    auto area = [&](size_t i) { return (long)(boxes[i].x_max - boxes[i].x_min + 1) * (boxes[i].y_max - boxes[i].y_min + 1); };
    for (size_t i = modal_from; i < acts.size(); i++)
      if (boxes[i].Contain(m.x, m.y) && (!best || area(i) < area(*best))) best = i;
    if (best) { auto f = acts[*best]; f(); }
    return true;
  }

  // false = quit
  bool key(Event e) {
    if (e == Event::Custom) return true;
    if (e.is_mouse()) return mouse(e);
    if (editing) {
      if (e == Event::Return) { editing = false; on_cancel = nullptr; on_commit(buf); }
      else if (e == Event::Escape) { editing = false; if (on_cancel) std::exchange(on_cancel, nullptr)(); }
      else if (e == Event::Backspace) { if (!buf.empty()) { do buf.pop_back(); while (!buf.empty() && ((unsigned char)buf.back() & 0xC0) == 0x80); } if (on_change) on_change(buf); }
      else if (e.is_character()) { buf += e.character(); if (on_change) on_change(buf); }
      return true;
    }
    if (confirm) {
      if (e == Event::Character('s')) { after_save = std::exchange(confirm, nullptr); save(); }
      else if (e == Event::Character('d')) { dirty.clear(); fields = json::object(); ok("changes discarded", th::dim); std::exchange(confirm, nullptr)(); }
      else if (e == Event::Escape) confirm = nullptr;
      return true;
    }
    if (help) { if (e == Event::Escape || e == Event::Character('?') || e == Event::Character('q')) help = false; return true; }
    if (err) {
      if (e == Event::Character('r')) { auto f = err->retry; err.reset(); f(); return true; }
      if (e == Event::Escape) { err.reset(); return true; }
    }
    if (ed) return editor_key(e);

    if (e == Event::ArrowDown || e == Event::Character('j')) move(1);
    else if (e == Event::ArrowUp || e == Event::Character('k')) move(-1);
    else if (e == Event::PageDown) move(10);
    else if (e == Event::PageUp) move(-10);
    else if (e == Event::Character('g') || e == Event::Home) move(-100000);
    else if (e == Event::Character('G') || e == Event::End) move(100000);
    else if (e == Event::Tab) goto_tab((tab + 1) % 5);
    else if (e == Event::TabReverse) goto_tab((tab + 4) % 5);
    else if (e.is_character() && e.character() >= "1" && e.character() <= "5" && e.character().size() == 1) goto_tab(e.character()[0] - '1');
    else if (e == Event::Character('?')) help = true;
    else if (e == Event::Character('r')) { load_tab(); if (tab != 0) load_home(); }
    else if (e == Event::Character('q') || e == Event::CtrlC) navigate(quit);
    else if (e == Event::Character('/') && tab != 1) {
      int t = tab;
      ask("filter", filter[t], [this, t](auto& v) { filter[t] = v; }, [this, t](auto& v) { filter[t] = v; cur[t] = 0; });
      on_cancel = [this, t, orig = filter[t]] { filter[t] = orig; };
    } else if (e == Event::Escape && !inflight.empty()) { inflight.clear(); after_save = nullptr; ok("cancelled", th::dim); }
    else if (e == Event::Escape && tab == 1) goto_tab(0);
    else if (e == Event::Escape && !filter[tab].empty()) { filter[tab].clear(); cur[tab] = 0; }
    else if (tab == 0 && e == Event::Character('h')) show_idle = !show_idle;
    else if (tab == 1) order_key(e);
    else if (tab == 2) {
      auto v = catalogue();
      if (e == Event::Character('u')) { only_unknown = !only_unknown; cur[2] = 0; }
      else if (e == Event::Character('x')) { only_archived = !only_archived; cur[2] = 0; }
      else if (e == Event::Return && cur[2] < (int)v.size()) open_editor(v[cur[2]]);
      else if (e == Event::Character('a') && cur[2] < (int)v.size()) { open_editor(v[cur[2]]); ed->active = !ed->active; ed_save(); }
    } else if (e == Event::Return) activate(cur[tab]);
    return true;
  }

  void order_key(const Event& e) {
    if (!order.is_object() || !order.contains("lines")) return;
    int c = cur[1];
    if (e == Event::Return) { mark(c, "arrived"); move(1); }
    else if (e == Event::Backspace || e == Event::Delete) { mark(c, "missing"); move(1); }
    else if (e == Event::Character('p')) mark(c, "pending");
    else if (e == Event::Character('A')) {
      for (int i = 0; i < (int)at(order, "lines").size(); i++) if (arrival(at(order, "lines")[i]) == "pending") mark(i, "arrived");
    } else if (e == Event::Character('s')) save();
    else if (e == Event::Character('d')) ask_due();
    else if (e == Event::Character('l')) ask_linha();
    else if (e == Event::Character('b')) ask_paid();
  }

  bool editor_key(const Event& e) {
    if (e == Event::Escape) { ed.reset(); ok("changes discarded", th::dim); }
    else if (e == Event::Return) ed_save();
    else if (e == Event::ArrowLeft || e == Event::Character('-')) ed_adjust(-1);
    else if (e == Event::ArrowRight || e == Event::Character('+')) ed_adjust(1);
    else if (e == Event::ArrowUp || e == Event::ArrowDown || e == Event::Character('m')) { ed->field = 1 - ed->field; ed->typed = false; }
    else if (e == Event::Character('a')) ed->active = !ed->active;
    else if (e == Event::Backspace) {
      if (ed->field == 0) ed->pack = ed->pack && *ed->pack >= 10 ? std::optional<int>(*ed->pack / 10) : std::nullopt;
      else ed->min /= 10;
      ed->typed = true;
    } else if (e.is_character() && e.character().size() == 1 && std::isdigit((unsigned char)e.character()[0])) ed_digit(e.character()[0] - '0');
    return true;
  }

  // ---- view ------------------------------------------------------------------------------------
  Element hit(Element el, std::function<void()> f) {
    boxes.emplace_back();
    acts.push_back(std::move(f));
    return el | reflect(boxes.back());
  }
  Element sel_row(Element el, bool on) { return on ? (el | bgcolor(th::sel) | focus) : el; }
  static Element section(const std::string& title, const std::string& sub, Element body) {
    return window(hbox({text(" " + title + " ") | bold, text(sub.empty() ? "" : "· " + sub + " ") | color(th::dim)}), body);
  }

  Element skeleton() {
    bool busy = inflight.count(tab_key()) > 0;
    char t[16];
    snprintf(t, sizeof t, "%.1fs", busy ? std::chrono::duration<double>(Clock::now() - inflight.at(tab_key()).t0).count() : 0.0);
    Elements rows = {busy ? text(spin() + " loading " + tab_key() + " · " + t) | color(th::blue)
                          : hbox({text("not loaded · ") | color(th::dim), keycap("r"), text(" retry") | color(th::dim)}),
                     text("")};
    for (int w : {46, 38, 42, 34, 40}) {
      std::string bars;
      for (int i = 0; i < w; i++) bars += "━";
      rows.push_back(text(bars) | color(th::line));
    }
    rows.push_back(text(""));
    rows.push_back(hbox({text("tabs and keys still work · ") | color(th::dim), keycap("Esc"), text(" cancel") | color(th::dim)}));
    return vbox(rows) | border;
  }
  std::string spin() const {
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count();
    return kSpin[(ms / 100) % 10];
  }

  Element boleto_status(const json& o) {
    const json& b = at(o, "boleto");
    std::string due = sstr(b, "due_date");
    if (!at(b, "paid_on").is_null()) return text("● paid " + sstr(b, "paid_on")) | color(th::green);
    if (b.value("overdue", false)) return text("▲ OVERDUE since " + due) | color(th::red);
    if (due.empty()) return text("▲ no due date · open and press d") | color(th::yellow);
    auto d = parse_date(due);
    int days = d ? (int)(*d - today_brt()).count() : 0;
    return text("○ open · due " + (days == 0 ? std::string("today") : "in " + std::to_string(days) + " days")) | color(th::dim);
  }

  Element home_view() {
    auto cy = home.value("cycle", json::object());
    Elements s = {trow({cell("", 1), cell("Item", 34), cell("Supplier", 8), cell("Demand", 6, true), cell("Pack", 4, true), cell("Order", 5, true), cell("Note", 10)}) | color(th::dim)};
    int hidden = 0;
    for (auto& g : home.value("suggestions", json::array())) {
      bool miss = g.value("missing_last_delivery", false), unk = g.value("unknown_pack", false);
      int sug = g.value("suggested", 0);
      if (sug == 0 && !miss && !show_idle) { hidden++; continue; }
      std::string note = unk ? "pack unknown · set it in Catalogue" : miss ? "missing from last delivery" :
                         sug > 0 && g.value("units", 0) == 0 ? "admin minimum" : "";
      Color nc = unk ? th::yellow : miss ? th::red : th::dim;
      s.push_back(trow({text(unk ? "▲" : miss ? "✗" : sug ? "●" : "·") | color(unk ? th::yellow : miss ? th::red : th::acc),
                        cell(sstr(g, "item"), 34), cell(sstr(g, "supplier"), 8) | color(th::dim),
                        cell(std::to_string(g.value("units", 0)), 6, true),
                        cell(at(g, "pack_size").is_number() ? std::to_string(at(g, "pack_size").get<int>()) : "?", 4, true) | color(unk ? th::yellow : Color::Default),
                        cell(std::to_string(sug), 5, true) | bold | color(th::acc), text(note) | color(nc)}));
    }
    if (hidden) s.push_back(hbox({text(" " + std::to_string(hidden) + " more with no demand · ") | color(th::dim), keycap("h"), text(" show") | color(th::dim)}));
    if (show_idle) s.push_back(hbox({text(" "), keycap("h"), text(" hide items with no demand") | color(th::dim)}));

    Elements o = {trow({cell("", 1), cell("Supplier", 8), cell("Order", 22), cell("Arrived", 16), cell("Amount", 12, true), cell("Due", 10), cell("Boleto", 10)}) | color(th::dim)};
    auto os = home.value("orders", json::array());
    for (int i = 0; i < (int)os.size(); i++) {
      auto& x = os[i];
      bool on = i == cur[0], pastc = sstr(x, "cycle_start") != sstr(cy, "start");
      int done = x.value("lines_done", 0), tot = x.value("lines_total", 0);
      auto r = trow({text(on ? "▸" : " ") | color(th::acc), cell(sstr(x, "supplier"), 8), cell(sstr(x, "external_id"), 22),
                     hbox({bar(done, tot, 10), text(fit(" " + std::to_string(done) + "/" + std::to_string(tot), 6))}),
                     cell(brl(x.value("total_cents", 0)), 12, true), cell(sstr(at(x, "boleto"), "due_date", "—"), 10),
                     hbox({boleto_status(x), text(pastc ? "  · cycle " + short_date(sstr(x, "cycle_start")) : "") | color(th::dim)})});
      o.push_back(hit(sel_row(r, on), [this, i] { activate(i); }));
    }
    if (os.empty()) o.push_back(hbox({text("No orders yet · import one with ") | color(th::dim), text("dionisio import FILE")}));
    return vbox({section("Suggestions", "what to order this cycle · demand = units asked over 10 workdays", vbox(s)),
                 section("Orders", "click or ⏎ to open the checklist", vbox(o) | vscroll_indicator | yframe | flex) | flex});
  }

  Element field_box(const std::string& label, const std::string& key, Element value, Color edge, std::function<void()> f) {
    return hit(vbox({hbox({text(label + " ") | color(th::dim), keycap(key)}), value}) | borderStyled(ROUNDED, edge) | flex, std::move(f));
  }

  Element order_view() {
    if (!order.is_object()) return vbox({text("No order open. Pick one on Home or Past cycles.") | color(th::dim)}) | border;
    const json& b = at(order, "boleto");
    auto val = [&](const char* k) -> std::optional<std::string> {
      if (fields.contains(k)) return fields[k].is_string() ? fields[k].get<std::string>() + " *" : std::string("(cleared) *");
      return b.contains(k) && b[k].is_string() ? std::optional<std::string>(b[k].get<std::string>()) : std::nullopt;
    };
    auto due = val("due_date"), linha = val("linha_digitavel"), paid = val("paid_on");
    bool overdue = b.value("overdue", false) && !fields.contains("due_date");
    Element due_v = due ? text(*due) | color(overdue ? th::red : Color::Default) : text("▲ not set · DM reminder daily") | color(th::yellow);
    Elements top = {
        field_box("Due date", "d", due_v, due ? (overdue ? th::red : th::line) : th::yellow, [this] { ask_due(); }),
        field_box("Linha digitável", "l", linha ? text(*linha) : text("—") | color(th::dim), th::line, [this] { ask_linha(); }),
        field_box("Paid on", "b", paid ? text("● " + *paid) | color(th::green) : text("— not paid") | color(th::dim), paid ? th::green : th::line, [this] { ask_paid(); })};

    int arrived = 0, missing = 0, n = (int)at(order, "lines").size();
    Elements rows = {trow({cell("", 1), cell("Qty", 4, true), cell("Item", 44), cell("Price", 11, true)}) | color(th::dim)};
    for (int i = 0; i < n; i++) {
      const json& l = at(order, "lines")[i];
      std::string a = arrival(l);
      arrived += a == "arrived"; missing += a == "missing";
      bool on = i == cur[1], changed = dirty.count(at(l, "id").get<int>()) > 0;
      Element g = a == "arrived" ? text("✓") | color(th::green) : a == "missing" ? text("✗") | color(th::red) : text("○") | color(th::dim);
      auto r = trow({hit(g | bold, [this, i] { cycle_line(i); }), cell(std::to_string(l.value("qty", 0)), 4, true),
                     cell((on ? "▸ " : "") + sstr(l, "name") + (changed ? " *" : ""), 44) | color(a == "missing" ? th::red : Color::Default),
                     cell(brl(l.value("price_cents", 0)), 11, true)});
      rows.push_back(hit(sel_row(r, on), [this, i] { cur[1] = i; }));
    }
    auto head = hbox({text(" " + sstr(order, "supplier") + " " + sstr(order, "external_id") + " ") | bold,
                      text("· delivery " + sstr(order, "delivery_date", "—") + " · " + std::to_string(n) + " lines") | color(th::dim), filler(),
                      bar(arrived + missing, n, 20), text("  checked " + std::to_string(arrived + missing) + "/" + std::to_string(n) + "   "),
                      text("✓ " + std::to_string(arrived)) | color(th::green), text("  "), text("✗ " + std::to_string(missing)) | color(th::red), text("  "),
                      text("○ " + std::to_string(n - arrived - missing)) | color(th::dim)});
    return vbox({head, hbox(top), window(text(" Checklist ") | bold, vbox(rows) | vscroll_indicator | yframe | flex) | flex});
  }

  Element catalogue_view() {
    auto v = catalogue();
    int unknown = 0, archived = 0, active = 0;
    for (auto& it : items) {
      if (!it.value("active", true)) archived++;
      else { active++; unknown += at(it, "pack_size").is_null(); }
    }
    auto chip = [&](const std::string& t, bool on, std::function<void()> f) {
      return hit(text(" " + t + " ") | (on ? bgcolor(Color::White) | color(th::ink) : bgcolor(th::line)), std::move(f));
    };
    auto bar_ = hbox({text(" / ") | color(th::dim), text(filter[2].empty() ? "filter items…" : filter[2]) | color(filter[2].empty() ? th::dim : Color::Default), filler(),
                      chip("All " + std::to_string(active), !only_unknown && !only_archived, [this] { only_unknown = only_archived = false; cur[2] = 0; }), text(" "),
                      chip("▲ Pack unknown " + std::to_string(unknown), only_unknown, [this] { only_unknown = !only_unknown; only_archived = false; cur[2] = 0; }), text(" "),
                      chip("Archived " + std::to_string(archived), only_archived, [this] { only_archived = !only_archived; only_unknown = false; cur[2] = 0; })});
    Elements rows = {trow({cell("", 1), cell("Item", 34), cell("Supplier", 8), cell("Pack", 4, true), cell("Min", 3, true), cell("SKU", 8)}) | color(th::dim)};
    for (int i = 0; i < (int)v.size(); i++) {
      const json& it = v[i];
      bool on = i == cur[2], unk = at(it, "pack_size").is_null(), act = it.value("active", true);
      auto r = trow({text(on ? "▸" : !act ? "◌" : unk ? "▲" : " ") | color(on ? th::acc : unk ? th::yellow : th::dim),
                     cell(sstr(it, "name"), 34) | color(act ? Color::Default : th::dim), cell(sstr(it, "supplier"), 8) | color(th::dim),
                     cell(unk ? "?" : std::to_string(at(it, "pack_size").get<int>()), 4, true) | bold | color(unk ? th::yellow : Color::Default),
                     cell(std::to_string(it.value("admin_min_packs", 0)), 3, true), cell(sstr(it, "sku", "—"), 8) | color(th::dim)});
      rows.push_back(hit(sel_row(r, on), [this, i] { activate(i); }));
    }
    if (v.empty()) rows.push_back(text(items.empty() ? "No items yet: they appear when you import an order." : "Nothing matches.") | color(th::dim));
    auto list = window(hbox({text(" Catalogue ") | bold, text("· click a row or ⏎ to edit ") | color(th::dim)}),
                       vbox({bar_, separatorLight(), vbox(rows) | vscroll_indicator | yframe | flex,
                             text(std::to_string(v.size()) + " shown · wheel scrolls") | color(th::dim)})) | flex;
    if (!ed) return list;
    return hbox({list, editor_view()});
  }

  Element stepper(const std::string& value, bool focused, std::function<void()> dec, std::function<void()> inc, std::function<void()> focus_me) {
    auto btn = [&](const std::string& t, std::function<void()> f) { return hit(text(" " + t + " ") | bgcolor(th::line) | bold, std::move(f)); };
    auto box = text(" " + fit(value, 5, true) + " ") | bold | (focused ? bgcolor(th::sel) | color(th::acc) : bgcolor(Color::Default));
    return hbox({btn("−", std::move(dec)), text(" "), hit(box, std::move(focus_me)), text(" "), btn("+", std::move(inc)),
                 text(focused ? "  ◂ editing" : "") | color(th::acc)});
  }

  Element editor_view() {
    const json* it = nullptr;
    for (auto& x : items) if (at(x, "id").get<int64_t>() == ed->id) it = &x;
    if (!it) return text("");
    auto detected = pack_size_from_name(sstr(*it, "name"));
    std::string diff = ed_diff();
    Elements presets;
    for (int p : {1, 4, 6, 12, 24}) {
      bool on = ed->pack == p;
      presets.push_back(hit(text(" " + std::to_string(p) + " ") | (on ? bgcolor(th::acc) | color(th::ink) : bgcolor(th::line)), [this, p] { if (!ed) return; ed->pack = p; ed->field = 0; ed->typed = false; }));
      presets.push_back(text(" "));
    }
    auto self = this;
    auto btn = [&](const std::string& t, bool primary, std::function<void()> f) {
      return hit(text("  " + t + "  ") | bold | (primary ? bgcolor(th::acc) | color(th::ink) : bgcolor(th::line)), std::move(f));
    };
    return window(text(" Edit item ") | bold | color(th::acc), vbox({
        paragraph(sstr(*it, "name")) | bold,
        text(sstr(*it, "supplier") + (sstr(*it, "sku").empty() ? "" : " · sku " + sstr(*it, "sku"))) | color(th::dim),
        text(""),
        hbox({text("Pack size ") | bold, text("· units per box") | color(th::dim)}),
        stepper(ed->pack ? std::to_string(*ed->pack) : "?", ed->field == 0, [self] { if (!self->ed) return; self->ed->field = 0; self->ed_adjust(-1); },
                [self] { if (!self->ed) return; self->ed->field = 0; self->ed_adjust(1); }, [self] { if (!self->ed) return; self->ed->field = 0; self->ed->typed = false; }),
        hbox(presets),
        detected ? text("✓ name says c/ " + std::to_string(*detected) + " un") | color(th::green) : text("no \"c/ N\" in the name: check the box") | color(th::dim),
        text(""),
        hbox({text("Minimum packs ") | bold, text("· always order") | color(th::dim)}),
        stepper(std::to_string(ed->min), ed->field == 1, [self] { if (!self->ed) return; self->ed->field = 1; self->ed_adjust(-1); },
                [self] { if (!self->ed) return; self->ed->field = 1; self->ed_adjust(1); }, [self] { if (!self->ed) return; self->ed->field = 1; self->ed->typed = false; }),
        text(""),
        hbox({btn(ed->active ? "◌ Archive  a" : "● Restore  a", false, [self] { if (!self->ed) return; self->ed->active = !self->ed->active; })}),
        filler(),
        text(diff.empty() ? "no changes" : "● unsaved: " + diff) | color(diff.empty() ? th::dim : th::acc),
        text(""),
        hbox({btn("Save ⏎", true, [self] { if (!self->ed) return; self->ed_save(); }), text(" "), btn("Cancel Esc", false, [self] { self->ed.reset(); })}),
    })) | size(WIDTH, EQUAL, 40);
  }

  Element new_items_view() {
    auto v = new_items();
    Elements rows = {trow({cell("", 1), cell("Asked by", 8, true), cell("Item (typed in Slack)", 50)}) | color(th::dim)};
    for (int i = 0; i < (int)v.size(); i++) {
      bool on = i == cur[3];
      rows.push_back(hit(sel_row(trow({text(on ? "▸" : " ") | color(th::acc), cell(std::to_string(v[i].value("requesters", 0)) + " ×", 8, true) | bold,
                                       cell(sstr(v[i], "text"), 50)}), on), [this, i] { cur[3] = i; }));
    }
    if (v.empty()) rows.push_back(text(reqs.empty() ? "Nobody asked for anything outside the catalogue." : "Nothing matches.") | color(th::dim));
    return window(hbox({text(" New items ") | bold, text("· requests for things not in the catalogue · they join it when an order with them is imported ") | color(th::dim)}),
                  vbox(rows) | vscroll_indicator | yframe | flex) | flex;
  }

  Element past_view() {
    auto v = past_orders();
    Elements rows = {trow({cell("", 1), cell("Cycle", 7), cell("Supplier", 8), cell("Order", 22), cell("Total", 12, true), cell("Due amount", 12, true), cell("Boleto", 10)}) | color(th::dim)};
    for (int i = 0; i < (int)v.size(); i++) {
      auto& [cs, o] = v[i];
      bool on = i == cur[4];
      auto r = trow({text(on ? "▸" : " ") | color(th::acc), cell(short_date(cs), 7), cell(sstr(o, "supplier"), 8), cell(sstr(o, "external_id"), 22),
                     cell(brl(o.value("total_cents", 0)), 12, true), cell(brl(at(o, "boleto").value("amount_due_cents", 0)), 12, true), boleto_status(o)});
      rows.push_back(hit(sel_row(r, on), [this, i] { activate(i); }));
    }
    if (v.empty()) rows.push_back(text("No past cycles yet.") | color(th::dim));
    return window(hbox({text(" Past cycles ") | bold, text("· click or ⏎ to open ") | color(th::dim)}), vbox(rows) | vscroll_indicator | yframe | flex) | flex;
  }

  Element header() {
    auto cy = home.value("cycle", json::object());
    Elements left = {text(" DIONISIO ") | bold | color(th::acc)};
    if (cy.contains("start")) {
      auto s = parse_date(sstr(cy, "start"));
      int day = s ? (int)(today_brt() - *s).count() + 1 : 0;
      left.push_back(text("  Cycle "));
      left.push_back(text(short_date(sstr(cy, "start")) + " → " + short_date(sstr(cy, "next_boundary"))) | bold);
      left.push_back(text("   "));
      left.push_back(bar(day, 14, 14));
      left.push_back(text("  day " + std::to_string(day) + " of 14") | color(th::dim));
    }
    Element right;
    if (!inflight.empty()) {
      double s = 0;
      std::string what;
      for (auto& [k, f] : inflight) { s = std::max(s, std::chrono::duration<double>(Clock::now() - f.t0).count()); what += (what.empty() ? "" : ", ") + k; }
      char t[16];
      snprintf(t, sizeof t, "%.1fs", s);
      right = text(spin() + " " + (inflight.count("save") || inflight.count("item") ? "saving" : "loading " + what) + " · " + t +
                   (s > 3 ? " · server waking up" : "") + " ") | color(th::blue);
    } else if (updated) {
      auto s = std::chrono::duration_cast<std::chrono::seconds>(Clock::now() - *updated).count();
      right = text("✓ updated " + (s < 60 ? std::to_string(s) + "s" : std::to_string(s / 60) + "m") + " ago ") | color(th::dim);
    } else right = text("");
    return hbox({hbox(left), filler(), right});
  }

  Element tabs() {
    Elements t;
    for (int i = 0; i < 5; i++) {
      std::string extra;
      Color ec = th::dim;
      if (i == 2 && loaded.count("catalogue")) { int a = 0; for (auto& it : items) a += it.value("active", true); extra = " " + std::to_string(a); }
      if (i == 3 && !reqs.empty()) { extra = " " + std::to_string(reqs.size()); ec = th::yellow; }
      Element e = hbox({text(" " + std::string(kTabs[i])), i == tab ? text(extra) : text(extra) | color(ec), text(" ")});
      e = i == tab ? e | bgcolor(th::acc) | color(th::ink) | bold : e;
      t.push_back(hit(e, [this, i] { goto_tab(i); }));
      t.push_back(text(" "));
    }
    return hbox(t);
  }

  Element footer() {
    using K = std::pair<std::string, std::string>;
    std::vector<K> ks;
    if (ed) ks = {{"← →", "−/+"}, {"0-9", "type"}, {"↑↓", "pack/min"}, {"a", "archive"}, {"⏎", "save"}, {"Esc", "cancel"}};
    else if (tab == 1) ks = {{"⏎", "arrived"}, {"⌫", "missing"}, {"p", "pending"}, {"A", "rest arrived"}, {"d l b", "boleto"}, {"s", "save"}, {"Esc", "back"}};
    else if (tab == 2) ks = {{"⏎", "edit"}, {"/", "filter"}, {"u", "unknown pack"}, {"x", "archived"}, {"a", "archive"}, {"?", "help"}};
    else ks = {{"1-5", "tabs"}, {"↑↓", "move"}, {"⏎", "open"}, {"/", "filter"}, {"r", "refresh"}, {"?", "help"}, {"q", "quit"}};
    Elements e;
    for (auto& [k, l] : ks) { e.push_back(keycap(k)); e.push_back(text(" " + l + "  ") | color(th::dim)); }
    e.push_back(filler());
    e.push_back(text("mouse: click · scroll ") | color(th::dim));
    if (!(tab == 1 && !ed && order.is_object() && order.contains("lines"))) return hbox(e);
    bool any = false;
    for (auto& l : at(order, "lines")) any |= arrival(l) != "pending";
    size_t nd = dirty.size() + fields.size();
    auto totals = hbox({text(" "), any ? text("Arrived total ") | color(th::dim) : text("Nothing checked yet · order total ") | color(th::dim),
                        text(brl(amount())) | bold | color(th::acc),
                        text(any ? " of " + brl(order.value("total_cents", 0)) + " ordered" : "") | color(th::dim), filler(),
                        nd ? text("● " + std::to_string(nd) + " unsaved · s saves ") | color(th::acc) : text("✓ all saved ") | color(th::green)});
    return vbox({totals, hbox(e)});
  }

  Element status_line() {
    if (editing) return hbox({text(" " + prompt + ": ") | color(th::acc), text(buf), text("▏") | blink, filler(), keycap("⏎"), text(" ok  ") | color(th::dim), keycap("Esc"), text(" cancel ") | color(th::dim)});
    return text(" " + status) | color(status_color);
  }

  Element err_banner() {
    auto self = this;
    return hbox({text(" ✗ " + err->title + " ") | bold | color(th::red), text("· " + err->detail) , filler(),
                 hit(text("  r Retry  ") | bold | bgcolor(Color::White) | color(th::ink), [self] { auto f = self->err->retry; self->err.reset(); f(); }), text(" "),
                 hit(text("  Esc Dismiss  ") | bgcolor(th::line), [self] { self->err.reset(); }), text(" ")}) | borderStyled(ROUNDED, th::red);
  }

  Element dialog() {
    auto self = this;
    size_t n = dirty.size() + fields.size();
    auto btn = [&](Element el, std::function<void()> f) { return hit(el, std::move(f)); };
    return vbox({text(" Leave this order? ") | bold, text(""),
                 text(" " + std::to_string(n) + " change" + (n == 1 ? "" : "s") + " not saved yet. "), text(""),
                 hbox({text(" "), btn(text("  s  Save  ") | bold | bgcolor(th::acc) | color(th::ink), [self] { self->after_save = std::exchange(self->confirm, nullptr); self->save(); }), text("  "),
                       btn(text("  d  Discard  ") | color(th::red) | bgcolor(th::line), [self] { self->dirty.clear(); self->fields = json::object(); std::exchange(self->confirm, nullptr)(); }), text("  "),
                       btn(text("  Esc  Stay  ") | bgcolor(th::line), [self] { self->confirm = nullptr; }), text(" ")})}) |
           borderStyled(ROUNDED, th::acc) | clear_under | center;
  }

  Element help_view() {
    auto group = [](const std::string& title, std::vector<std::pair<std::string, std::string>> ks) {
      Elements r = {text(title) | bold | color(th::acc)};
      for (auto& [k, w] : ks) r.push_back(hbox({text(fit(k, 11)), text(w) | color(th::dim)}));
      return vbox(r);
    };
    return vbox({hbox({text(" ? Help ") | bold, filler(), keycap("?"), text(" or ") | color(th::dim), keycap("Esc"), text(" closes ") | color(th::dim)}), text(""),
                 hbox({vbox({group("⌂ Everywhere", {{"1-5 Tab", "switch tab (loads in background)"}, {"↑↓ j k", "move · PgUp/PgDn · g/G"}, {"/", "filter the table"}, {"r", "refresh now"}, {"Esc", "cancel loading · clear filter"}, {"q", "quit (asks if unsaved)"}}),
                              text(""),
                              group("☑ Order", {{"⏎", "mark arrived"}, {"⌫ Del", "mark missing"}, {"p", "back to pending"}, {"A", "every pending line arrived"}, {"d l b", "due · linha · paid on"}, {"s", "save"}})}),
                       text("    "),
                       vbox({group("▤ Catalogue", {{"⏎", "open the item editor"}, {"← →", "pack size −1 / +1"}, {"0-9", "type a value"}, {"↑↓ m", "switch pack / minimum"}, {"a", "archive / restore"}, {"u x", "unknown pack · archived"}}),
                             text(""),
                             group("◎ Mouse", {{"click", "tabs, rows, buttons, ✓ ✗ ○"}, {"wheel", "scroll the list"}, {"Shift+drag", "select text to copy"}})})})}) |
           borderStyled(ROUNDED, th::acc) | clear_under | center;
  }

  Element view() {
    boxes.clear();
    acts.clear();
    modal_from = 0;
    Element body;
    if (tab != 1 && !loaded.count(tab_key())) body = skeleton();
    else if (tab == 0) body = home_view();
    else if (tab == 1) body = order.is_object() || !inflight.count("order") ? order_view() : skeleton();
    else if (tab == 2) body = catalogue_view();
    else if (tab == 3) body = new_items_view();
    else body = past_view();
    Elements page = {header(), tabs(), body | flex};
    if (err) page.push_back(err_banner());
    page.push_back(separatorLight());
    page.push_back(footer());
    page.push_back(status_line());
    Element ui = vbox(page);
    if (confirm || help) {
      modal_from = acts.size();
      ui = dbox({ui, confirm ? dialog() : help_view()});
    }
    return ui;
  }
};

int main(int argc, char** argv) {
  const char* u = getenv("DIONISIO_URL");
  const char* tk = getenv("DIONISIO_TOKEN");
  g_url = u ? u : "http://localhost:8080";
  while (!g_url.empty() && g_url.back() == '/') g_url.pop_back();
  g_token = tk ? tk : "";
  curl_global_init(CURL_GLOBAL_DEFAULT);  // before any request thread
  if (argc >= 3 && std::string(argv[1]) == "import") return do_import(argv[2]);
  if (argc > 1) { puts("usage: dionisio [import FILE]"); return 1; }

  auto screen = ScreenInteractive::Fullscreen();
  g_app = &screen;
  Tui app;
  app.quit = [&] { screen.Exit(); };
  screen.ForceHandleCtrlC(false);  // Ctrl-C asks about unsaved changes like q
  if (g_token.empty()) app.ok("▲ DIONISIO_TOKEN is not set; every request will be rejected", th::yellow);
  app.load_all();
  // Repaint while requests are in flight so the spinner and timers move.
  std::thread([] {
    for (;;) {
      std::this_thread::sleep_for(std::chrono::milliseconds(120));
      if (g_busy > 0) g_app->PostEvent(Event::Custom);
    }
  }).detach();

  auto ui = Renderer([&] {
    try {
      return app.view();
    } catch (const std::exception& x) {  // unexpected JSON shape: show, don't die
      app.boxes.clear();
      app.acts.clear();
      return text(std::string("render error: ") + x.what()) | color(Color::Red);
    }
  }) | CatchEvent([&](Event e) {
    try {
      app.key(e);
    } catch (const std::exception& x) {  // unexpected JSON shape: show, don't die
      app.ok(std::string("✗ internal: ") + x.what(), th::red);
    }
    return true;
  });
  screen.Loop(ui);
  std::fflush(stdout);
  std::_Exit(0);  // request threads may still be running; don't wait for or tear down under them
}
