// Lock (UI-37): a locked body or component, and what is under it, is faded in the view and never selected there (a click
// passes it, a band leaves it out, the mouse does not light it up), while it stays a reference: guided tools and feature
// inputs pick it, snaps find it, sketch Project takes its edges (the Lock layer of looks, LookDelta::reference); the core
// refuses to change, move or remove it (refuse_locked, plan_ops). Lock / Unlock is one step for the selection
// (design.lock, labelled as the selection is; Unlock frees what holds the lock, the component above a body too); the
// browser's lock badge unlocks with a click (dim on what a locked component holds, naming it); resting the mouse on a
// locked body names it in the status bar and the right-click menu there offers Unlock <name>. A drawing's locked layer
// (DXF) is locked the same way, its lines faded towards the background (line aspects ignore alpha).
#include <QAction>
#include <QEvent>
#include <QMenu>
#include <QMouseEvent>
#include <QTimer>

#include <algorithm>
#include <map>
#include <optional>
#include <utility>

#include "AreaController.hpp"
#include "BodyLook.hpp"
#include "BrowserPanel.hpp"
#include "Commands.hpp"
#include "DesignController.hpp"
#include "Icons.hpp"
#include "Theme.hpp"
#include "Viewport.hpp"

OPAD_ICON_TABLE(lock, {"unlock", R"(<rect x="5" y="11" width="14" height="10"/><path d="M8 11V7a4 4 0 0 1 7.7-1.5"/>)"});

namespace {
constexpr double kFade = 0.5;  // a locked body's opacity, times its own
}  // namespace

class Lock : public AreaController {
 public:
  using AreaController::AreaController;

  void buildActions() override {
    CommandInfo info{"design.lock", tr("Lock"), "lock"};
    info.keywords = {"unlock", "protect", "freeze", "pin"};
    info.enabledWhen = [](const CommandContext& c) { return c.document; };  // nothing selected: says what to select and waits for it (UI-109)
    m_lock = services().addCommand(info, [this] { toggle(nodes(services().selection())); });
    m_lock->setProperty("shortcutHint", tr("Locked objects are faded and not selected in the view, and edits leave them alone; they are still measured and snapped to."));
    shortcuts::updateTooltip(m_lock);
  }

  void ready() override {
    services().browser()->addDecorator([this](const browser::Row& row, browser::Decoration& d) { decorate(row, d); });
    services().viewport()->installEventFilter(this);  // a locked body's hover hint and right-click
    m_hoverTimer.setSingleShot(true);
    m_hoverTimer.setInterval(150);  // once the mouse rests: one pick of the navigation selector, not one per move
    connect(&m_hoverTimer, &QTimer::timeout, this, &Lock::hoverHint);
    connect(theme::notifier(), &theme::Notifier::changed, this, &Lock::refresh);  // drawings fade towards the background
    refresh();
  }

  void contextMenu(const SelectionContext& selection, QMenu& menu) override {
    const std::optional<QPointF> at = std::exchange(m_menuAt, std::nullopt);  // a right-click in the view, there
    AppDocument* doc = services().document();
    if (!at || selection.sketching || !doc->hasDocument || !m_any) return;
    const std::string body = services().viewport()->drawnAt(*at);
    const opad::Node* holder = holderOf(body);
    const auto& ids = selection.ids;
    if (!holder || std::find(ids.begin(), ids.end(), body) != ids.end() || std::find(ids.begin(), ids.end(), holder->id) != ids.end()) return;
    menu.addSeparator();
    QAction* a = menu.addAction(icons::themed("unlock", 16), tr("Unlock %1").arg(doc->nodeName(holder->id)));
    a->setObjectName("lockUnlockAt");
    connect(a, &QAction::triggered, this, [this, body] { services().guarded([&] { unlock({body}); }); });
  }

  void selectionChanged(const SelectionContext& selection) override { relabel(selection); }
  void documentChanged(bool) override { refresh(); }

 protected:
  bool eventFilter(QObject* object, QEvent* event) override {
    if (object != services().viewport()) return AreaController::eventFilter(object, event);
    const auto* e = static_cast<const QMouseEvent*>(event);  // for the mouse events below
    switch (event->type()) {
      case QEvent::MouseMove:
        if (e->buttons() == Qt::NoButton && (m_any || !m_hint.isEmpty())) {
          m_hoverAt = e->position();
          m_hoverTimer.start();
        }
        break;
      case QEvent::Leave:
        m_hoverTimer.stop();
        showHint({});
        break;
      case QEvent::MouseButtonPress:
        if (e->button() == Qt::RightButton) m_rightAt = e->position();
        break;
      case QEvent::MouseButtonRelease:
        if (e->button() == Qt::RightButton && m_any && (e->position() - m_rightAt).manhattanLength() < 4) {
          m_menuAt = e->position();  // the menu this release opens asks for it; gone if it opens none
          QTimer::singleShot(0, this, [this] { m_menuAt.reset(); });
        }
        break;
      default:
        break;
    }
    return false;
  }

 private:
  std::vector<std::string> nodes(const SelectionContext& selection) const {  // the selection's bodies and components
    std::vector<std::string> out;
    for (const auto& id : selection.ids)
      if (services().document()->scene.node(id)) out.push_back(id);
    return out;
  }

  // What holds a node's lock: the node, or the nearest locked component above it; null when it is free.
  const opad::Node* holderOf(const std::string& id) const {
    const opad::Scene& scene = services().document()->scene;
    for (const opad::Node* n = scene.node(id); n; n = n->parent.empty() ? nullptr : scene.node(n->parent))
      if (n->locked) return n;
    return nullptr;
  }

