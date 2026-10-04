// Docks and floating tool panels: the browser overlay, Properties, Annotations, Section, the tool panel, the timeline strip.
#include "MainWindow.hpp"
#include "CheckPanel.hpp"

#include <QDockWidget>
#include <QStackedWidget>
#include <QVBoxLayout>

#include <algorithm>

void MainWindow::buildDocks() {
  // The timeline strip runs under the browser and the viewport. There is no right dock: properties,
  // annotations and section are floating tool panels over the viewport.
  setCorner(Qt::BottomLeftCorner, Qt::BottomDockWidgetArea);
  setCorner(Qt::BottomRightCorner, Qt::RightDockWidgetArea);
  setCorner(Qt::TopLeftCorner, Qt::LeftDockWidgetArea);
  setCorner(Qt::TopRightCorner, Qt::RightDockWidgetArea);

  m_browser = new BrowserPanel(m_doc, this);
  m_browserOverlay = new BrowserOverlay(m_browser, m_viewport);
  connect(m_browser,&BrowserPanel::autoHideChanged,m_browserOverlay,[this](bool on){m_browserOverlay->setAutoHide(on);});
  connect(m_doc,&AppDocument::changed,m_browserOverlay,[this] { m_browserOverlay->refresh(); });
  m_browserOverlay->place();


  m_props = new PropertiesPanel(this);
  m_annotations = new AnnotationsPanel(m_doc, this);
  m_section = new SectionPanel(m_doc, this);
  m_propsPanel = new ToolPanel("properties", "body", &Tokens::fg2, tr("Properties"), m_props, 420, this);
  m_annotationsPanel = new ToolPanel("annotations", "annotate", &Tokens::amber, tr("Annotations"), m_annotations, 520, this);
  m_sectionPanel = new ToolPanel("section", "section", &Tokens::sel, tr("Section"), m_section, 420, this);
  m_toolSteps = new ToolStepsPanel(this);
  m_checks = new CheckPanel(this);
  m_toolStack = new QStackedWidget(this);
  m_toolStack->addWidget(m_toolSteps);
  m_toolStack->addWidget(m_checks);
  m_toolPanel = new ToolPanel("tool", "distance", &Tokens::sel, tr("Distance"), m_toolStack, 360, this);
  m_toolPanel->setContentSizeHint([this](int width) { return m_toolStack->currentWidget() == m_checks ? m_checks->preferredSize(width) : m_toolSteps->preferredSize(width); });
  connect(m_checks, &CheckPanel::runRequested, this, &MainWindow::runCheck);
  connect(m_checks, &CheckPanel::findingActivated, this, &MainWindow::showFinding);
  m_checks->setKeepable(true);
  connect(m_checks, &CheckPanel::keepRequested, this, &MainWindow::keepCheck);
  connect(m_toolSteps, &ToolStepsPanel::contentSizeChanged, m_toolPanel, &ToolPanel::requestContentFit);
  connect(m_checks, &CheckPanel::contentResized, m_toolPanel, &ToolPanel::requestContentFit);
  m_panels = {m_propsPanel, m_annotationsPanel, m_sectionPanel, m_toolPanel};
  // The note / hand drawing editor's panel: filled by each AnnotationEditor, open exactly as long as it runs. Not one
  // of m_panels, so opening another panel never ends an annotation in progress.
  auto* annotationHost = new QWidget(this);
  new QVBoxLayout(annotationHost);
  annotationHost->layout()->setContentsMargins(0, 0, 0, 0);
  m_annotationPanel = new ToolPanel("annotation", "annotate", &Tokens::amber, tr("Note"), annotationHost, 420, this);
  m_annotationPanel->setPinnable(false);
  m_annotationPanel->setEscapeHandler([this] { if (m_annotationEditor) m_annotationEditor->cancel(); else m_annotationPanel->hide(); });
  connect(m_annotationPanel, &ToolPanel::visibilityChanged, this, [this](bool on) {
    // Its close button. Minimising the window hides it too, spontaneously: isVisible() stays true then.
    if (!on && m_annotationEditor && !m_annotationPanel->isVisible()) m_annotationEditor->cancel();
    syncAnnotationActions();
  });
  connect(m_propsPanel, &ToolPanel::visibilityChanged, this, [this](bool on) {
    if (!on && m_propsJob) m_propsJob->cancel();  // nobody is looking at the component bbox any more
  });

  m_timeline = new TimelineWidget(m_doc, this);
  auto* bottom = m_timelineDock = new QDockWidget(tr("Timeline"), this);
  bottom->setObjectName("dock.timeline");
  bottom->setTitleBarWidget(new QWidget(bottom));  // the strip is its own header
  bottom->setFeatures(QDockWidget::NoDockWidgetFeatures);
  bottom->setWidget(m_timeline);
  bottom->setFixedHeight(m_timeline->height());  // 48 px, more at a larger text size
  connect(theme::notifier(), &theme::Notifier::changed, bottom, [this, bottom] { bottom->setFixedHeight(m_timeline->height()); });
  addDockWidget(Qt::BottomDockWidgetArea, bottom);


  m_props->clear();

  // Bind the panel actions to the docks' own toggle actions (both directions).
  action("panel.browser")->setChecked(true);
  connect(action("panel.browser"), &QAction::triggered, this, [this](bool on) {
    m_browserOverlay->setVisible(on);
    if (on) m_browserOverlay->reveal();
  });
  bindPanel(action("panel.annotations"), m_annotationsPanel);
  bindPanel(action("panel.section"), m_sectionPanel);
  bindPanel(action("panel.timeline"), bottom);
}

