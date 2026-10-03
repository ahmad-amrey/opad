#include "DockHeader.hpp"

#include <QDockWidget>
#include <QHBoxLayout>
#include <QToolButton>

#include "Icons.hpp"
#include "Theme.hpp"

// ---------------------------------------------------------------- DockHeader
DockHeader::DockHeader(const QString& title, QDockWidget* dock) : QWidget(dock) {
  setObjectName("dockHeader");
  setAttribute(Qt::WA_StyledBackground);
  setFixedHeight(28);
  auto* l = new QHBoxLayout(this);
  l->setContentsMargins(8, 0, 6, 0);
  l->setSpacing(8);
  m_title = new QLabel(title, this);
  m_title->setObjectName("dockTitle");
  l->addWidget(m_title, 1);
  const Tokens& t = theme::current();
  auto* flt = new QToolButton(this);
  flt->setObjectName("dockButton");
  flt->setIcon(icons::icon("float", t.fg2));
  flt->setIconSize(QSize(16, 16));
  flt->setFixedSize(20, 20);
  flt->setToolTip(tr("Float / dock"));
  auto* close = new QToolButton(this);
  close->setObjectName("dockButton");
  close->setIcon(icons::icon("close", t.fg2));
  close->setIconSize(QSize(16, 16));
  close->setFixedSize(20, 20);
  close->setToolTip(tr("Close panel"));
  l->addWidget(flt);
  l->addWidget(close);
  connect(flt, &QToolButton::clicked, dock, [dock] { dock->setFloating(!dock->isFloating()); });
  connect(close, &QToolButton::clicked, dock, &QDockWidget::close);
  connect(theme::notifier(), &theme::Notifier::changed, this, [flt, close] {
    flt->setIcon(icons::icon("float", theme::current().fg2));
    close->setIcon(icons::icon("close", theme::current().fg2));
  });
}

void DockHeader::setTitle(const QString& t) { m_title->setText(t); }
