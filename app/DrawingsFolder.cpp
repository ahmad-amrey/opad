#include "DrawingsFolder.hpp"

#include <QAction>
#include <QClipboard>
#include <QCoreApplication>
#include <QGuiApplication>
#include <QMenu>

#include <map>
#include <set>
#include <unordered_map>

#include "AppDocument.hpp"
#include "CommandHelp.hpp"
#include "Icons.hpp"
#include "KeyText.hpp"
#include "opad/drawing/sheet.hpp"

namespace drawings {
namespace {

const char* const kContext = "drawings";
QString tr(const char* text) { return QCoreApplication::translate(kContext, text); }
QString qs(const std::string& s) { return QString::fromStdString(s); }
QString mm(double v) { return QString::number(v, 'g', 6); }

const std::string kDrawing = "drawing:";
bool isDrawing(const std::string& id) { return id.rfind(kDrawing, 0) == 0; }

QString orientation(const std::string& o) {
  if (o == "front") return tr("Front view");
  if (o == "back") return tr("Back view");
  if (o == "top") return tr("Top view");
  if (o == "bottom") return tr("Bottom view");
  if (o == "right") return tr("Right view");
  if (o == "left") return tr("Left view");
  if (o == "iso") return tr("Isometric view");
  if (o == "iso-back") return tr("Back isometric view");
  return tr("View");
}

// A section, detail or auxiliary view by its letter (UI-82), else by what it shows.
QString kindName(const opad::SheetView& v, const std::string& orient) {
  const QString letter = qs(v.def.value("letter", ""));
  if (v.kind == "section") return letter.isEmpty() ? tr("Section view") : tr("Section %1-%1").arg(letter);
  if (v.kind == "detail") return letter.isEmpty() ? tr("Detail view") : tr("Detail %1").arg(letter);
  if (v.kind == "auxiliary") return letter.isEmpty() ? tr("Auxiliary view") : tr("Auxiliary view %1").arg(letter);
  return orientation(orient);
}

QString viewName(const opad::Scene& s, const opad::SheetView& v) {
  return v.name.empty() ? kindName(v, opad::drawing::view_orientation(s, v)) : qs(v.name);
}

QString itemType(const opad::SheetItem& t) {
  if (t.kind == "note") return t.def.contains("refs") ? tr("Note with a leader") : tr("Note");
  if (t.kind == "centermark") return tr("Centre mark");
  if (t.kind == "centerline") return tr("Centre line");
  if (t.kind == "hole_callout") return tr("Hole callout");
  if (t.kind == "hole_table") return tr("Hole table");
  if (t.kind == "datum") return tr("Datum");
  if (t.kind == "fcf") return tr("Feature control frame");
  if (t.kind == "surface") return tr("Surface texture");
  if (t.kind == "parts_list") return tr("Parts list");
  if (t.kind == "balloon") return tr("Balloon");
  if (t.kind == "revision_table") return tr("Revision table");
  if (t.kind == "issue") return tr("Issued revision");
  if (t.kind == "dimension_set")
    return t.type == "ordinate" ? tr("Ordinate dimensions") : t.type == "baseline" ? tr("Baseline dimensions") : t.type == "chain" ? tr("Chain dimensions") : qs(t.type);
  if (t.kind != "dimension") return qs(t.kind);
  if (t.type == "horizontal") return tr("Horizontal dimension");
  if (t.type == "vertical") return tr("Vertical dimension");
  if (t.type == "aligned") return tr("Aligned dimension");
  if (t.type == "radius") return tr("Radius dimension");
  if (t.type == "diameter") return tr("Diameter dimension");
  if (t.type == "angle") return tr("Angle dimension");
  return qs(t.type);
}

QString projection(const std::string& p) { return p == "first" ? tr("first angle") : p == "third" ? tr("third angle") : qs(p); }

// The records by id: a sheet of hundreds of dimensions is listed in one pass, not a search per row.
struct Index {
  std::unordered_map<std::string, const opad::Sheet*> sheets;
  std::unordered_map<std::string, const opad::SheetView*> views;
  std::unordered_map<std::string, const opad::SheetItem*> items;
  explicit Index(const opad::Scene& s) {
    for (const auto& x : s.sheets) sheets[x.id] = &x;
    for (const auto& x : s.sheet_views) views[x.id] = &x;
    for (const auto& x : s.sheet_items) items[x.id] = &x;
  }
  template <class T> static const T* get(const std::unordered_map<std::string, const T*>& m, const std::string& id) {
    const auto it = m.find(id);
    return it == m.end() ? nullptr : it->second;
  }
};

Row make(const opad::Scene& s, const Index& ix, const opad::drawing::OutlineRow& o) {
  Row r;
  r.id = o.id;
  r.error = !o.error.empty();
  QStringList tip;
  if (o.kind == "drawing") {
    r.name = qs(o.name);
    r.icon = "drawing";
    r.editable = true;
    tip << r.name << tr("Drawing · %1 sheets").arg(o.children.size());
  } else if (const opad::Sheet* sheet = o.kind == "sheet" ? Index::get(ix.sheets, o.id) : nullptr) {
    r.name = qs(o.name);
    r.icon = "drawingSheet";  // DocsArea.cpp's icon table
    r.editable = true;
    const opad::json size = sheet->def.value("size", opad::json::object());
    const QString paper = QString::fromUtf8("%1 × %2 mm").arg(mm(sheet->width), mm(sheet->height));
    tip << r.name << QString::fromUtf8("%1 · %2 · %3 · %4").arg(size.contains("preset") ? qs(size.value("preset", "")) + QString::fromUtf8(" · ") + paper : paper,
                                                               qs(sheet->standard).toUpper(), projection(sheet->projection), qs(opad::drawing::scale_text(sheet->scale)));
  } else if (const opad::SheetView* view = o.kind == "view" ? Index::get(ix.views, o.id) : nullptr) {
    r.name = o.name.empty() ? kindName(*view, o.orient) : qs(o.name);
    r.icon = view->kind == "section" ? "viewSection" : view->kind == "detail" ? "viewDetail" : view->kind == "auxiliary" ? "viewAuxiliary" : "ortho";
    r.editable = true;
    tip << r.name;
    if (const opad::SheetView* parent = view->kind == "section" || view->kind == "detail" || view->kind == "auxiliary" ? Index::get(ix.views, view->parent) : nullptr)
      tip << (view->kind == "section" ? tr("Section of %1") : view->kind == "detail" ? tr("Detail of %1") : tr("Auxiliary view of %1")).arg(viewName(s, *parent));
    if (view->def.contains("crop")) tip << tr("Cropped");
    if (view->def.contains("breaks")) tip << tr("Broken view");
    if (view->kind == "base") {
      const std::string scale = view->def.value("scale", "sheet");
      const opad::Sheet* sheet = Index::get(ix.sheets, view->sheet);
      tip << QString::fromUtf8("%1 · %2").arg(tr("Base view"), scale != "sheet" ? qs(scale) : sheet ? qs(opad::drawing::scale_text(sheet->scale)) : QString());
    } else if (const opad::SheetView* parent = view->kind == "projected" ? Index::get(ix.views, view->parent) : nullptr) {
      tip << tr("Projected from %1").arg(viewName(s, *parent));
    }
  } else if (const opad::SheetItem* item = o.kind == "item" ? Index::get(ix.items, o.id) : nullptr) {
    r.name = o.name.empty() ? itemType(*item) : item->kind == "balloon" ? tr("Balloon %1").arg(qs(o.name)) : item->kind == "issue" ? tr("Revision %1").arg(qs(o.name)) : qs(o.name);
    static const std::map<std::string, const char*> kindIcons = {
        {"dimension", "dimension"},   {"note", "text"},          {"centermark", "centerMark"}, {"centerline", "centerLine"},
        {"hole_callout", "holeCallout"}, {"hole_table", "holeTable"}, {"datum", "datumSymbol"},  {"fcf", "featureFrame"},
        {"surface", "surfaceTexture"}, {"parts_list", "partsList"}, {"balloon", "balloon"}, {"revision_table", "revisionTable"}, {"issue", "issueRevision"}};
    const auto icon = kindIcons.find(item->kind);
    r.icon = item->kind == "dimension_set" ? (item->type == "baseline" ? "dimBaseline" : item->type == "chain" ? "dimChain" : "dimOrdinate")
             : icon != kindIcons.end() ? icon->second : "dot";
    tip << itemType(*item);
    if ((item->kind == "dimension" || item->kind == "hole_callout" || item->kind == "dimension_set") && !o.name.empty()) tip << tr("%1 when it was made").arg(r.name);
    else if (item->kind == "note") tip << qs(item->def.value("text", ""));
    else if (item->kind == "issue")
      tip << QString::fromUtf8("%1 · %2").arg(qs(item->def.value("date", "")), qs(item->def.value("description", ""))) << qs(item->def.value("pdf", ""));
  }
  if (r.error) {
    r.icon = "warning";
    tip << qs(o.error);
  }
  if (!isDrawing(o.id)) tip << qs(o.id.substr(0, 8));
  r.tooltip = tip.join('\n');
  for (const auto& c : o.children) r.children.push_back(make(s, ix, c));
  return r;
}

std::vector<std::string> sheetsOf(const opad::Scene& s, const std::string& drawing) {
  std::vector<std::string> ids;
  for (const auto& sheet : s.sheets)
    if (sheet.drawing == drawing) ids.push_back(sheet.id);
  return ids;
}

QString nameOf(const opad::Scene& s, const std::string& id) {
  if (const opad::Sheet* sheet = s.sheet(id)) return qs(sheet->name);
  if (const opad::SheetView* view = s.sheet_view(id)) return viewName(s, *view);
  if (const opad::SheetItem* item = s.sheet_item(id)) return itemType(*item);
  return qs(id.substr(0, 8));
}

}  // namespace

std::vector<Row> rows(const AppDocument& doc) {
  std::vector<Row> out;
  if (!doc.hasDocument) return out;
  const Index ix(doc.scene);
  for (const auto& o : opad::drawing::outline(doc.scene)) out.push_back(make(doc.scene, ix, o));
  return out;
}

void rename(AppDocument* doc, const std::string& id, const QString& name) {
  const std::string text = name.trimmed().toStdString();
  if (text.empty()) throw opad::Error(tr("A name cannot be empty.").toStdString());
  if (isDrawing(id)) {
    opad::json ops = opad::json::array();
    for (const auto& sheet : sheetsOf(doc->scene, id.substr(kDrawing.size())))
      ops.push_back({{"op", "edit"}, {"target", sheet}, {"set", {{"drawing", text}}}});
    if (!ops.empty()) doc->run("append", {{"ops", ops}});
  } else if (doc->scene.sheet(id) || doc->scene.sheet_view(id)) {
    doc->run("sheet_edit", {{"target", id}, {"set", {{"name", text}}}});
  } else {
    throw opad::Error(tr("Only drawings, sheets and views have names.").toStdString());
  }
}

int remove(AppDocument* doc, const std::vector<std::string>& ids) {
  const opad::Scene& s = doc->scene;
  const Index ix(s);
  std::vector<std::string> chosen;
  std::set<std::string> in;
  const auto add = [&](const std::string& id) {
    if (in.insert(id).second) chosen.push_back(id);
  };
  QStringList names;  // the first few, for the status bar
  int named = 0;
  const auto name = [&](const QString& n) {
    if (++named <= 3) names << n;
  };
  for (const auto& id : ids) {
    if (isDrawing(id)) {
      const auto sheets = sheetsOf(s, id.substr(kDrawing.size()));
      for (const auto& sheet : sheets) add(sheet);
      if (!sheets.empty()) name(qs(id.substr(kDrawing.size())));
    } else if (ix.sheets.count(id) || ix.views.count(id) || ix.items.count(id)) {
      if (!in.count(id)) name(nameOf(s, id));
      add(id);
    }
  }
  if (named > 3) names << QString("+%1").arg(named - 3);
  // What goes with a sheet or a view that goes too needs no tombstone of its own.
  const auto viewGoes = [&](const std::string& id) {
    int depth = 0;
    for (const opad::SheetView* v = Index::get(ix.views, id); v && depth < 64; v = Index::get(ix.views, v->parent), ++depth)
      if (in.count(v->sheet) || (v->id != id && in.count(v->id))) return true;
    return false;
  };
  opad::json ops = opad::json::array();
  for (const auto& id : chosen) {
    if (ix.views.count(id) && viewGoes(id)) continue;
    if (const opad::SheetItem* t = Index::get(ix.items, id); t && (in.count(t->sheet) || (!t->view.empty() && (in.count(t->view) || viewGoes(t->view))))) continue;
    ops.push_back({{"op", "delete"}, {"target", id}});
  }
  if (ops.empty()) return 0;
  doc->run("append", {{"ops", ops}});
  emit doc->message(help::expand(tr("Deleted %1 · Undo ({key:edit.undo}) brings it back").arg(names.join(", "))));
  return int(ops.size());
}

void contextMenu(AppDocument* doc, const std::string& id, QMenu& menu, const std::function<void()>& startRename, const std::function<void()>& exportSheet) {
  const opad::Scene& s = doc->scene;
  if (!s.sheet_item(id)) menu.addAction(icons::themed("rename", 16), tr("Rename"), startRename)->setObjectName("drawings.rename");
  if (!isDrawing(id)) {
    menu.addAction(icons::themed("commit", 16), tr("Copy id"), [id] { QGuiApplication::clipboard()->setText(qs(id)); })->setObjectName("drawings.copyId");
  }
  if ((s.sheet(id) || isDrawing(id)) && exportSheet)
    menu.addAction(icons::themed("export", 16), isDrawing(id) ? tr("Export drawing…") : tr("Export sheet…"), exportSheet)->setObjectName("drawings.export");
  menu.addSeparator();
  const QString what = isDrawing(id) ? tr("Delete drawing") : s.sheet(id) ? tr("Delete sheet") : s.sheet_view(id) ? tr("Delete view") : tr("Delete");
  menu.addAction(icons::themed("delete", 16), keys::menuText(what, QStringLiteral("edit.delete")), [doc, id] {  // Del is edit.delete's
        try {
          remove(doc, {id});
        } catch (const std::exception& e) {
          emit doc->message(QString::fromUtf8(e.what()));
        }
      })->setObjectName("drawings.delete");
}

}  // namespace drawings
