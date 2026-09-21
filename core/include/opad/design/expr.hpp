#pragma once
// User parameters and the expressions every numeric design input is written in: "width / 2 + 3 mm",
// "2 * PI * r", "count - 1", "45 deg". Values carry a dimension so "10 mm * 2 in" is an area and cannot be
// used where a length is asked for. Internally lengths are mm and angles radians. No kernel, no Qt.
#include <map>
#include <string>
#include <vector>

#include "../util.hpp"

namespace opad::design {

enum class Dim { None, Length, Angle };

struct Quantity {
  double value = 0;
  int len = 0;         // power of length: 1 = mm, 2 = mm^2, ...
  bool angle = false;  // radians
};

struct ParamDef {
  std::string id;  // the param op's id
  std::string name, expr, comment;
};

// The document's user parameters. Lookup is by name; evaluation follows references between parameters in any
// order and reports cycles as errors.
class ParamTable {
 public:
  ParamTable() = default;
  explicit ParamTable(std::vector<ParamDef> defs);
  const std::vector<ParamDef>& defs() const { return m_defs; }
  const ParamDef* find(const std::string& name) const;

  Quantity eval(const std::string& expr) const;    // throws Error with a readable message
  double length(const std::string& expr) const;    // mm; a plain number is taken as mm
  double angle(const std::string& expr) const;     // radians; a plain number is taken as degrees
  double number(const std::string& expr) const;    // must be dimensionless
  int count(const std::string& expr) const;        // dimensionless whole number
  double as(Dim dim, const std::string& expr) const;
  // Value of a parameter itself (its own dimension). Throws for unknown names, cycles, bad expressions.
  Quantity value_of(const std::string& name) const;

 private:
  friend struct Parser;
  Quantity value_of(const std::string& name, std::vector<std::string>& stack) const;
  Quantity eval(const std::string& expr, std::vector<std::string>& stack) const;
  std::vector<ParamDef> m_defs;
  mutable std::map<std::string, Quantity> m_cache;
};

// Letters, digits and '_', not starting with a digit, and not a unit, function or constant name.
bool valid_param_name(const std::string& name);
// Parameter names an expression refers to (units, functions and constants are not included).
std::vector<std::string> expr_identifiers(const std::string& expr);
// The expression with every reference to parameter `from` replaced by `to` (renaming a parameter).
std::string expr_rename(const std::string& expr, const std::string& from, const std::string& to);
// "12.5 mm", "30 deg", "4", "25 mm^2": for display next to an expression.
std::string format_quantity(const Quantity& q);
// A numeric design input as stored in an op: a JSON number is taken as mm / degrees / a plain number, a
// string is an expression.
double eval_input(const ParamTable& params, const json& v, Dim dim);

}  // namespace opad::design
