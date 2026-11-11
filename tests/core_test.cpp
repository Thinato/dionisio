#include "core.hpp"

#include <cmath>
#include <cstdio>
#include <fstream>
#include <limits>
#include <sstream>

static int fails = 0;
#define CHECK(c) \
  do { \
    if (!(c)) { ++fails; std::fprintf(stderr, "%s:%d CHECK(%s)\n", __FILE__, __LINE__, #c); } \
  } while (0)
#define THROWS(e) \
  do { \
    bool thrown = false; \
    try { (void)(e); } catch (...) { thrown = true; } \
    if (!thrown) { ++fails; std::fprintf(stderr, "%s:%d expected throw: %s\n", __FILE__, __LINE__, #e); } \
  } while (0)

static std::string slurp(const std::string& name) {
  std::ifstream f(std::string(FIXTURES) + "/" + name);
  std::stringstream s;
  s << f.rdbuf();
  return s.str();
}
static Date d(const char* s) { return parse_date(s).value(); }
static int64_t line_sum(const ParsedOrder& o) {
  int64_t t = 0;
  for (auto& l : o.lines) t += l.price_cents;
  return t;
}

int main() {
  auto juntos = parse_export(slurp("pedido-71205254322071e20101.json"));
  auto bees = parse_export(slurp("bees-pedido-9354967539.json"));

  // detection + parse
  CHECK(juntos.supplier == "JUNTOS");
  CHECK(bees.supplier == "BEES");
  CHECK(juntos.external_id == "71205254322071e20101");
  CHECK(bees.external_id == "9354967539");
  CHECK(juntos.delivery_date == "2026-11-10");
  CHECK(juntos.lines.size() == 21 && juntos.total_cents == 100772);
  CHECK(bees.lines.size() == 7 && bees.total_cents == 60241);
  CHECK(line_sum(juntos) == juntos.total_cents);
  CHECK(line_sum(bees) == bees.total_cents);
  for (auto& l : juntos.lines) {
    CHECK(l.sku.empty());
    if (l.name == "Sprite Zero Açúcar") CHECK(l.price_cents == 5419 && l.qty == 3);
  }
  THROWS(parse_export(R"({"orderId":"x","items":[{"name":"a","quantity":1}],"summary":{"total":1}})"));
  THROWS(parse_export(R"({"orderId":"x","items":[],"summary":{"total":1}})"));
  THROWS(parse_export("not json"));
  THROWS(parse_export(R"({"orderId":"x","items":[{"name":"a","quantity":0,"totalPrice":1}],"summary":{"total":1}})"));

  // pack sizes by sku
  std::map<std::string, int> want = {{"0019229", 6},  {"0019227", 4},  {"0036434", 4}, {"0009085", 12},
                                     {"0009274", 12}, {"0009093", 12}, {"0006905", 12}};
  CHECK(bees.lines.size() == want.size());
  for (auto& l : bees.lines) CHECK(pack_size_from_name(l.name) == want.at(l.sku));
  CHECK(!pack_size_from_name("Monster Energy"));
  CHECK(!pack_size_from_name("Caixa c/ 0 un"));

  CHECK(norm_name("  Monster   ENERGY ") == "monster energy");

  // checklist
  std::vector<LineState> js;
  for (auto& l : juntos.lines) js.push_back({l.price_cents, l.name == "Eisenbahn Unfiltered" ? "missing" : "arrived"});
  CHECK(arrived_total(js) == 89516);
  CHECK(amount_due(juntos.total_cents, js) == 89516);
  std::vector<LineState> bs;
  for (auto& l : bees.lines) bs.push_back({l.price_cents, "arrived"});
  CHECK(arrived_total(bs) == 60241);
  for (auto& l : bs) l.arrival = "pending";
  CHECK(arrived_total(bs) == 0);
  CHECK(amount_due(bees.total_cents, bs) == 60241);  // untouched checklist -> order total
  bs[0].arrival = "missing";
  CHECK(amount_due(bees.total_cents, bs) == 0);  // checking started, nothing arrived

  // money
  CHECK(brl(89516) == "R$ 895.16");
  CHECK(brl(5) == "R$ 0.05");
  CHECK(brl(100772) == "R$ 1007.72");
  CHECK(to_cents(54.19) == 5419);
  CHECK(to_cents(0) == 0);
  THROWS(to_cents(-0.01));
  THROWS(to_cents(std::numeric_limits<double>::quiet_NaN()));
  THROWS(to_cents(std::numeric_limits<double>::infinity()));

  // suggest
  auto s = suggest({{1, "day"}}, 6, 0);
  CHECK(s.units == 10 && s.packs == 2 && s.suggested == 2);
  CHECK(suggest({{1, "day"}}, 6, 3).suggested == 3);
  s = suggest({{2, "week"}, {1, "day"}}, 12, 0);
  CHECK(s.units == 14 && s.suggested == 2);
  s = suggest({{1, "day"}, {1, "day"}}, 6, 0);
  CHECK(s.units == 20 && s.suggested == 4);
  s = suggest({{1, "day"}}, std::nullopt, 0);
  CHECK(s.units == 10 && !s.packs && s.suggested == 10);
  CHECK(suggest({}, 6, 1).suggested == 1);

  // dates + cycles
  CHECK(!parse_date("2026-02-30") && !parse_date("2026-1-01") && !parse_date("x") && !parse_date("2026-11-090"));
  CHECK(iso(d("2026-11-09")) == "2026-11-09");
  CHECK(cycle_start(d("2026-11-22")) == d("2026-11-09"));
  CHECK(cycle_start(d("2026-11-23")) == d("2026-11-23"));
  CHECK(cycle_start(d("2026-11-09")) == d("2026-11-09"));
  CHECK(next_boundary_after(d("2026-11-09")) == d("2026-11-23"));
  CHECK(next_boundary_after(d("2026-11-10")) == d("2026-11-23"));
  CHECK(cycle_start(d("2026-11-08")) == d("2026-10-26"));
  CHECK(cycle_start(d("2026-10-26")) == d("2026-10-26"));
  CHECK(cycle_start(d("2026-10-25")) == d("2026-10-12"));
  CHECK(is_active(d("2026-11-23"), d("2026-11-23")));
  CHECK(!is_active(d("2026-11-23"), d("2026-11-24")));
  CHECK(renew_to(d("2026-11-23"), d("2026-11-20")) == d("2026-12-07"));
  CHECK(renew_to(d("2026-11-23"), d("2026-11-23")) == d("2026-12-07"));
  CHECK(renew_to(d("2026-11-09"), d("2026-11-25")) == d("2026-12-07"));

  // boleto alerts
  Date due = d("2026-11-20");
  auto at = [&](int off) { return boleto_alert(due, due - std::chrono::days(off)); };
  CHECK(!at(4));
  CHECK(at(3)->kind == "boleto_d3" && at(3)->key == due);
  CHECK(at(2)->kind == "boleto_d3" && at(2)->key == due);
  CHECK(at(1)->kind == "boleto_d1" && at(1)->key == due);
  CHECK(at(0)->kind == "boleto_d0" && at(0)->key == due);
  CHECK(at(-1)->kind == "boleto_overdue" && at(-1)->key == due + std::chrono::days(1));
  CHECK(at(-5)->kind == "boleto_overdue" && at(-5)->key == due + std::chrono::days(5));

  // renewal alerts
  Date exp = d("2026-11-23");
  auto rn = [&](int off) { return renewal_alert(exp, exp - std::chrono::days(off)); };
  CHECK(!rn(4));
  CHECK(rn(3)->kind == "renew72" && rn(3)->key == exp);
  CHECK(rn(2)->kind == "renew72");
  CHECK(rn(1)->kind == "renew24");
  CHECK(rn(0)->kind == "renew24" && rn(0)->key == exp);
  CHECK(!rn(-1));

  // DM templates
  CHECK(boleto_dm("BEES", 60241, due, due - std::chrono::days(3), "") ==
        "Boleto BEES · R$ 602.41 · vence 2026-11-20 (em 3 dias) · Linha digitável: não informada");
  CHECK(boleto_dm("BEES", 60241, due, due - std::chrono::days(1), "1234 5678") ==
        "Boleto BEES · R$ 602.41 · vence 2026-11-20 (amanhã) · Linha digitável: 1234 5678");
  CHECK(boleto_dm("JUNTOS", 89516, due, due, "") ==
        "Boleto JUNTOS · R$ 895.16 · vence 2026-11-20 (hoje) · Linha digitável: não informada");
  CHECK(boleto_dm("JUNTOS", 89516, due, due + std::chrono::days(1), "") ==
        "Boleto JUNTOS · R$ 895.16 · vence 2026-11-20 (vencido há 1 dia) · Linha digitável: não informada");
  CHECK(boleto_dm("JUNTOS", 89516, due, due + std::chrono::days(4), "") ==
        "Boleto JUNTOS · R$ 895.16 · vence 2026-11-20 (vencido há 4 dias) · Linha digitável: não informada");
  CHECK(no_due_date_dm("BEES", "9354967539", 60241) ==
        "Pedido BEES 9354967539 (R$ 602.41) sem data de vencimento. Defina no TUI.");
  CHECK(renewal_dm("Monster Energy", 2, "day", exp) ==
        "Sua preferência Monster Energy 2x/dia expira em 2026-11-23. Renovar para o próximo ciclo?");
  CHECK(renewal_dm("Toddynho", 1, "week", exp) ==
        "Sua preferência Toddynho 1x/semana expira em 2026-11-23. Renovar para o próximo ciclo?");

  if (fails) std::fprintf(stderr, "%d check(s) failed\n", fails);
  else std::puts("core_test: all checks passed");
  return fails ? 1 : 0;
}
