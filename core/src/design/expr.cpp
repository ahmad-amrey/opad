#include "opad/design/expr.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstdio>

namespace opad::design {

namespace {

constexpr double kPi = 3.14159265358979323846;

struct Unit {
  const char* name;
  double factor;  // to mm or radians
  bool angle;
};
const Unit kUnits[] = {{"mm", 1.0, false},  {"cm", 10.0, false},          {"m", 1000.0, false}, {"um", 0.001, false}, {"in", 25.4, false},
                       {"ft", 304.8, false}, {"deg", kPi / 180.0, true}, {"rad", 1.0, true}};
const char* const kFunctions[] = {"sin", "cos", "tan", "asin", "acos", "atan", "atan2", "sqrt", "abs", "min", "max",
                                  "floor", "ceil", "round", "pow", "exp", "ln", "log", "hypot", "sign"};
const char* const kConstants[] = {"PI", "pi", "E"};

const Unit* unit_named(const std::string& s) {
  for (const Unit& u : kUnits)
    if (s == u.name) return &u;
  return nullptr;
}
bool is_function(const std::string& s) { return std::any_of(std::begin(kFunctions), std::end(kFunctions), [&](const char* f) { return s == f; }); }
bool is_constant(const std::string& s) { return std::any_of(std::begin(kConstants), std::end(kConstants), [&](const char* f) { return s == f; }); }
bool ident_start(char c) { return std::isalpha(static_cast<unsigned char>(c)) || c == '_'; }
bool ident_char(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; }

struct Token {
  enum Kind { End, Number, Ident, Sym } kind = End;
  double number = 0;
  std::string text;
  size_t pos = 0, end = 0;
};

std::vector<Token> tokenize(const std::string& s) {
  std::vector<Token> out;
  size_t i = 0;
  while (i < s.size()) {
    const char c = s[i];
    if (std::isspace(static_cast<unsigned char>(c))) { ++i; continue; }
    Token t;
    t.pos = i;
    if (std::isdigit(static_cast<unsigned char>(c)) || (c == '.' && i + 1 < s.size() && std::isdigit(static_cast<unsigned char>(s[i + 1])))) {
      // strtod would read "2e" of "2 east" or the "in" of "5in" wrongly only for e/E: accept an exponent
      // only when digits follow it.
      size_t j = i;
      while (j < s.size() && (std::isdigit(static_cast<unsigned char>(s[j])) || s[j] == '.')) ++j;
      if (j < s.size() && (s[j] == 'e' || s[j] == 'E')) {
        size_t k = j + 1;
        if (k < s.size() && (s[k] == '+' || s[k] == '-')) ++k;
        if (k < s.size() && std::isdigit(static_cast<unsigned char>(s[k]))) {
          while (k < s.size() && std::isdigit(static_cast<unsigned char>(s[k]))) ++k;
          j = k;
        }
      }
      t.kind = Token::Number;
      t.number = std::strtod(s.substr(i, j - i).c_str(), nullptr);
      i = j;
    } else if (ident_start(c)) {
      size_t j = i;
      while (j < s.size() && ident_char(s[j])) ++j;
      t.kind = Token::Ident;
      t.text = s.substr(i, j - i);
      i = j;
    } else if (std::string("+-*/^(),").find(c) != std::string::npos) {
      t.kind = Token::Sym;
      t.text = std::string(1, c);
      ++i;
    } else {
      throw Error("unexpected character '" + std::string(1, c) + "' in expression \"" + s + "\"");
    }
    t.end = i;
    out.push_back(t);
  }
  Token end;
  end.pos = end.end = s.size();
  out.push_back(end);
  return out;
}

Quantity plain(double v) { return Quantity{v, 0, false}; }

void need_plain(const Quantity& q, const char* what) {
  if (q.len != 0 || q.angle) throw Error(std::string(what) + " needs a plain number");
}

}  // namespace

struct Parser {
  const ParamTable& table;
  const std::string& src;
  std::vector<std::string>& stack;
  std::vector<Token> tok;
  size_t at = 0;

  const Token& peek() const { return tok[at]; }
  bool sym(const char* s) {
    if (peek().kind == Token::Sym && peek().text == s) { ++at; return true; }
    return false;
  }
  [[noreturn]] void fail(const std::string& msg) const { throw Error(msg + " in expression \"" + src + "\""); }

