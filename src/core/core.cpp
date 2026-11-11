#include "core.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <regex>
#include <stdexcept>

#include <nlohmann/json.hpp>

using namespace std::chrono;

int64_t to_cents(double reais) {
  if (!std::isfinite(reais) || reais < 0 || reais > 1e9) throw std::invalid_argument("invalid money value");
  return std::llround(reais * 100);
}

std::string brl(int64_t cents) {
  if (cents < 0) return "-" + brl(-cents);
  char buf[32];
  std::snprintf(buf, sizeof buf, "R$ %lld.%02lld", (long long)(cents / 100), (long long)(cents % 100));
  return buf;
}

std::optional<Date> parse_date(std::string_view s) {
  if (s.size() != 10 || s[4] != '-' || s[7] != '-') return std::nullopt;
  for (int i : {0, 1, 2, 3, 5, 6, 8, 9})
    if (!std::isdigit((unsigned char)s[i])) return std::nullopt;
  auto num = [&](int pos, int len) { return std::stoi(std::string(s.substr(pos, len))); };
  year_month_day ymd{year{num(0, 4)}, month{(unsigned)num(5, 2)}, day{(unsigned)num(8, 2)}};
  if (!ymd.ok()) return std::nullopt;
  return sys_days{ymd};
}

std::string iso(Date d) {
  year_month_day ymd{d};
  char buf[16];
  std::snprintf(buf, sizeof buf, "%04d-%02u-%02u", (int)ymd.year(), (unsigned)ymd.month(), (unsigned)ymd.day());
  return buf;
}

Date today_brt() { return floor<days>(system_clock::now() - hours(3)); }

Date cycle_start(Date d, Date anchor) {
  auto n = (d - anchor).count();
  auto k = n >= 0 ? n / 14 : -((-n + 13) / 14);  // floor division
  return anchor + days(14 * k);
}

Date next_boundary_after(Date d, Date anchor) { return cycle_start(d, anchor) + days(14); }

bool is_active(Date expires_on, Date today) { return expires_on >= today; }

Date renew_to(Date expires_on, Date today, Date anchor) {
  return next_boundary_after(std::max(today, expires_on), anchor);
}

// orderId / sku: a non-empty string or an integer, nothing else.
static std::string scalar_id(const nlohmann::json& v, const char* what) {
  if (v.is_string() && !v.get_ref<const std::string&>().empty()) return v.get<std::string>();
  if (v.is_number_integer()) return v.dump();
  throw std::invalid_argument(std::string("invalid ") + what);
}

ParsedOrder parse_export(const std::string& raw) {
  auto j = nlohmann::json::parse(raw);
  const auto& items = j.at("items");
  if (!items.is_array() || items.empty() || !items[0].is_object()) throw std::invalid_argument("unknown export format");
  bool bees = items[0].contains("sku");
  if (!bees && !items[0].contains("totalPrice")) throw std::invalid_argument("unknown export format");

  ParsedOrder o;
  o.supplier = bees ? "BEES" : "JUNTOS";
  o.external_id = scalar_id(j.at("orderId"), "orderId");
  if (auto dd = j.find("deliveryDate"); dd != j.end() && dd->is_string() && parse_date(dd->get_ref<const std::string&>()))
    o.delivery_date = dd->get<std::string>();
  o.total_cents = to_cents(j.at("summary").at("total").get<double>());
  for (const auto& it : items) {
    ParsedLine l;
    l.name = it.at("name").get<std::string>();
    if (bees) l.sku = scalar_id(it.at("sku"), "sku");
    const auto& qty = it.at("quantity");
    if (!qty.is_number_integer() || qty.get<int64_t>() < 1 || qty.get<int64_t>() > 100000)
      throw std::invalid_argument("quantity must be an integer in 1..100000");
    l.qty = qty.get<int>();
    l.price_cents = to_cents(it.at(bees ? "subtotal" : "totalPrice").get<double>());
    if (norm_name(l.name).empty()) throw std::invalid_argument("invalid line");
    o.lines.push_back(std::move(l));
  }
  return o;
}

