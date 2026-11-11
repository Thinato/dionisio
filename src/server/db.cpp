#include <map>
#include <memory>
#include <set>
#include <stdexcept>

#include "server.hpp"

sqlite3* g_db = nullptr;
std::string g_db_path;
std::mutex g_mu;
int g_saved = 0;
unsigned g_reloads = 0;

// ponytail: no migrations; add PRAGMA user_version steps on first schema change.
static const char* kSchema = R"SQL(
PRAGMA foreign_keys = ON;
CREATE TABLE IF NOT EXISTS items (
  id INTEGER PRIMARY KEY,
  supplier TEXT NOT NULL CHECK (supplier IN ('JUNTOS','BEES')),
  name TEXT NOT NULL, norm_name TEXT NOT NULL,
  sku TEXT,
  pack_size INTEGER CHECK (pack_size > 0),
  admin_min_packs INTEGER NOT NULL DEFAULT 0 CHECK (admin_min_packs >= 0),
  active INTEGER NOT NULL DEFAULT 1 CHECK (active IN (0,1)),
  UNIQUE (supplier, norm_name));
CREATE UNIQUE INDEX IF NOT EXISTS items_sku ON items(supplier, sku) WHERE sku IS NOT NULL;
CREATE TABLE IF NOT EXISTS orders (
  id INTEGER PRIMARY KEY,
  supplier TEXT NOT NULL CHECK (supplier IN ('JUNTOS','BEES')),
  external_id TEXT NOT NULL,
  cycle_start TEXT NOT NULL,
  delivery_date TEXT,
  total_cents INTEGER NOT NULL CHECK (total_cents >= 0),
  imported_at TEXT NOT NULL,
  due_date TEXT, linha_digitavel TEXT, paid_on TEXT,
  UNIQUE (supplier, external_id));
CREATE TABLE IF NOT EXISTS order_lines (
  id INTEGER PRIMARY KEY,
  order_id INTEGER NOT NULL REFERENCES orders(id),
  item_id INTEGER NOT NULL REFERENCES items(id),
  qty INTEGER NOT NULL CHECK (qty > 0),
  price_cents INTEGER NOT NULL CHECK (price_cents >= 0),
  arrival TEXT NOT NULL DEFAULT 'pending' CHECK (arrival IN ('pending','arrived','missing')));
CREATE TABLE IF NOT EXISTS preferences (
  id INTEGER PRIMARY KEY,
  slack_user_id TEXT NOT NULL,
  item_id INTEGER REFERENCES items(id),
  other_text TEXT, other_norm TEXT,
  count INTEGER NOT NULL CHECK (count BETWEEN 1 AND 5),
  period TEXT NOT NULL CHECK (period IN ('day','week')),
  expires_on TEXT NOT NULL,
  CHECK ((item_id IS NULL) <> (other_text IS NULL)));
CREATE UNIQUE INDEX IF NOT EXISTS pref_item  ON preferences(slack_user_id, item_id)    WHERE item_id IS NOT NULL;
CREATE UNIQUE INDEX IF NOT EXISTS pref_other ON preferences(slack_user_id, other_norm) WHERE other_norm IS NOT NULL;
CREATE TABLE IF NOT EXISTS notifications (
  kind TEXT NOT NULL,
  ref_id INTEGER NOT NULL,
  on_date TEXT NOT NULL,
  target TEXT NOT NULL,
  PRIMARY KEY (kind, ref_id, on_date, target));
)SQL";

void open_db() {
  if (sqlite3_open(g_db_path.c_str(), &g_db) != SQLITE_OK)
    throw std::runtime_error("open " + g_db_path + ": " + sqlite3_errmsg(g_db));
  char* err = nullptr;
  if (sqlite3_exec(g_db, kSchema, nullptr, nullptr, &err) != SQLITE_OK) {
    std::string msg = err ? err : "schema failed";
    sqlite3_free(err);
    throw std::runtime_error(msg);
  }
}