void MainWindow::bindPanel(QAction* a, QDockWidget* dock) {
  if (!a || !dock) return;
  QAction* native = dock->toggleViewAction();
  a->setChecked(native->isChecked());
  // The dock's own action only reacts to being triggered, not to setChecked, so drive the dock directly.
  connect(native, &QAction::toggled, a, [a](bool on) { if (a->isChecked() != on) a->setChecked(on); });  // closed via its X
  connect(a, &QAction::triggered, dock, [dock](bool on) {
    dock->setVisible(on);
    if (on) dock->raise();
  });
}

void MainWindow::bindPanel(QAction* a, ToolPanel* panel) {
  connect(panel, &ToolPanel::visibilityChanged, a, [a](bool on) { if (a->isChecked() != on) a->setChecked(on); });
  connect(a, &QAction::triggered, panel, [this, panel](bool on) {
    if (on) openPanel(panel);
    else panel->hide();
  });
}

void MainWindow::openPanel(ToolPanel* panel) {
  int top = 186;  // below the view cube; pinned panels already there push it down, 8 px apart
  for (ToolPanel* o : m_panels) {
    if (o == panel || !o->isVisible()) continue;
    if (!o->pinned()) o->hide();
    else if (!o->userPlaced()) top = std::max(top, o->bottom() + 8);
  }
  if (!panel->isVisible()) panel->setDefaultTop(top);
  panel->anchorTo(QRect(m_viewport->mapToGlobal(QPoint(0, 0)), m_viewport->size()));
  panel->show();
  panel->raise();
}

bool MainWindow::closeTopPanel() {
  for (ToolPanel* p : m_panels)
    if (p->isVisible() && !p->pinned()) {
      p->hide();
      return true;
    }
  return false;
}

void MainWindow::resetLayout() {
  for (QDockWidget* d : {m_timelineDock}) {
    if (!d) continue;
    d->setFloating(false);
    d->show();
  }
  // The browser comes back with its command ticked again (Browser stayed unticked, so a document opened next hid it once
  // more); without a document it shows when one opens, as at the start.
  action("panel.browser")->setChecked(true);
  m_browserOverlay->setVisible(m_doc->hasDocument);
  m_browserOverlay->place();
  addDockWidget(Qt::BottomDockWidgetArea, m_timelineDock);
  setCorner(Qt::BottomLeftCorner, Qt::BottomDockWidgetArea);
  setCorner(Qt::BottomRightCorner, Qt::RightDockWidgetArea);
  setCorner(Qt::TopLeftCorner, Qt::LeftDockWidgetArea);
  setCorner(Qt::TopRightCorner, Qt::RightDockWidgetArea);
  // Every floating tool panel too (TODO 11 help audit P9.6): the window's own, the editors' and the areas'.
  for (ToolPanel* panel : findChildren<ToolPanel*>()) panel->restoreDefaultPlace();
  positionOverlays();
}
