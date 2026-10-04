#include "SheetPage.hpp"

#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QPushButton>
#include <QSettings>
#include <QStackedWidget>
#include <QTabBar>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <set>

#include "AppDocument.hpp"
#include "Icons.hpp"
#include "SheetAnnotate.hpp"
#include "SheetCanvas.hpp"
#include "SheetViewTool.hpp"
#include "Theme.hpp"
#include "opad/drawing/tables.hpp"

SheetPage::SheetPage(AppDocument* doc, JobRunner* jobs, QWidget* parent) : QWidget(parent), m_doc(doc) {
  setObjectName("sheetPage");
  auto* v = new QVBoxLayout(this);
  v->setContentsMargins(0, 0, 0, 0);
  v->setSpacing(0);
  m_stack = new QStackedWidget(this);
  m_canvas = new SheetCanvas(doc, jobs, m_stack);
  // Nothing to show yet: what a drawing is and how to start one.
  auto* start = new QWidget(m_stack);
  start->setObjectName("sheetStart");
  auto* sv = new QVBoxLayout(start);
  sv->addStretch(2);
  auto* icon = new QLabel(start);
  icon->setAlignment(Qt::AlignCenter);
  icon->setPixmap(icons::pixmap("drawingSheet", theme::current().fg2, 48, devicePixelRatioF()));
  auto* title = new QLabel(tr("No drawing yet"), start);
  title->setFont(theme::ui(18, QFont::DemiBold));
  title->setAlignment(Qt::AlignCenter);
  auto* text = new QLabel(tr("A drawing shows the model in standard views on sheets with a frame and a title block, ready to print or export as PDF, DXF, DWG or SVG."), start);
  text->setObjectName("secondary");
  text->setWordWrap(true);
  text->setAlignment(Qt::AlignCenter);
  text->setMaximumWidth(460);
  auto* textRow = new QHBoxLayout();  // centred without an alignment flag, which would keep its wrapped height from it
  textRow->addStretch();
  textRow->addWidget(text, 1);
  textRow->addStretch();
  auto* create = new QPushButton(tr("New drawing…"), start);
  create->setObjectName("primary");
  create->setCursor(Qt::PointingHandCursor);
  sv->addWidget(icon);
  sv->addSpacing(8);
  sv->addWidget(title);
  sv->addLayout(textRow);
  sv->addSpacing(12);
  sv->addWidget(create, 0, Qt::AlignHCenter);
  sv->addStretch(3);
  m_stack->addWidget(m_canvas);
  m_stack->addWidget(start);
  v->addWidget(m_stack, 1);
  m_annotator = new SheetAnnotator(doc, m_canvas, this);
  m_viewTool = new SheetViewTool(doc, m_canvas, this);
  v->addWidget(m_annotator->bar());
  // The bar: sheets, +, prompt, cursor, sheet info.
  auto* bar = new QWidget(this);
  bar->setObjectName("sheetBar");
  bar->setAutoFillBackground(true);
  auto* h = new QHBoxLayout(bar);
  h->setContentsMargins(6, 0, 10, 0);
  h->setSpacing(10);
  m_tabs = new QTabBar(bar);
  m_tabs->setObjectName("sheetTabs");
  m_tabs->setDrawBase(false);
  m_tabs->setExpanding(false);
  m_tabs->setDocumentMode(true);
  m_add = new QToolButton(bar);
  m_add->setAutoRaise(true);
  m_add->setToolTip(tr("New sheet in this drawing"));
  m_add->setIcon(icons::themed("plus"));
  m_prompt = new QLabel(bar);
  m_prompt->setObjectName("secondary");
  m_cursor = new QLabel(bar);
  m_cursor->setFont(theme::mono(11));
  m_cursor->setObjectName("tertiary");
  m_info = new QLabel(bar);
  m_info->setObjectName("secondary");
  h->addWidget(m_tabs);
  h->addWidget(m_add);
  h->addSpacing(12);
  h->addWidget(m_prompt, 1);
  // Snaps: on or off, and which kinds (remembered).
  m_snap = new QToolButton(bar);
  m_snap->setObjectName("sheetSnap");
  m_snap->setText(tr("Snap"));
  m_snap->setCheckable(true);
  m_snap->setAutoRaise(true);
  m_snap->setPopupMode(QToolButton::MenuButtonPopup);
  m_snap->setToolTip(tr("Snap the pointer to ends, midpoints, centres, quadrants, crossings and lines of the drawing"));
  auto* kinds = new QMenu(m_snap);
  QSettings settings;
  const unsigned chosen = settings.value("drawings/snaps", opad::drawing::kAllSnaps).toUInt() & opad::drawing::kAllSnaps;
  for (int k = 0; k <= static_cast<int>(opad::drawing::SnapKind::Nearest); ++k) {
    const auto kind = static_cast<opad::drawing::SnapKind>(k);
    QAction* a = kinds->addAction(SheetCanvas::snapName(kind));
    a->setCheckable(true);
    a->setChecked(chosen & opad::drawing::snap_bit(kind));
    a->setData(opad::drawing::snap_bit(kind));
  }
  m_snap->setMenu(kinds);
  m_snap->setChecked(settings.value("drawings/snap", true).toBool());
  const auto applySnaps = [this, kinds] {
    unsigned bits = 0;
    for (QAction* a : kinds->actions())
      if (a->isChecked()) bits |= a->data().toUInt();
    QSettings s;
    s.setValue("drawings/snaps", bits);
    s.setValue("drawings/snap", m_snap->isChecked());
    m_canvas->setSnapKinds(m_snap->isChecked() ? bits : 0);
  };
  connect(m_snap, &QToolButton::toggled, this, applySnaps);
  connect(kinds, &QMenu::triggered, this, applySnaps);
  m_canvas->setSnapKinds(m_snap->isChecked() ? chosen : 0);
  h->addWidget(m_snap);
  // Annotations whose references are gone: their count, a menu to re-attach each.
  m_dangling = new QToolButton(bar);
  m_dangling->setObjectName("sheetDangling");
  m_dangling->setAutoRaise(true);
  m_dangling->setIcon(icons::themed("warning"));
  m_dangling->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
  m_dangling->setPopupMode(QToolButton::InstantPopup);
  m_dangling->setMenu(new QMenu(m_dangling));
  m_dangling->setToolTip(tr("Annotations that cannot find what they measure any more: shown in magenta with the value they were made with"));
  m_dangling->hide();
  connect(m_canvas, &SheetCanvas::danglingChanged, this, &SheetPage::updateDangling);
  h->addWidget(m_dangling);
  // The drawing's revision, and whether it changed since it was issued.
  m_issue = new QToolButton(bar);
  m_issue->setObjectName("sheetIssue");
  m_issue->setAutoRaise(true);
  m_issue->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
  m_issue->setPopupMode(QToolButton::MenuButtonPopup);
  m_issue->setMenu(new QMenu(m_issue));
  m_issue->hide();
  connect(m_issue, &QToolButton::clicked, this, &SheetPage::issueRequested);
  connect(m_canvas, &SheetCanvas::issueChanged, this, &SheetPage::updateIssue);
  h->addWidget(m_issue);
  h->addWidget(m_cursor);
  h->addWidget(m_info);
  bar->setFixedHeight(30);
  v->addWidget(bar);
  const auto paintBar = [bar] {
    QPalette pal = bar->palette();
    pal.setColor(QPalette::Window, theme::current().bg2);
    bar->setPalette(pal);
  };
  paintBar();
  connect(theme::notifier(), &theme::Notifier::changed, this, paintBar);
  connect(create, &QPushButton::clicked, this, &SheetPage::newDrawingRequested);
  connect(m_add, &QToolButton::clicked, this, &SheetPage::newSheetRequested);
  connect(m_tabs, &QTabBar::currentChanged, this, [this](int i) {
    if (m_filling || i < 0) return;
    showSheet(m_tabs->tabData(i).toString().toStdString());
  });
  connect(m_canvas, &SheetCanvas::promptChanged, m_prompt, &QLabel::setText);
  connect(m_canvas, &SheetCanvas::cursorMoved, this, [this](double x, double y, bool on, const QString& snap) {
    m_cursor->setText(on ? QString("x %1  y %2 mm").arg(x, 0, 'f', 2).arg(y, 0, 'f', 2) + (snap.isEmpty() ? QString() : " · " + snap) : QString());
  });
  documentChanged();
}

