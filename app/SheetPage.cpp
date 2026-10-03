#include "SheetPage.hpp"

#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QStackedWidget>
#include <QTabBar>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <set>

#include "AppDocument.hpp"
#include "Icons.hpp"
#include "SheetCanvas.hpp"
#include "Theme.hpp"

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
  connect(m_canvas, &SheetCanvas::cursorMoved, this, [this](double x, double y, bool on) {
    m_cursor->setText(on ? QString("x %1  y %2 mm").arg(x, 0, 'f', 1).arg(y, 0, 'f', 1) : QString());
  });
  documentChanged();
}

const std::string& SheetPage::sheet() const { return m_canvas->sheet(); }
bool SheetPage::empty() const { return m_stack->currentIndex() == 1; }

void SheetPage::showSheet(const std::string& id) {
  if (!m_doc->scene.sheet(id)) return;
  m_canvas->setSheet(id);
  rebuildTabs();
  updateInfo();
  emit sheetShown(id);
}

void SheetPage::documentChanged() {
  const auto& sheets = m_doc->scene.sheets;
  const bool none = !m_doc->hasDocument || sheets.empty();
  m_stack->setCurrentIndex(none ? 1 : 0);
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

void SheetPage::updateInfo() {
  const opad::Sheet* s = m_doc->scene.sheet(m_canvas->sheet());
  if (!s) return m_info->clear();
  const opad::json size = s->def.value("size", opad::json::object());
  const QString paper = size.contains("preset") ? QString::fromStdString(size["preset"].get<std::string>()) : QString("%1 × %2").arg(s->width).arg(s->height);
  m_info->setText(tr("%1 · scale %2 · %3").arg(paper, QString::fromStdString(opad::drawing::scale_text(s->scale)),
                                               s->projection == "third" ? tr("third angle") : tr("first angle")));
}