  bool allLocked(const std::vector<std::string>& ids) const {
    return !ids.empty() && std::all_of(ids.begin(), ids.end(), [this](const std::string& id) { return holderOf(id) != nullptr; });
  }

  // Locks what is free of the selection, or, when all of it is locked, unlocks it: one step either way.
  void toggle(const std::vector<std::string>& ids) {
    if (ids.empty()) throw opad::UserHint("Select the objects to lock or unlock first.", true);  // waits for the selection (UI-109)
    if (allLocked(ids)) return unlock(ids);
    std::vector<std::string> free;
    for (const auto& id : ids)
      if (!holderOf(id)) free.push_back(id);
    services().document()->run("appearance", opad::json{{"targets", free}, {"locked", true}});
  }

  // Frees them: every lock on their way up (a body under a locked component frees the component, which says so).
  void unlock(const std::vector<std::string>& ids) {
    AppDocument* doc = services().document();
    std::vector<std::string> holders;
    QStringList others;
    for (const auto& id : ids)
      for (const opad::Node* n = doc->scene.node(id); n; n = n->parent.empty() ? nullptr : doc->scene.node(n->parent))
        if (n->locked && std::find(holders.begin(), holders.end(), n->id) == holders.end()) {
          holders.push_back(n->id);
          if (std::find(ids.begin(), ids.end(), n->id) == ids.end()) others << doc->nodeName(n->id);
        }
    if (holders.empty()) return;
    doc->run("appearance", opad::json{{"targets", holders}, {"locked", false}});
    if (!others.isEmpty()) services().showMessage(tr("Unlocked %1 too: the lock was there").arg(others.join(", ")), 6000);
  }

  void relabel(const SelectionContext& selection) {
    const bool unlock = allLocked(nodes(selection));
    const QString icon = unlock ? "unlock" : "lock";
    if (m_lock->data().toString() == icon && !m_lock->text().isEmpty()) return;
    m_lock->setText(unlock ? tr("Unlock") : tr("Lock"));
    m_lock->setData(icon);
    m_lock->setIcon(icons::themed(icon));
    shortcuts::updateTooltip(m_lock);
  }

  void decorate(const browser::Row& row, browser::Decoration& d) {
    const opad::Node* holder = m_any && row.node ? holderOf(row.id) : nullptr;
    if (!holder) return;
    const bool own = holder->id == row.id;
    const QString name = services().document()->nodeName(holder->id);
    browser::Badge badge;
    badge.icon = "lock";
    badge.fill = nullptr;
    badge.color = own ? &Tokens::locked : &Tokens::fg3;
    badge.tooltip = own ? tr("Locked: faded and not selected in the view, left alone by edits, still measured and snapped to. Click to unlock.")
                        : tr("Locked with %1. Click to unlock %1.").arg(name);
    badge.clicked = [this, id = holder->id] { services().guarded([&] { unlock({id}); }); };
    d.badges.push_back(badge);
  }

  // The Lock layer of looks: every locked node faded and a reference only (its entry covers what is under it); a
  // drawing's lines, whose alpha is ignored, towards the background instead.
  void refresh() {
    const opad::Scene& scene = services().document()->scene;
    const QColor bg = theme::current().vp;
    std::map<std::string, LookDelta> layer;
    for (const auto& [id, n] : scene.nodes) {
      if (!n.locked) continue;
      LookDelta lock;
      lock.fade = kFade;
      lock.reference = true;
      layer[id] = lock;
      for (const auto& body : scene.bodies_under(id))
        if (const opad::Node* b = scene.node(body); b && b->representation == "drawing2d") {
          LookDelta lines = lock;
          lines.color = looks::mix(b->color, {bg.redF(), bg.greenF(), bg.blueF()}, 1 - kFade);
          layer[body] = lines;
        }
    }
    m_any = !layer.empty();
    if (!m_any) showHint({});
    services().viewport()->setLookLayer(LookSource::Lock, std::move(layer));
    relabel(services().selection());
  }

  // Resting on a locked body (not picked, so the viewport does not name it): its name and the way to unlock it.
  void hoverHint() {
    Viewport* view = services().viewport();
    DesignController* design = services().design();
    const bool idle = m_any && !view->ghostsPickable() && !design->sketchActive() && !design->featureActive();
    const std::string id = idle ? view->drawnAt(m_hoverAt) : std::string();
    const opad::Node* holder = id.empty() || view->shownLook(id).ghost ? nullptr : holderOf(id);  // a ghost is the activation's
    const AppDocument* doc = services().document();
    showHint(!holder ? QString()
             : holder->id == id ? tr("%1 is locked · right-click to unlock").arg(doc->nodeName(id))
                                : tr("%1 is locked with %2 · right-click to unlock").arg(doc->nodeName(id), doc->nodeName(holder->id)));
  }

  void showHint(const QString& text) {  // in the status bar's hover text, over the viewport's own
    if (text == m_hint) return;
    m_hint = text;
    Viewport* view = services().viewport();
    emit view->hoverChanged(text.isEmpty() ? view->hoverText() : text);
  }

  QAction* m_lock = nullptr;
  bool m_any = false;  // something is locked
  QTimer m_hoverTimer;
  QPointF m_hoverAt, m_rightAt;
  QString m_hint;
  std::optional<QPointF> m_menuAt;
};

OPAD_AREA(Lock)