const std::string& SheetPage::sheet() const { return m_canvas->sheet(); }
bool SheetPage::empty() const { return m_stack->currentIndex() == 1; }

void SheetPage::showSheet(const std::string& id) {
  if (!m_doc->scene.sheet(id)) return;
  if (id != m_canvas->sheet()) m_annotator->cancel(), m_viewTool->cancel();  // their picks were on the other sheet
  m_canvas->setSheet(id);
  rebuildTabs();
  updateInfo();
  emit sheetShown(id);
}

void SheetPage::documentChanged() {
  const auto& sheets = m_doc->scene.sheets;
  const bool none = !m_doc->hasDocument || sheets.empty();
  m_stack->setCurrentIndex(none ? 1 : 0);
  if (none || !m_doc->scene.sheet(m_canvas->sheet())) m_annotator->cancel(), m_viewTool->cancel();
  if (!m_viewTool->view().empty() && !m_doc->scene.sheet_view(m_viewTool->view())) m_viewTool->cancel();  // its view was deleted (Ctrl+Z)
  if (none) {
    m_canvas->setSheet("");
  } else if (!m_doc->scene.sheet(m_canvas->sheet())) {
    m_canvas->setSheet(sheets.front().id);
    emit sheetShown(sheets.front().id);
  } else {
    m_canvas->refresh();
  }
  rebuildTabs();
  updateInfo();
}