std::optional<int> pack_size_from_name(std::string_view name) {
  static const std::regex re(R"(c/\s*(\d{1,4})\s*un)", std::regex::icase);
  std::match_results<std::string_view::const_iterator> m;
  if (!std::regex_search(name.begin(), name.end(), m, re)) return std::nullopt;
  int n = std::stoi(m[1].str());
  return n > 0 ? std::optional<int>(n) : std::nullopt;
}

std::string norm_name(std::string_view s) {
  std::string out;
  for (char c : s) {
    if (std::isspace((unsigned char)c)) {
      if (!out.empty() && out.back() != ' ') out += ' ';
    } else {
      out += (char)std::tolower((unsigned char)c);
    }
  }
  if (!out.empty() && out.back() == ' ') out.pop_back();
  return out;
}

// No overflow: to_cents caps a line at 1e11 centavos and the 1 MiB body caps the line count.
int64_t arrived_total(const std::vector<LineState>& lines) {
  int64_t t = 0;
  for (const auto& l : lines)
    if (l.arrival == "arrived") t += l.price_cents;
  return t;
}

int64_t amount_due(int64_t total_cents, const std::vector<LineState>& lines) {
  bool started = std::any_of(lines.begin(), lines.end(), [](const LineState& l) { return l.arrival != "pending"; });
  return started ? arrived_total(lines) : total_cents;
}

Suggestion suggest(const std::vector<Pref>& prefs, std::optional<int> pack_size, int admin_min_packs) {
  int units = 0;
  for (const auto& p : prefs) units += p.count * (p.period == "day" ? 10 : 2);
  if (!pack_size || *pack_size <= 0) return {units, std::nullopt, units};
  int packs = (units + *pack_size - 1) / *pack_size;
  return {units, packs, std::max(admin_min_packs, packs)};
}

std::optional<Alert> boleto_alert(Date due, Date today) {
  auto n = (due - today).count();
  if (n > 3) return std::nullopt;
  if (n >= 2) return Alert{"boleto_d3", due};
  if (n == 1) return Alert{"boleto_d1", due};
  if (n == 0) return Alert{"boleto_d0", due};
  return Alert{"boleto_overdue", today};
}

std::optional<Alert> renewal_alert(Date expires_on, Date today) {
  auto n = (expires_on - today).count();
  if (n == 2 || n == 3) return Alert{"renew72", expires_on};
  if (n == 0 || n == 1) return Alert{"renew24", expires_on};
  return std::nullopt;
}

std::string boleto_dm(std::string_view supplier, int64_t amount_due, Date due, Date today, std::string_view linha) {
  auto n = (due - today).count();
  std::string when = n > 1    ? "em " + std::to_string(n) + " dias"
                     : n == 1 ? "amanhã"
                     : n == 0 ? "hoje"
                     : n == -1 ? "vencido há 1 dia"
                               : "vencido há " + std::to_string(-n) + " dias";
  return "Boleto " + std::string(supplier) + " · " + brl(amount_due) + " · vence " + iso(due) + " (" + when +
         ") · Linha digitável: " + (linha.empty() ? std::string("não informada") : std::string(linha));
}

std::string no_due_date_dm(std::string_view supplier, std::string_view external_id, int64_t amount_due) {
  return "Pedido " + std::string(supplier) + " " + std::string(external_id) + " (" + brl(amount_due) +
         ") sem data de vencimento. Defina no TUI.";
}

std::string renewal_dm(std::string_view item, int count, std::string_view period, Date expires_on) {
  return "Sua preferência " + std::string(item) + " " + std::to_string(count) + "x/" +
         (period == "day" ? "dia" : "semana") + " expira em " + iso(expires_on) + ". Renovar para o próximo ciclo?";
}
