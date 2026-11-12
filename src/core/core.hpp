// Pure domain logic shared by dionisio-server and the dionisio TUI (+ a tiny libcurl helper).
#pragma once
#include <chrono>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

using Date = std::chrono::sys_days;
inline constexpr Date kAnchor = std::chrono::sys_days{std::chrono::year{2026} / 11 / 9};

// Money: integer centavos everywhere; doubles only inside the export parser.
int64_t to_cents(double reais);  // llround(x*100); throws std::invalid_argument on negative / non-finite
std::string brl(int64_t cents);  // cents >= 0; 89516 -> "R$ 895.16"

// Dates. ponytail: fixed -03:00; tzdb if Brazil reinstates DST.
std::optional<Date> parse_date(std::string_view s);  // strict, valid YYYY-MM-DD only
std::string iso(Date d);                             // YYYY-MM-DD
Date today_brt();                                    // floor((unix - 10800) / 86400)
Date cycle_start(Date d, Date anchor = kAnchor);     // anchor + 14*floor((d-anchor)/14)
Date next_boundary_after(Date d, Date anchor = kAnchor);  // first boundary Monday > d
Date renew_to(Date expires_on, Date today, Date anchor = kAnchor);  // next_boundary_after(max(today, expires_on))

// Export parsing. Detect by items[0]: "sku" -> BEES, "totalPrice" -> JUNTOS, else throw.
struct ParsedLine {
  std::string name, sku;  // sku empty for JUNTOS
  int qty;
  int64_t price_cents;  // exported final line price (JUNTOS totalPrice, BEES subtotal)
};
struct ParsedOrder {
  std::string supplier, external_id, delivery_date;  // delivery_date empty if absent/invalid
  int64_t total_cents;                                // summary.total
  std::vector<ParsedLine> lines;
};
ParsedOrder parse_export(const std::string& raw);  // throws std::exception on bad JSON / unknown format
std::optional<int> pack_size_from_name(std::string_view name);  // case-insensitive "c/ N un"
std::string norm_name(std::string_view s);  // ASCII lowercase + collapse/trim whitespace

// Checklist. arrival is "pending" | "arrived" | "missing".
struct LineState {
  int64_t price_cents;
  std::string arrival;
};
int64_t arrived_total(const std::vector<LineState>& lines);
int64_t amount_due(int64_t total_cents, const std::vector<LineState>& lines);  // total while all pending

// Suggestions: units = sum(count*10) per-day + sum(count*2) per-week.
struct Pref {
  int count;
  std::string period;  // "day" | "week"
};
struct Suggestion {
  int units;
  std::optional<int> packs;  // nullopt when pack size unknown
  int suggested;             // max(admin_min, packs); raw units when pack size unknown
};
Suggestion suggest(const std::vector<Pref>& prefs, std::optional<int> pack_size, int admin_min_packs);

// Alert windows (a missed run is backfilled). kind matches notifications.kind.
struct Alert {
  std::string kind;
  Date key;  // notifications.on_date
};
std::optional<Alert> boleto_alert(Date due, Date today);         // boleto_d3|d1|d0 (key=due), boleto_overdue (key=today)
std::optional<Alert> renewal_alert(Date expires_on, Date today);  // renew72 {3,2}, renew24 {1,0}; key=expires_on

// pt-BR DM templates.
std::string boleto_dm(std::string_view supplier, int64_t amount_due, Date due, Date today, std::string_view linha);
std::string no_due_date_dm(std::string_view supplier, std::string_view external_id, int64_t amount_due);
std::string renewal_dm(std::string_view item, int count, std::string_view period, Date expires_on);

// libcurl helper (http.cpp). status 0 = transport error, body holds the curl message.
// Header keys are lowercased. Servers must call curl_global_init once before threads start.
struct HttpResp {
  long status = 0;
  std::string body;
  std::map<std::string, std::string> headers;
};
HttpResp http(const std::string& method, const std::string& url, const std::vector<std::string>& headers = {},
              const std::string& body = "", long timeout_ms = 1500);
