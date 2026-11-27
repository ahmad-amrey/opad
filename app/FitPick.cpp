#include "FitPick.hpp"

#include <QCoreApplication>
#include <QLocale>

#include <algorithm>

#include "Units.hpp"
#include "opad/scene.hpp"

namespace fitpick {

namespace {
constexpr const char* kContext = "FitPick";
QString tr(const char* text) { return QCoreApplication::translate(kContext, text); }
}  // namespace

bool onMesh(const opad::Scene& scene, const opad::Ref& ref) {
  const opad::Node* n = scene.node(ref.body);
  return n && n->kind == opad::Node::Kind::Body && n->representation == "mesh";
}

opad::json fromPick(const opad::Scene& scene, const opad::Ref& ref, bool axis) {
  if (!onMesh(scene, ref) || ref.index < 0) return {};
  int seed = -1;
  if (ref.kind == opad::Ref::Kind::Face) seed = ref.index;
  else if (ref.kind == opad::Ref::Kind::Edge) seed = ref.index / 3;  // side k of triangle t is 3 t + k (BodyShape's MeshPickSet)
  if (seed < 0) return {};
  opad::json fit = {{"body", ref.body}, {"seed", seed}, {"of", axis ? "auto" : "plane"}};
  fit["tolerance"] = axis ? "20 deg" : "10 deg";
  return {{"fit", fit}};
}

opad::json fromTriangles(const opad::Scene& scene, const std::vector<opad::Ref>& refs) {
  if (refs.size() < 2) return {};
  std::vector<int> tris;
  const std::string body = refs.front().body;
  for (const auto& r : refs) {
    if (r.body != body || r.kind != opad::Ref::Kind::Face || r.index < 0) return {};
    tris.push_back(r.index);
  }
  if (!onMesh(scene, refs.front())) return {};
  std::sort(tris.begin(), tris.end());
  tris.erase(std::unique(tris.begin(), tris.end()), tris.end());
  opad::json ranges = opad::json::array();  // [first, last] runs: a box over a scan takes runs of neighbours
  for (size_t i = 0; i < tris.size();) {
    size_t j = i;
    while (j + 1 < tris.size() && tris[j + 1] == tris[j] + 1) ++j;
    if (j == i) ranges.push_back(tris[i]);
    else ranges.push_back({tris[i], tris[j]});
    i = j + 1;
  }
  return {{"fit", {{"body", body}, {"triangles", ranges}, {"of", "plane"}}}};
}

opad::json fromPoints(const std::vector<opad::Ref>& refs, bool line) {
  opad::json points = opad::json::array();
  std::string body;
  bool one = true;
  for (const auto& r : refs) {
    points.push_back(r.to_json());
    if (body.empty()) body = r.body;
    one = one && r.body == body && !body.empty();
  }
  opad::json fit = {{"points", points}, {"of", line ? "line" : "plane"}};
  if (one) fit["body"] = body;  // all on one body: the fit moves with it
  return {{"fit", fit}};
}

bool isFit(const opad::json& v) { return v.is_object() && v.contains("fit") && v["fit"].is_object(); }
bool fitted(const opad::json& v) { return isFit(v) && v.contains("result") && v["result"].is_object() && v["result"].contains("origin"); }

QString title(const opad::json& v) {
  if (!isFit(v)) return {};
  const opad::json& fit = v["fit"];
  std::string of = fitted(v) ? v["result"].value("of", std::string()) : fit.value("of", std::string());
  if (of == "scan_axes") return tr("Scan axes");
  if (of == "cylinder") {
    if (fitted(v) && v["result"].contains("radius")) return tr("Round axis %1").arg(units::format(units::Kind::Length, 2 * v["result"].value("radius", 0.0)));
    return tr("Round face axis");
  }
  if (of == "normal") return tr("Fitted normal");
  if (of == "wall") return tr("Wall direction");
  if (of == "line") return fit.contains("points") ? tr("Line through %1 points").arg(int(fit["points"].size())) : tr("Fitted line");
  if (of == "intersection") return tr("Where two planes meet");
  if (of == "auto") return tr("Fitted axis");
  return fit.contains("points") ? tr("Plane through %1 points").arg(int(fit["points"].size())) : tr("Fitted plane");
}

QString quality(const opad::json& v) {
  if (!fitted(v)) return isFit(v) ? tr("Fitting…") : QString();
  const opad::json& r = v["result"];
  const opad::json q = r.value("quality", opad::json::object());
  const std::string g = q.value("grade", std::string());
  QStringList parts;
  if (r.value("of", std::string()) == "scan_axes") {
    parts << tr("base %1 of %2").arg(v["fit"].value("base", 0) % std::max(1, r.value("bases", 1)) + 1).arg(r.value("bases", 1));
    parts << tr("X %1 of %2").arg(v["fit"].value("turn", 0) % std::max(1, r.value("turns", 1)) + 1).arg(r.value("turns", 1));
    if (r.value("method", std::string()) == "pca") {
      parts << tr("no flats: principal axes");
      return parts.join(QString::fromUtf8(" · "));
    }
    parts << tr("%1% of the area").arg(qRound(100 * r.value("share", 0.0)));
  }
  parts << tr("rms %1").arg(units::format(units::Kind::Length, q.value("rms_mm", 0.0), 3));
  const int used = q.value("used", 0), total = q.value("total", 0);
  parts << (used == total ? QLocale().toString(used) : tr("%1 of %2").arg(QLocale().toString(used), QLocale().toString(total)));
  parts << (g == "good" ? tr("good") : g == "fair" ? tr("fair") : g == "poor" ? tr("poor: not that shape") : QString::fromStdString(g));
  return parts.join(QString::fromUtf8(" · "));
}

std::string grade(const opad::json& v) { return fitted(v) ? v["result"].value("quality", opad::json::object()).value("grade", std::string()) : std::string(); }

}  // namespace fitpick
