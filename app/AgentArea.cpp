// The AI assistant's place in the window (AreaController.hpp), around the bridge MainWindow makes (AgentBridge.hpp):
//   - the status bar's AI chip, in the row beside the document's path (AreaServices::addStatusChip), after the areas'
//     own chips (it is added in ready(): the bridge is made after the status bar's hooks ran);
//   - the viewport's preview chip at the end of the top-left chips row while an AI preview or task is staged: the AI mark,
//     "Claude's preview · not applied" and Discard (the same discard as the panel's group row); its tooltip lists the
//     staged steps in words;
//   - "Show in AI activity" on the timeline marker of a step an AI assistant made, while its row is in the feed;
//   - the selection published for assistants only while AI access is on (UI-06): at once when it comes on, the file
//     removed when it goes off.
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QMainWindow>
#include <QMenu>
#include <QToolButton>
#include <utility>

#include "AgentBridge.hpp"
#include "AreaController.hpp"
#include "Icons.hpp"
#include "Theme.hpp"
#include "ViewportChips.hpp"

namespace {

class AgentArea : public AreaController {
 public:
  using AreaController::AreaController;

  void ready() override {
    m_agent = services().window()->findChild<AgentBridge*>();
    if (!m_agent) return;
    services().addStatusChip(m_agent->chip());
    buildPreviewChip();
    m_publishing = m_agent->publishesSelection();
    connect(m_agent, &AgentBridge::statusChanged, this, [this] {
      refreshPreviewChip();
      if (std::exchange(m_publishing, m_agent->publishesSelection()) != m_publishing) services().publishSelection(m_publishing);
    });
    refreshPreviewChip();
  }

  void timelineMenu(const std::string& opId, QMenu& menu) override {
    const opad::Op* op = services().document()->doc.find_op(opId);
    if (!m_agent || !op || op->data.value("by", std::string()) != "Agent") return;
    const quint64 row = m_agent->rowForOp(opId);
    menu.addSeparator();
    QAction* a = menu.addAction(icons::themed(QStringLiteral("aiMark"), 16), tr("Show in AI activity"));
    a->setObjectName(QStringLiteral("timelineAiActivity"));
    a->setEnabled(row != 0);  // a step from an earlier session, or a row cleared: nothing to show
    connect(a, &QAction::triggered, this, [this, row] {
      if (m_agent) m_agent->revealRow(row);
    });
  }

 private:
  void buildPreviewChip() {
    ViewportChips* chips = services().chips();
    if (!chips) return;
    m_chip = new QFrame;
    m_chip->setObjectName(QStringLiteral("aiPreviewChip"));
    auto* row = new QHBoxLayout(m_chip);
    row->setContentsMargins(6, 1, 2, 1);
    row->setSpacing(5);
    m_mark = new QLabel(m_chip);
    m_text = new QLabel(m_chip);
    m_text->setObjectName(QStringLiteral("aiPreviewText"));
    m_discard = new QToolButton(m_chip);
    m_discard->setObjectName(QStringLiteral("chipAction"));
    m_discard->setText(tr("Discard"));
    m_discard->setToolTip(tr("Drop the preview. Your design doesn't change."));
    m_discard->setCursor(Qt::PointingHandCursor);
    m_discard->setFocusPolicy(Qt::NoFocus);
    connect(m_discard, &QToolButton::clicked, this, [this] {
      if (!m_agent) return;
      const auto staged = m_agent->staged();
      if (staged.row) m_agent->rowAction(staged.row, QStringLiteral("discard"));
    });
    row->addWidget(m_mark);
    row->addWidget(m_text);
    row->addWidget(m_discard);
    m_chip->hide();
    chips->addChip(m_chip);
    restyle();
    connect(theme::notifier(), &theme::Notifier::changed, m_chip, [this] { restyle(); });
  }

  void restyle() {
    if (!m_chip) return;
    const Tokens& t = theme::current();
    m_chip->setStyleSheet(QStringLiteral("QFrame#aiPreviewChip { background: %1; border: 1px solid %2; border-radius: 3px; }"
                                         "QLabel#aiPreviewText { background: transparent; border: none; font-size: 11px; color: %3; }")
                              .arg(t.bg2.name(), t.hov.name(), t.fg.name()));
    m_mark->setPixmap(icons::pixmap(QStringLiteral("aiMark"), t.hov, 14, m_chip->devicePixelRatioF()));
  }

  void refreshPreviewChip() {
    if (!m_chip || !m_agent) return;
    const auto staged = m_agent->staged();
    if (!staged.row) {
      m_chip->hide();
      return;
    }
    const QString who = staged.client.isEmpty() ? tr("AI assistant") : staged.client;
    m_text->setText(tr("%1's preview · not applied").arg(who));
    QStringList tip{tr("Not applied yet. Your design hasn't changed.")};
    if (staged.steps.isEmpty()) tip << tr("No steps yet.");
    for (const QString& step : staged.steps) tip << QStringLiteral("• ") + step;
    m_chip->setToolTip(tip.join(QLatin1Char('\n')));
    m_chip->setAccessibleName(m_text->text());
    m_chip->show();
  }

  AgentBridge* m_agent = nullptr;
  QFrame* m_chip = nullptr;
  QLabel* m_mark = nullptr;
  QLabel* m_text = nullptr;
  QToolButton* m_discard = nullptr;
  bool m_publishing = false;
};

}  // namespace

OPAD_AREA(AgentArea)