  // A unitless operand takes the other side's unit: "10 mm + 5" is 15 mm, "30 deg + 15" is 45 deg.
  Quantity add(Quantity a, Quantity b, double sign) {
    if (a.len == 0 && !a.angle && b.angle) a = Quantity{a.value * kPi / 180.0, 0, true};
    else if (b.len == 0 && !b.angle && a.angle) b = Quantity{b.value * kPi / 180.0, 0, true};
    else if (a.len == 0 && !a.angle) a.len = b.len;
    else if (b.len == 0 && !b.angle) b.len = a.len;
    if (a.len != b.len || a.angle != b.angle) fail("cannot add a length and an angle (or lengths of different powers)");
    a.value += sign * b.value;
    return a;
  }

  Quantity expr() {
    Quantity v = term();
    for (;;) {
      if (sym("+")) v = add(v, term(), 1);
      else if (sym("-")) v = add(v, term(), -1);
      else return v;
    }
  }

  Quantity term() {
    Quantity v = unary();
    for (;;) {
      if (sym("*")) {
        Quantity r = unary();
        v = Quantity{v.value * r.value, v.len + r.len, (v.angle != r.angle) && v.len + r.len == 0};
      } else if (sym("/")) {
        Quantity r = unary();
        if (r.value == 0) fail("division by zero");
        const bool ratio = v.angle && r.angle;  // angle / angle is a plain number
        v = Quantity{v.value / r.value, v.len - r.len, !ratio && v.angle && !r.angle && v.len - r.len == 0};
      } else {
        return v;
      }
    }
  }

  Quantity unary() {
    if (sym("-")) { Quantity v = unary(); v.value = -v.value; return v; }
    if (sym("+")) return unary();
    return power();
  }

  Quantity power() {
    Quantity base = postfix();
    if (!sym("^")) return base;
    Quantity e = unary();
    need_plain(e, "an exponent");
    if (base.angle) fail("cannot raise an angle to a power");
    const double lenPow = base.len * e.value;
    if (base.len != 0 && std::fabs(lenPow - std::round(lenPow)) > 1e-9) fail("that power of a length has no unit");
    return Quantity{std::pow(base.value, e.value), static_cast<int>(std::lround(lenPow)), false};
  }

  // "5 mm", "(a + b) in": a unit name right after an operand scales it.
  Quantity postfix() {
    Quantity v = primary();
    while (peek().kind == Token::Ident) {
      const Unit* u = unit_named(peek().text);
      if (!u) fail("unexpected \"" + peek().text + "\"");
      ++at;
      if (u->angle) {
        if (v.len != 0 || v.angle) fail("a unit was given twice");
        v = Quantity{v.value * u->factor, 0, true};
      } else {
        if (v.angle) fail("a unit was given twice");
        v = Quantity{v.value * u->factor, v.len + 1, false};
      }
    }
    return v;
  }

  Quantity primary() {
    const Token t = peek();
    if (t.kind == Token::Number) { ++at; return plain(t.number); }
    if (sym("(")) {
      Quantity v = expr();
      if (!sym(")")) fail("missing ')'");
      return v;
    }
    if (t.kind == Token::Ident) {
      ++at;
      if (t.text == "PI" || t.text == "pi") return plain(kPi);
      if (t.text == "E") return plain(2.718281828459045);
      if (is_function(t.text)) return call(t.text);
      if (unit_named(t.text)) fail("unit \"" + t.text + "\" without a value");
      return table.value_of(t.text, stack);
    }
    if (t.kind == Token::End) fail("the expression ends too early");
    fail("unexpected \"" + t.text + "\"");
  }

