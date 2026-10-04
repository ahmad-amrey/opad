#include "ViewportChips.hpp"

#include <QEvent>
#include <QHBoxLayout>
#include <QMouseEvent>
#include <QPalette>

#include <initializer_list>

#include "CommandHelp.hpp"
#include "KeyText.hpp"
#include "Theme.hpp"

// ---------------------------------------------------------------- ViewportChips
ViewportChips::ViewportChips(QWidget* parent) : QWidget(parent) {
  // Not transparent for the mouse: the row is opaque anyway, and its cards are buttons (2D mode, viewer mode).
  setObjectName("chipsHost");
  setAutoFillBackground(true);
  auto paintHost = [this] {
    QPalette pal = palette();
    pal.setColor(QPalette::Window, theme::current().vp);
    setPalette(pal);
  };
  paintHost();
  connect(theme::notifier(), &theme::Notifier::changed, this, paintHost);
  auto* l = new QHBoxLayout(this);
  l->setContentsMargins(8, 8, 8, 8);
  l->setSpacing(6);
  m_mode = new QLabel(this);
  m_mode->setObjectName("chip");
  m_proj = new QLabel(this);
  m_proj->setObjectName("chip");
  // 2D mode is a mode the view stays in (no orbit, locked projection): shown as its own card while on.
  m_twoD = new QLabel(tr("2D mode"), this);
  m_twoD->setObjectName("chipSel");
  m_twoD->setCursor(Qt::PointingHandCursor);
  m_twoD->installEventFilter(this);
  m_twoD->hide();
  m_section = new QLabel(this);
  m_section->setObjectName("chipSel");
  m_isolate = new QLabel(this);
  m_isolate->setObjectName("chipSel");
  // Isolation is a mode the view stays in too: the card ends it, its × says so (Exit isolate does the same).
  m_isolate->setToolTip(tr("Only the isolated bodies are shown. Click to show everything again (Exit isolate)."));
  m_isolate->setCursor(Qt::PointingHandCursor);
  m_isolate->installEventFilter(this);
  // Viewer mode: what is shown, and saving it to edit.
  m_viewer = new QLabel(tr("Viewer · read-only"), this);
  m_viewer->setObjectName("chipSel");
  m_saveToEdit = new QToolButton(this);
  m_saveToEdit->setObjectName("chipAction");
  m_saveToEdit->setText(tr("Save to edit"));
  m_saveToEdit->setCursor(Qt::PointingHandCursor);
  m_saveToEdit->setFocusPolicy(Qt::NoFocus);
  connect(m_saveToEdit, &QToolButton::clicked, this, &ViewportChips::saveToEditRequested);
  for (QWidget* w : std::initializer_list<QWidget*>{m_viewer, m_saveToEdit}) {
    l->addWidget(w);
    w->hide();
  }
  l->addWidget(m_mode);
  l->addWidget(m_proj);
  l->addWidget(m_twoD);
  l->addWidget(m_section);
  l->addWidget(m_isolate);
  l->addStretch();
  keyTexts();
  connect(keys::notifier(), &keys::Notifier::changed, this, &ViewportChips::keyTexts);
}

void ViewportChips::keyTexts() {
  m_twoD->setToolTip(help::expand(tr("2D mode is on: the view is locked to a plane and does not orbit. Click to turn it off, or use 2D mode ({key:view.2d}).")));
  m_saveToEdit->setToolTip(help::expand(m_document ? tr("Save a copy of the document, which can be edited ({key:file.save}). The file you opened is not changed.")
                                                   : tr("Save as an OPAD document, which can be edited ({key:file.save}). The file you opened is not changed.")));
}

bool ViewportChips::eventFilter(QObject* object, QEvent* event) {
  if (object == m_twoD && event->type() == QEvent::MouseButtonRelease) {
    emit leaveTwoDimensional();
    return true;
  }
  if (object == m_isolate && event->type() == QEvent::MouseButtonRelease && static_cast<QMouseEvent*>(event)->button() == Qt::LeftButton) {
    emit exitIsolation();
    return true;
  }
  return QWidget::eventFilter(object, event);
}

void ViewportChips::setViewer(const QString& file, bool document) {
  const bool on = !file.isEmpty();
  m_viewer->setVisible(on);
  m_viewer->setText(document ? tr("Read-only") : tr("Viewer · read-only"));
  m_viewer->setToolTip(!on ? QString()
                       : document ? tr("%1 is open read-only: measure, section, hide and colour freely; the file is not changed. Editing needs a copy.").arg(file)
                                  : tr("%1 is shown read-only: measure, section, hide and colour freely. Editing needs it saved as an OPAD document.").arg(file));
  m_saveToEdit->setText(document ? tr("Save a copy to edit") : tr("Save to edit"));
  m_document = document;
  keyTexts();
  m_saveToEdit->setVisible(on);
  adjustSize();
}

void ViewportChips::setDisplayChips(bool shown) {
  m_mode->setVisible(shown);
  m_proj->setVisible(shown);
  adjustSize();
}

void ViewportChips::set(const QString& mode, const QString& projection, const QString& section, const QString& isolate, bool twoDimensional) {
  m_mode->setText(mode);
  m_proj->setText(projection);
  m_twoD->setVisible(twoDimensional);
  m_section->setText(section);
  m_section->setVisible(!section.isEmpty());
  m_isolate->setText(isolate.isEmpty() ? QString() : isolate + QStringLiteral("  ×"));  // the close glyph: a click ends it
  m_isolate->setVisible(!isolate.isEmpty());
  adjustSize();
}

void ViewportChips::addChip(QWidget* chip) {
  chip->setParent(this);
  auto* row = static_cast<QHBoxLayout*>(layout());
  row->insertWidget(row->count() - 1, chip);  // before the stretch
  m_areaChips = true;
  adjustSize();
}

// set() and setViewer() fit the row to the built-in chips. An area's chip changes on its own (text, shown, hidden), and
// each change posts a layout request to the row.
bool ViewportChips::event(QEvent* event) {
  const bool done = QWidget::event(event);
  if (event->type() == QEvent::LayoutRequest && m_areaChips) adjustSize();
  return done;
}
