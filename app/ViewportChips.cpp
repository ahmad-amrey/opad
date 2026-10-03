#include "ViewportChips.hpp"

#include <QEvent>
#include <QHBoxLayout>
#include <QPalette>

#include <initializer_list>

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
  m_twoD->setToolTip(tr("2D mode is on: the view is locked to a plane and does not orbit. Click, or press Shift+2, to turn it off."));
  m_twoD->setCursor(Qt::PointingHandCursor);
  m_twoD->installEventFilter(this);
  m_twoD->hide();
  m_section = new QLabel(this);
  m_section->setObjectName("chipSel");
  m_isolate = new QLabel(this);
  m_isolate->setObjectName("chipSel");
  // Viewer mode: what is shown, and saving it to edit.
  m_viewer = new QLabel(tr("Viewer · read-only"), this);
  m_viewer->setObjectName("chipSel");
  m_saveToEdit = new QToolButton(this);
  m_saveToEdit->setObjectName("chipAction");
  m_saveToEdit->setText(tr("Save to edit"));
  m_saveToEdit->setToolTip(tr("Save as an OPAD document, which can be edited (Ctrl+S). The file you opened is not changed."));
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
}

bool ViewportChips::eventFilter(QObject* object, QEvent* event) {
  if (object == m_twoD && event->type() == QEvent::MouseButtonRelease) {
    emit leaveTwoDimensional();
    return true;
  }
  return QWidget::eventFilter(object, event);
}

void ViewportChips::setViewer(const QString& file) {
  const bool on = !file.isEmpty();
  m_viewer->setVisible(on);
  m_viewer->setToolTip(on ? tr("%1 is shown read-only: measure, section, hide and colour freely. Editing needs it saved as an OPAD document.").arg(file) : QString());
  m_saveToEdit->setVisible(on);
  adjustSize();
}

void ViewportChips::set(const QString& mode, const QString& projection, const QString& section, const QString& isolate, bool twoDimensional) {
  m_mode->setText(mode);
  m_proj->setText(projection);
  m_twoD->setVisible(twoDimensional);
  m_section->setText(section);
  m_section->setVisible(!section.isEmpty());
  m_isolate->setText(isolate);
  m_isolate->setVisible(!isolate.isEmpty());
  adjustSize();
}