  Quantity call(const std::string& fn) {
    if (!sym("(")) fail(fn + " needs '('");
    std::vector<Quantity> a;
    if (!sym(")")) {
      do a.push_back(expr());
      while (sym(","));
      if (!sym(")")) fail("missing ')'");
    }
    auto argc = [&](size_t n) { if (a.size() != n) fail(fn + " takes " + std::to_string(n) + (n == 1 ? " argument" : " arguments")); };
    auto radians = [&](const Quantity& q) { if (q.len != 0) fail(fn + " needs an angle"); return q.value; };
    if (fn == "sin") { argc(1); return plain(std::sin(radians(a[0]))); }
    if (fn == "cos") { argc(1); return plain(std::cos(radians(a[0]))); }
    if (fn == "tan") { argc(1); return plain(std::tan(radians(a[0]))); }
    if (fn == "asin" || fn == "acos" || fn == "atan") {
      argc(1);
      need_plain(a[0], fn.c_str());
      if (fn != "atan" && std::fabs(a[0].value) > 1) fail(fn + " needs a value between -1 and 1");
      return Quantity{fn == "asin" ? std::asin(a[0].value) : fn == "acos" ? std::acos(a[0].value) : std::atan(a[0].value), 0, true};
    }
    if (fn == "atan2") {
      argc(2);
      if (a[0].len != a[1].len) fail("atan2 needs two values of the same unit");
      return Quantity{std::atan2(a[0].value, a[1].value), 0, true};
    }
    if (fn == "sqrt") {
      argc(1);
      if (a[0].value < 0) fail("sqrt of a negative value");
      if (a[0].angle || a[0].len % 2 != 0) fail("sqrt of that unit has no unit");
      return Quantity{std::sqrt(a[0].value), a[0].len / 2, false};
    }
    if (fn == "abs") { argc(1); a[0].value = std::fabs(a[0].value); return a[0]; }
    if (fn == "sign") { argc(1); return plain(a[0].value > 0 ? 1 : a[0].value < 0 ? -1 : 0); }
    if (fn == "floor" || fn == "ceil" || fn == "round") {
      argc(1);
      // Rounds in the unit the value is shown in (mm, degrees).
      const double k = a[0].angle ? 180.0 / kPi : 1.0;
      const double v = a[0].value * k;
      a[0].value = (fn == "floor" ? std::floor(v) : fn == "ceil" ? std::ceil(v) : std::round(v)) / k;
      return a[0];
    }
    if (fn == "min" || fn == "max") {
      if (a.empty()) fail(fn + " needs at least one argument");
      Quantity best = a[0];
      for (size_t i = 1; i < a.size(); ++i) {
        Quantity d = add(a[i], best, -1);  // unit check (and unit adoption)
        if (fn == "min" ? d.value < 0 : d.value > 0) best = add(d, best, 1);
      }
      return best;
    }
    if (fn == "hypot") {
      argc(2);
      Quantity d = add(a[0], a[1], 1);
      return Quantity{std::hypot(a[0].value, a[1].value), d.len, false};
    }
    if (fn == "pow") {
      argc(2);
      need_plain(a[1], "an exponent");
      if (a[0].angle) fail("cannot raise an angle to a power");
      const double lenPow = a[0].len * a[1].value;
      if (a[0].len != 0 && std::fabs(lenPow - std::round(lenPow)) > 1e-9) fail("that power of a length has no unit");
      return Quantity{std::pow(a[0].value, a[1].value), static_cast<int>(std::lround(lenPow)), false};
    }
    argc(1);
    need_plain(a[0], fn.c_str());
    if (fn == "exp") return plain(std::exp(a[0].value));
    if (a[0].value <= 0) fail(fn + " needs a positive value");
    return plain(fn == "ln" ? std::log(a[0].value) : std::log10(a[0].value));
  }
};

ParamTable::ParamTable(std::vector<ParamDef> defs) : m_defs(std::move(defs)) {}

const ParamDef* ParamTable::find(const std::string& name) const {
  // The last definition of a name wins, like every other "latest op" rule of the log.
  for (auto it = m_defs.rbegin(); it != m_defs.rend(); ++it)
    if (it->name == name) return &*it;
  return nullptr;
}

Quantity ParamTable::eval(const std::string& expr, std::vector<std::string>& stack) const {
  Parser p{*this, expr, stack, tokenize(expr)};
  if (p.peek().kind == Token::End) throw Error("empty expression");
  Quantity v = p.expr();
  if (p.peek().kind != Token::End) p.fail("unexpected \"" + (p.peek().text.empty() ? std::string("number") : p.peek().text) + "\"");
  if (!std::isfinite(v.value)) throw Error("expression \"" + expr + "\" has no finite value");
  return v;
}

Quantity ParamTable::eval(const std::string& expr) const {
  std::vector<std::string> stack;
  return eval(expr, stack);
}

Quantity ParamTable::value_of(const std::string& name, std::vector<std::string>& stack) const {
  if (auto it = m_cache.find(name); it != m_cache.end()) return it->second;
  const ParamDef* d = find(name);
  if (!d) throw Error("unknown parameter \"" + name + "\"");
  if (std::find(stack.begin(), stack.end(), name) != stack.end()) throw Error("parameter \"" + name + "\" refers to itself");
  stack.push_back(name);
  Quantity v = eval(d->expr, stack);
  stack.pop_back();
  m_cache[name] = v;
  return v;
}

Quantity ParamTable::value_of(const std::string& name) const {
  std::vector<std::string> stack;
  return value_of(name, stack);
}

double ParamTable::as(Dim dim, const std::string& expr) const {
  Quantity q = eval(expr);
  switch (dim) {
    case Dim::Length:
      if (q.angle || (q.len != 0 && q.len != 1)) throw Error("\"" + expr + "\" is not a length");
      return q.value;
    case Dim::Angle:
      if (q.len != 0) throw Error("\"" + expr + "\" is not an angle");
      return q.angle ? q.value : q.value * kPi / 180.0;
    case Dim::None:
      if (q.angle || q.len != 0) throw Error("\"" + expr + "\" must be a plain number");
      return q.value;
  }
  return q.value;
}

double ParamTable::length(const std::string& expr) const { return as(Dim::Length, expr); }
double ParamTable::angle(const std::string& expr) const { return as(Dim::Angle, expr); }
double ParamTable::number(const std::string& expr) const { return as(Dim::None, expr); }

int ParamTable::count(const std::string& expr) const {
  const double v = number(expr);
  if (std::fabs(v - std::round(v)) > 1e-9) throw Error("\"" + expr + "\" must be a whole number");
  return static_cast<int>(std::lround(v));
}

bool valid_param_name(const std::string& name) {
  if (name.empty() || !ident_start(name[0])) return false;
  if (!std::all_of(name.begin(), name.end(), ident_char)) return false;
  return !unit_named(name) && !is_function(name) && !is_constant(name);
}

std::vector<std::string> expr_identifiers(const std::string& expr) {
  std::vector<std::string> out;
  std::vector<Token> tok;
  try {
    tok = tokenize(expr);
  } catch (const Error&) {
    return out;
  }
  for (size_t i = 0; i < tok.size(); ++i) {
    const Token& t = tok[i];
    if (t.kind != Token::Ident || is_constant(t.text)) continue;
    const bool called = tok[i + 1].kind == Token::Sym && tok[i + 1].text == "(";
    if (called && is_function(t.text)) continue;
    // A unit name is a unit only right after an operand; parameters cannot take those names anyway.
    if (unit_named(t.text)) continue;
    if (std::find(out.begin(), out.end(), t.text) == out.end()) out.push_back(t.text);
  }
  return out;
}

std::string expr_rename(const std::string& expr, const std::string& from, const std::string& to) {
  std::vector<Token> tok;
  try {
    tok = tokenize(expr);
  } catch (const Error&) {
    return expr;
  }
  std::string out;
  size_t last = 0;
  for (const Token& t : tok) {
    if (t.kind != Token::Ident || t.text != from) continue;
    out += expr.substr(last, t.pos - last) + to;
    last = t.end;
  }
  return out + expr.substr(last);
}

std::string format_quantity(const Quantity& q) {
  char buf[64];
  auto num = [&](double v) {
    std::snprintf(buf, sizeof buf, "%.6f", v);
    std::string s = buf;
    s.erase(s.find_last_not_of('0') + 1);
    if (!s.empty() && s.back() == '.') s.pop_back();
    return s == "-0" ? std::string("0") : s;
  };
  if (q.angle) return num(q.value * 180.0 / kPi) + " deg";
  if (q.len == 0) return num(q.value);
  if (q.len == 1) return num(q.value) + " mm";
  return num(q.value) + " mm^" + std::to_string(q.len);
}

double eval_input(const ParamTable& params, const json& v, Dim dim) {
  if (v.is_number()) return dim == Dim::Angle ? v.get<double>() * kPi / 180.0 : v.get<double>();
  if (v.is_string()) return params.as(dim, v.get<std::string>());
  throw Error("a value must be a number or an expression");
}

}  // namespace opad::design