json q(const std::string& sql, const json& params) {
  sqlite3_stmt* raw = nullptr;
  if (sqlite3_prepare_v2(g_db, sql.c_str(), -1, &raw, nullptr) != SQLITE_OK)
    throw std::runtime_error(sqlite3_errmsg(g_db));
  std::unique_ptr<sqlite3_stmt, int (*)(sqlite3_stmt*)> st(raw, sqlite3_finalize);
  for (int i = 0; i < (int)params.size(); ++i) {
    const json& p = params[i];
    if (p.is_null()) sqlite3_bind_null(raw, i + 1);
    else if (p.is_boolean()) sqlite3_bind_int64(raw, i + 1, p.get<bool>());
    else if (p.is_number_integer()) sqlite3_bind_int64(raw, i + 1, p.get<int64_t>());
    else if (p.is_string()) sqlite3_bind_text(raw, i + 1, p.get_ref<const std::string&>().c_str(), -1, SQLITE_TRANSIENT);
    else throw std::invalid_argument("unsupported sql param");
  }
  json rows = json::array();
  int rc;
  while ((rc = sqlite3_step(raw)) == SQLITE_ROW) {
    json row = json::object();
    for (int c = 0; c < sqlite3_column_count(raw); ++c) {
      const char* name = sqlite3_column_name(raw, c);
      switch (sqlite3_column_type(raw, c)) {
        case SQLITE_INTEGER: row[name] = (int64_t)sqlite3_column_int64(raw, c); break;
        case SQLITE_NULL: row[name] = nullptr; break;
        default: row[name] = std::string((const char*)sqlite3_column_text(raw, c));
      }
    }
    rows.push_back(std::move(row));
  }
  if (rc != SQLITE_DONE) throw std::runtime_error(sqlite3_errmsg(g_db));
  return rows;
}

json order_json(int64_t id, Date today, bool with_lines) {
  auto rows = q("SELECT * FROM orders WHERE id = ?", {id});
  if (rows.empty()) return nullptr;
  json o = rows[0];
  auto lines = q(
      "SELECT l.id, l.item_id, i.name, l.qty, l.price_cents, l.arrival FROM order_lines l "
      "JOIN items i ON i.id = l.item_id WHERE l.order_id = ? ORDER BY l.id",
      {id});
  std::vector<LineState> st;
  int done = 0;
  for (auto& l : lines) {
    st.push_back({l["price_cents"].get<int64_t>(), l["arrival"].get<std::string>()});
    done += st.back().arrival != "pending";
  }
  auto due = o["due_date"].is_string() ? parse_date(o["due_date"].get<std::string>()) : std::nullopt;
  json r = {{"id", id},
            {"supplier", o["supplier"]},
            {"external_id", o["external_id"]},
            {"cycle_start", o["cycle_start"]},
            {"delivery_date", o["delivery_date"]},
            {"imported_at", o["imported_at"]},
            {"total_cents", o["total_cents"]},
            {"lines_done", done},
            {"lines_total", (int)lines.size()},
            {"boleto",
             {{"amount_due_cents", amount_due(o["total_cents"].get<int64_t>(), st)},
              {"due_date", o["due_date"]},
              {"linha_digitavel", o["linha_digitavel"]},
              {"paid_on", o["paid_on"]},
              {"overdue", o["paid_on"].is_null() && due && *due < today}}}};
  if (with_lines) r["lines"] = lines;
  return r;
}

json home_json(Date today) {
  Date cs = cycle_start(today, g_anchor);
  std::map<int64_t, std::vector<Pref>> demand;
  for (auto& p : q("SELECT item_id, count, period FROM preferences WHERE item_id IS NOT NULL AND expires_on >= ?",
                   {iso(today)}))
    demand[p["item_id"].get<int64_t>()].push_back({p["count"].get<int>(), p["period"].get<std::string>()});
  // Per supplier: the most recent prior-cycle order whose checklist was started; its missing lines get flagged.
  std::set<int64_t> missing;
  for (auto& r : q(R"(
      SELECT DISTINCT l.item_id FROM order_lines l WHERE l.arrival = 'missing' AND l.order_id IN (
        SELECT (SELECT o.id FROM orders o WHERE o.supplier = s.supplier AND o.cycle_start < ?
                  AND EXISTS (SELECT 1 FROM order_lines x WHERE x.order_id = o.id AND x.arrival <> 'pending')
                ORDER BY o.cycle_start DESC, o.id DESC LIMIT 1)
        FROM (SELECT 'JUNTOS' AS supplier UNION ALL SELECT 'BEES') s))",
                   {iso(cs)}))
    missing.insert(r["item_id"].get<int64_t>());

  json sugg = json::array();
  for (auto& it : q("SELECT id, name, supplier, pack_size, admin_min_packs FROM items WHERE active = 1 "
                    "ORDER BY supplier, name")) {
    int64_t id = it["id"];
    std::optional<int> pack;
    if (!it["pack_size"].is_null()) pack = it["pack_size"].get<int>();
    auto s = suggest(demand[id], pack, it["admin_min_packs"].get<int>());
    sugg.push_back({{"item_id", id},
                    {"item", it["name"]},
                    {"supplier", it["supplier"]},
                    {"units", s.units},
                    {"pack_size", it["pack_size"]},
                    {"packs", s.packs ? json(*s.packs) : json(nullptr)},
                    {"admin_min", it["admin_min_packs"]},
                    {"suggested", s.suggested},
                    {"unknown_pack", !pack},
                    {"missing_last_delivery", missing.count(id) > 0}});
  }
  json orders = json::array();
  for (auto& r : q("SELECT id FROM orders WHERE cycle_start = ? OR paid_on IS NULL ORDER BY cycle_start, id",
                   {iso(cs)}))
    orders.push_back(order_json(r["id"], today, false));
  return {{"cycle", {{"start", iso(cs)}, {"next_boundary", iso(next_boundary_after(today, g_anchor))}}},
          {"suggestions", sugg},
          {"orders", orders}};
}