void SheetPage::rebuildTabs() {
  m_filling = true;
  while (m_tabs->count()) m_tabs->removeTab(0);
  std::vector<const opad::Sheet*> ordered;  // drawing by drawing, in the order of their first sheet
  for (const auto& s : m_doc->scene.sheets)
    if (std::none_of(ordered.begin(), ordered.end(), [&](const opad::Sheet* o) { return o->drawing == s.drawing; }))
      for (const auto& t : m_doc->scene.sheets)
        if (t.drawing == s.drawing) ordered.push_back(&t);
  std::set<std::string> drawings;
  for (const auto* s : ordered) drawings.insert(s->drawing);
  for (const auto* s : ordered) {
    const QString name = QString::fromStdString(s->name);
    const int i = m_tabs->addTab(drawings.size() > 1 && !s->drawing.empty() ? QString::fromStdString(s->drawing) + " · " + name : name);
    m_tabs->setTabData(i, QString::fromStdString(s->id));
    if (s->id == m_canvas->sheet()) m_tabs->setCurrentIndex(i);
  }
  m_add->setVisible(!ordered.empty());
  m_filling = false;
}

void SheetPage::updateDangling() {
  const auto dangling = m_canvas->dangling();
  m_dangling->setVisible(!dangling.empty());
  m_dangling->setText(tr("%n dangling", nullptr, static_cast<int>(dangling.size())));
  QMenu* menu = m_dangling->menu();
  menu->clear();
  for (const auto& [id, why] : dangling) {
    const opad::SheetItem* t = m_doc->scene.sheet_item(id);
    if (!t) continue;
    const opad::json shown = t->def.value("result", opad::json::object()).value("shown", opad::json());
    const QString name = shown.is_string() ? QString::fromStdString(shown.get<std::string>()).section('\n', 0, 0) : QString::fromStdString(t->kind);
    QAction* a = menu->addAction(tr("Re-attach %1").arg(name), this, [this, id = id] { emit reattachRequested(id); });
    a->setToolTip(why);
  }
}

void SheetPage::updateIssue() {
  const opad::json& since = m_canvas->sinceIssue();
  m_issue->setVisible(since.is_object());
  if (!since.is_object()) return;
  const QString rev = QString::fromStdString(since.value("rev", "")), date = QString::fromStdString(since.value("date", ""));
  const int views = static_cast<int>(since.value("views", opad::json::array()).size()), values = static_cast<int>(since.value("values", opad::json::array()).size()),
            gone = static_cast<int>(since.value("gone", opad::json::array()).size());
  const bool changed = views || values || gone;
  m_issue->setIcon(icons::themed(changed ? "warning" : "issueRevision"));
  m_issue->setText(changed ? tr("Changed since rev %1").arg(rev) : tr("Rev %1").arg(rev));
  QStringList tip{changed ? tr("Revision %1 was issued on %2; since then:").arg(rev, date) : tr("Revision %1, issued on %2, is what the sheet shows.").arg(rev, date)};
  if (views) tip << tr("%n views draw differently", nullptr, views);
  if (values) tip << tr("%n annotations show other values", nullptr, values);
  if (gone) tip << tr("%n views or annotations are gone", nullptr, gone);
  tip << tr("Click to issue the next revision.");
  m_issue->setToolTip(tip.join('\n'));
  QMenu* menu = m_issue->menu();  // every revision, to export as it was issued
  menu->clear();
  menu->addAction(icons::themed("issueRevision", 16), tr("Issue the next revision…"), this, &SheetPage::issueRequested);
  menu->addSeparator();
  if (const opad::Sheet* s = m_doc->scene.sheet(m_canvas->sheet()))
    for (const opad::SheetItem* t : opad::drawing::drawing_issues(m_doc->scene, *s)) {
      const std::string r = t->def.value("rev", "");
      QAction* a = menu->addAction(tr("Export revision %1 as issued…").arg(QString::fromStdString(r)), this, [this, r] { emit exportIssueRequested(r); });
      a->setObjectName(QString::fromStdString("sheet.exportIssue." + r));
      a->setToolTip(tr("Its views as they were frozen when it was issued, with the values it was issued with"));
    }
}

void SheetPage::updateInfo() {
  const opad::Sheet* s = m_doc->scene.sheet(m_canvas->sheet());
  if (!s) return m_info->clear();
  const opad::json size = s->def.value("size", opad::json::object());
  const QString paper = size.contains("preset") ? QString::fromStdString(size["preset"].get<std::string>()) : QString("%1 × %2").arg(s->width).arg(s->height);
  m_info->setText(tr("%1 · scale %2 · %3").arg(paper, QString::fromStdString(opad::drawing::scale_text(s->scale)),
                                               s->projection == "third" ? tr("third angle") : tr("first angle")));
}
