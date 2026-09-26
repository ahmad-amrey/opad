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
                                  "floor", "ceil", "round", "pow", "exp", "ln", "log", "hypot", "sign",
                                  "if", "select", "clamp", "mod", "assert"};
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
  enum Kind { End, Number, Ident, Sym, String } kind = End;
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
    } else if (c == '"' || c == '\'') {  // a message for assert
      const size_t close = s.find(c, i + 1);
      if (close == std::string::npos) throw Error("a text in quotes is not closed in expression \"" + s + "\"");
      t.kind = Token::String;
      t.text = s.substr(i + 1, close - i - 1);
      i = close + 1;
    } else if (i + 1 < s.size() && (s.compare(i, 2, "<=") == 0 || s.compare(i, 2, ">=") == 0 || s.compare(i, 2, "==") == 0 ||
                                    s.compare(i, 2, "!=") == 0 || s.compare(i, 2, "&&") == 0 || s.compare(i, 2, "||") == 0)) {
      t.kind = Token::Sym;
      t.text = s.substr(i, 2);
      i += 2;
    } else if (std::string("+-*/^(),%<>!?:").find(c) != std::string::npos) {
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
Quantity derived(double v) { return Quantity{v, 0, false, true}; }
Quantity truth(bool v) { return derived(v ? 1 : 0); }

void need_plain(const Quantity& q, const char* what) {
  if (q.len != 0 || q.angle) throw Error(std::string(what) + " needs a plain number");
}

}  // namespace

// Recursive descent, evaluating as it parses. Lowest to highest: c ? a : b, ||, &&, comparisons, + -, * / %,
// unary - + !, ^, a unit after an operand. Branches not taken (if, select, ?:, the right of && and ||) are parsed
// "quietly": a value error there (sqrt of a negative, a failing assert, a failing parameter) does not count, so a
// guard such as if(x > 0, sqrt(x), 0) works; syntax errors and unknown names still do.
struct Parser {
  const ParamTable& table;
  const std::string& src;
  std::vector<std::string>& stack;
  std::vector<Token> tok;
  size_t at = 0;
  int quiet = 0;

  const Token& peek() const { return tok[at]; }
  bool sym(const char* s) {
    if (peek().kind == Token::Sym && peek().text == s) { ++at; return true; }
    return false;
  }
  [[noreturn]] void fail(const std::string& msg) const { throw Error(msg + " in expression \"" + src + "\""); }
  // A value error: thrown unless in a branch not taken, where it gives NaN (never read).
  bool bad(const std::string& msg) const {
    if (quiet == 0) fail(msg);
    return true;
  }

  // Units made to match for adding or comparing. A unitless operand takes the other side's unit: "10 mm + 5" is
  // 15 mm, "30 deg + 15" is 45 deg. A plain number a function computed is not a count of degrees (gap log #7:
  // tan(45 deg) + asin(1) read 1 + 90 deg as 91 deg): that is refused.
  void unify(Quantity& a, Quantity& b) {
    auto degrees = [&](Quantity& q) {
      if (q.derived) fail("a plain number from a function is not an angle: multiply it by 1 rad or 1 deg");
      q = Quantity{q.value * kPi / 180.0, 0, true};
    };
    if (a.len == 0 && !a.angle && b.angle) degrees(a);
    else if (b.len == 0 && !b.angle && a.angle) degrees(b);
    else if (a.len == 0 && !a.angle) a.len = b.len;
    else if (b.len == 0 && !b.angle) b.len = a.len;
    if (a.len != b.len || a.angle != b.angle) fail("cannot add a length and an angle (or lengths of different powers)");
  }

  Quantity add(Quantity a, Quantity b, double sign) {
    unify(a, b);
    a.value += sign * b.value;
    a.derived = a.derived || b.derived;
    return a;
  }

  // Parses what `parse` reads, quietly unless `taken`.
  template <class F>
  Quantity branch(bool taken, F&& parse) {
    if (!taken) ++quiet;
    Quantity v = parse();
    if (!taken) --quiet;
    return v;
  }

  Quantity expr() {
    Quantity c = logical_or();
    if (!sym("?")) return c;
    const bool yes = c.value != 0;
    Quantity a = branch(yes, [&] { return expr(); });
    if (!sym(":")) fail("? needs a ':'");
    Quantity b = branch(!yes, [&] { return expr(); });
    unify(a, b);
    return yes ? a : b;
  }

  Quantity logical_or() {
    Quantity v = logical_and();
    while (sym("||")) {
      const bool left = v.value != 0;
      const Quantity r = branch(!left, [&] { return logical_and(); });
      v = truth(left || r.value != 0);
    }
    return v;
  }

  Quantity logical_and() {
    Quantity v = comparison();
    while (sym("&&")) {
      const bool left = v.value != 0;
      const Quantity r = branch(left, [&] { return comparison(); });
      v = truth(left && r.value != 0);
    }
    return v;
  }