json items_json() {
  auto rows = q("SELECT id, supplier, name, sku, pack_size, admin_min_packs, active FROM items ORDER BY supplier, name");
  for (auto& r : rows) r["active"] = r["active"] == 1;
  return rows;
}

json requests_json(Date today) {
  return q(
      "SELECT MIN(other_text) AS text, COUNT(DISTINCT slack_user_id) AS requesters FROM preferences "
      "WHERE other_norm IS NOT NULL AND expires_on >= ? GROUP BY other_norm ORDER BY requesters DESC, text",
      {iso(today)});
}

json cycles_json(Date today) {
  json out = json::array();
  for (auto& c : q("SELECT DISTINCT cycle_start FROM orders ORDER BY cycle_start DESC")) {
    json orders = json::array();
    for (auto& r : q("SELECT id FROM orders WHERE cycle_start = ? ORDER BY id", json::array({c["cycle_start"]})))
      orders.push_back(order_json(r["id"], today, false));
    out.push_back({{"start", c["cycle_start"]}, {"orders", orders}});
  }
  return out;
}

// BEES: match by (supplier, sku), then (supplier, norm_name). A name already taken by another sku gets " [sku]".
static int64_t find_or_create_item(const std::string& supplier, const ParsedLine& l) {
  json sku = l.sku.empty() ? json(nullptr) : json(l.sku);
  if (!l.sku.empty()) {
    auto r = q("SELECT id FROM items WHERE supplier = ? AND sku = ?", {supplier, l.sku});
    if (!r.empty()) return r[0]["id"];
  }
  std::string name = l.name, nn = norm_name(l.name);
  auto r = q("SELECT id, sku FROM items WHERE supplier = ? AND norm_name = ?", {supplier, nn});
  if (!r.empty()) {
    if (l.sku.empty()) return r[0]["id"];
    if (r[0]["sku"].is_null()) {
      q("UPDATE items SET sku = ? WHERE id = ?", {l.sku, r[0]["id"]});
      return r[0]["id"];
    }
    name += " [" + l.sku + "]";
    nn = norm_name(name);
  }
  auto pack = l.sku.empty() ? std::nullopt : pack_size_from_name(l.name);
  q("INSERT INTO items (supplier, name, norm_name, sku, pack_size) VALUES (?, ?, ?, ?, ?)",
    {supplier, name, nn, sku, pack ? json(*pack) : json(nullptr)});
  return sqlite3_last_insert_rowid(g_db);
}

std::pair<int, json> import_order(const ParsedOrder& o, Date today) {
  if (!q("SELECT 1 FROM orders WHERE supplier = ? AND external_id = ?", {o.supplier, o.external_id}).empty())
    return {409, {{"error", "already imported"}, {"supplier", o.supplier}, {"external_id", o.external_id}}};
  int64_t id = 0;
  tx([&] {
    q("INSERT INTO orders (supplier, external_id, cycle_start, delivery_date, total_cents, imported_at) "
      "VALUES (?, ?, ?, ?, ?, ?)",
      {o.supplier, o.external_id, iso(cycle_start(today, g_anchor)),
       o.delivery_date.empty() ? json(nullptr) : json(o.delivery_date), o.total_cents, iso(today)});
    id = sqlite3_last_insert_rowid(g_db);
    for (const auto& l : o.lines)
      q("INSERT INTO order_lines (order_id, item_id, qty, price_cents) VALUES (?, ?, ?, ?)",
        {id, find_or_create_item(o.supplier, l), l.qty, l.price_cents});
  });
  return {201, {{"id", id}, {"supplier", o.supplier}, {"external_id", o.external_id}, {"lines", o.lines.size()}}};
}