  Quantity comparison() {
    Quantity a = sum();
    for (const char* op : {"<=", ">=", "==", "!=", "<", ">"}) {
      if (!sym(op)) continue;
      Quantity b = sum();
      unify(a, b);
      const double d = a.value - b.value, tol = 1e-9 * std::max({1.0, std::fabs(a.value), std::fabs(b.value)});
      const std::string o = op;
      const bool r = o == "<" ? d < -tol : o == ">" ? d > tol : o == "<=" ? d <= tol : o == ">=" ? d >= -tol : o == "==" ? std::fabs(d) <= tol : std::fabs(d) > tol;
      return truth(r);
    }
    return a;
  }

  Quantity sum() {
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
        const int len = v.len + r.len;
        const bool angle = (v.angle != r.angle) && len == 0;
        v = Quantity{v.value * r.value, len, angle, !angle && len == 0 && (v.derived || r.derived)};
      } else if (sym("/")) {
        Quantity r = unary();
        if (r.value == 0 && bad("division by zero")) return Quantity{NAN, v.len - r.len, false};
        const bool ratio = v.angle && r.angle;  // angle / angle is a plain number
        const int len = v.len - r.len;
        const bool angle = !ratio && v.angle && !r.angle && len == 0;
        v = Quantity{v.value / r.value, len, angle, !angle && len == 0 && (v.derived || r.derived)};
      } else if (sym("%")) {
        v = modulo(v, unary());
      } else {
        return v;
      }
    }
  }

  // Floored: the result takes the sign of the divisor, so mod(-30 deg, 360 deg) is 330 deg.
  Quantity modulo(Quantity a, Quantity b) {
    unify(a, b);
    if (b.value == 0 && bad("modulo by zero")) return Quantity{NAN, a.len, a.angle};
    a.value -= b.value * std::floor(a.value / b.value);
    a.derived = a.derived || b.derived;
    return a;
  }

  Quantity unary() {
    if (sym("-")) { Quantity v = unary(); v.value = -v.value; return v; }
    if (sym("+")) return unary();
    if (sym("!")) return truth(unary().value == 0);
    return power();
  }

  Quantity power() {
    Quantity base = postfix();
    if (!sym("^")) return base;
    Quantity e = unary();
    return raise(base, e);
  }

  Quantity raise(const Quantity& base, const Quantity& e) {
    need_plain(e, "an exponent");
    if (base.angle) fail("cannot raise an angle to a power");
    const double lenPow = base.len * e.value;
    if (base.len != 0 && std::fabs(lenPow - std::round(lenPow)) > 1e-9)
      fail("that power of a length has no unit (mm^" + format_power(lenPow) + "): divide by 1 mm first, e.g. (x / 1 mm)^1.5");
    return Quantity{std::pow(base.value, e.value), static_cast<int>(std::lround(lenPow)), false, base.derived && base.len == 0};
  }

  static std::string format_power(double p) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%g", p);
    return buf;
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
      // A function name only when called: an older parameter may share a new function's name.
      if (is_function(t.text) && peek().kind == Token::Sym && peek().text == "(") return call(t.text);
      if (unit_named(t.text)) fail("unit \"" + t.text + "\" without a value");
      if (quiet && table.find(t.text)) {
        const size_t depth = stack.size();
        try {
          return table.value_of(t.text, stack);
        } catch (const Error& e) {
          stack.resize(depth);  // the failed evaluation left its names on the stack
          if (std::string(e.what()).find("refers to itself") != std::string::npos) throw;
          return Quantity{NAN, 0, false};
        }
      }
      return table.value_of(t.text, stack);
    }
    if (t.kind == Token::String) fail("a text in quotes is only an assert's message");
    if (t.kind == Token::End) fail("the expression ends too early");
    fail("unexpected \"" + t.text + "\"");
  }

  // if(c, a, b), select(i, v0, v1, ...), assert(c, "message"): arguments after the first are parsed as needed.
  Quantity lazy_call(const std::string& fn) {
    Quantity first = expr();
    if (fn == "assert") {
      std::string message = "assert failed";
      if (sym(",")) {
        if (peek().kind != Token::String) fail("assert's message goes in quotes");
        message = peek().text;
        ++at;
      }
      if (!sym(")")) fail("missing ')'");
      if (first.value == 0 && bad(message)) return Quantity{NAN, 0, false};
      return derived(1);
    }
    long pick = 0;
    if (fn == "if") {
      pick = first.value != 0 ? 0 : 1;
    } else {  // select
      need_plain(first, "select's index");
      if (std::fabs(first.value - std::round(first.value)) > 1e-9) fail("select's index must be a whole number");
      pick = std::lround(first.value);
    }
    std::vector<Quantity> options;
    while (sym(",")) {
      const bool taken = static_cast<long>(options.size()) == pick;
      options.push_back(branch(taken, [&] { return expr(); }));
    }
    if (!sym(")")) fail("missing ')'");
    if (fn == "if" && options.size() != 2) fail("if takes a condition and two values");
    if (fn == "select" && options.empty()) fail("select needs values to choose from");
    for (size_t i = 1; i < options.size(); ++i) unify(options[0], options[i]);  // one unit for every value
    for (size_t i = 1; i < options.size(); ++i) unify(options[i], options[0]);
    if (pick < 0 || pick >= static_cast<long>(options.size())) {
      if (bad("select's index " + std::to_string(pick) + " is outside 0 to " + std::to_string(options.size() - 1))) return Quantity{NAN, options[0].len, options[0].angle};
    }
    return options[static_cast<size_t>(pick)];
  }

  Quantity call(const std::string& fn) {
    if (!sym("(")) fail(fn + " needs '('");
    if (fn == "if" || fn == "select" || fn == "assert") return lazy_call(fn);
    std::vector<Quantity> a;
    if (!sym(")")) {
      do a.push_back(expr());
      while (sym(","));
      if (!sym(")")) fail("missing ')'");
    }
    auto argc = [&](size_t n) { if (a.size() != n) fail(fn + " takes " + std::to_string(n) + (n == 1 ? " argument" : " arguments")); };
    auto radians = [&](const Quantity& q) { if (q.len != 0) fail(fn + " needs an angle"); return q.value; };
    if (fn == "sin") { argc(1); return derived(std::sin(radians(a[0]))); }
    if (fn == "cos") { argc(1); return derived(std::cos(radians(a[0]))); }
    if (fn == "tan") { argc(1); return derived(std::tan(radians(a[0]))); }
    if (fn == "asin" || fn == "acos" || fn == "atan") {
      argc(1);
      need_plain(a[0], fn.c_str());
      if (fn != "atan" && std::fabs(a[0].value) > 1 && bad(fn + " needs a value between -1 and 1")) return Quantity{NAN, 0, true};
      return Quantity{fn == "asin" ? std::asin(a[0].value) : fn == "acos" ? std::acos(a[0].value) : std::atan(a[0].value), 0, true};
    }
    if (fn == "atan2") {
      argc(2);
      if (a[0].len != a[1].len) fail("atan2 needs two values of the same unit");
      return Quantity{std::atan2(a[0].value, a[1].value), 0, true};
    }
    if (fn == "sqrt") {
      argc(1);
      if (a[0].angle || a[0].len % 2 != 0) fail("sqrt of that unit has no unit");
      if (a[0].value < 0 && bad("sqrt of a negative value")) return Quantity{NAN, a[0].len / 2, false};
      return Quantity{std::sqrt(a[0].value), a[0].len / 2, false, a[0].derived && a[0].len == 0};
    }
    if (fn == "abs") { argc(1); a[0].value = std::fabs(a[0].value); return a[0]; }
    if (fn == "sign") { argc(1); return derived(a[0].value > 0 ? 1 : a[0].value < 0 ? -1 : 0); }
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
    if (fn == "clamp") {
      argc(3);
      unify(a[0], a[1]);
      unify(a[0], a[2]);
      unify(a[1], a[2]);
      if (a[1].value > a[2].value && bad("clamp's lower limit is above its upper one")) return Quantity{NAN, a[0].len, a[0].angle};
      a[0].value = std::min(std::max(a[0].value, a[1].value), a[2].value);
      return a[0];
    }
    if (fn == "mod") { argc(2); return modulo(a[0], a[1]); }
    if (fn == "hypot") {
      argc(2);
      Quantity d = add(a[0], a[1], 1);
      return Quantity{std::hypot(a[0].value, a[1].value), d.len, false};
    }
    if (fn == "pow") {
      argc(2);
      return raise(a[0], a[1]);
    }
    argc(1);
    need_plain(a[0], fn.c_str());
    if (fn == "exp") return derived(std::exp(a[0].value));
    if (a[0].value <= 0 && bad(fn + " needs a positive value")) return Quantity{NAN, 0, false};
    return derived(fn == "ln" ? std::log(a[0].value) : std::log10(a[0].value));
  }
};

ParamTable::ParamTable(std::vector<ParamDef> defs,std::string unit) : m_unit(std::move(unit)),m_defs(std::move(defs)) {
  const auto* u=unit_named(m_unit);if(!u||u->angle)throw Error("unsupported document length unit");
}
std::string ParamTable::explicit_length(const std::string& expression) const {
  const auto q=eval(expression);
  return !q.angle && q.len==0 ? "("+expression+") * 1 "+m_unit : expression;
}

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
      return q.len==0?q.value*unit_named(m_unit)->factor:q.value;
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
