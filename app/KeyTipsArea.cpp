// KeyTips (UI-117, UI-124): the ribbon from the keyboard. KeyTips.hpp has the rules; this is the overlay: the badges, the
// levels and the keys, taken from the application before the window's one-key shortcuts see them.
#include <QAbstractButton>
#include <QApplication>
#include <QDateTime>
#include <QKeyEvent>
#include <QLabel>
#include <QMainWindow>
#include <QPointer>
#include <QTabBar>
#include <QToolButton>

#include <algorithm>
#include <functional>

#include "AreaController.hpp"
#include "KeyTips.hpp"
#include "Ribbon.hpp"
#include "Theme.hpp"

class KeyTipsArea : public AreaController {
 public:
  explicit KeyTipsArea(AreaServices& services) : AreaController(services) {}
  ~KeyTipsArea() override { clear(); }

  void ready() override {
    m_ribbon = services().window()->findChild<RibbonBar*>();
    qApp->installEventFilter(this);
  }

  // What shows now (benches): 0 none, 1 the tabs and the tab row's buttons, 2 the current tab's tools.
  int level() const { return m_level; }
  struct Tip {
    QString keys;
    QPointer<QWidget> target;
    std::function<void()> run;
    QPointer<QLabel> badge;
  };
  const QList<Tip>& tips() const { return m_tips; }

  void show(int level) {
    clear();
    if (!m_ribbon || !m_ribbon->isVisibleTo(services().window())) return;
    m_level = level;
    struct Target {
      QString label;
      QWidget* widget;
      QRect rect;  // in the widget
      std::function<void()> run;
    };
    QList<Target> targets, numbered;
    auto label = [](QAbstractButton* b) {
      QString text = b->text();
      if (auto* tool = qobject_cast<QToolButton*>(b); tool && tool->defaultAction()) text = tool->defaultAction()->text();
      return text.remove('&').trimmed().isEmpty() ? b->toolTip() : text;
    };
    auto click = [this](QAbstractButton* b) { return [this, b = QPointer<QAbstractButton>(b)] { clear(); if (b) b->click(); }; };
    if (level == 1) {
      QTabBar* tabs = m_ribbon->tabBar();
      for (int i = 0; i < tabs->count(); ++i)
        if (tabs->isTabVisible(i)) targets.push_back({tabs->tabText(i), tabs, tabs->tabRect(i), [this, tabs, i] { tabs->setCurrentIndex(i); show(2); }});
      if (auto* chip = m_ribbon->findChild<WorkspaceChip*>(); chip && chip->isVisibleTo(m_ribbon))
        targets.push_back({tr("Workspace"), chip, chip->rect(), click(chip)});
      QList<QAbstractButton*> row;  // the tab row's cluster: quick access, search, settings, the areas' buttons
      for (QAbstractButton* b : m_ribbon->cluster()->findChildren<QAbstractButton*>())
        if (b->isVisibleTo(m_ribbon) && b->isEnabled()) row << b;
      const bool rtl = m_ribbon->layoutDirection() == Qt::RightToLeft;
      std::sort(row.begin(), row.end(), [rtl](QAbstractButton* a, QAbstractButton* b) { return rtl ? a->x() > b->x() : a->x() < b->x(); });
      for (QAbstractButton* b : row) numbered.push_back({label(b), b, b->rect(), click(b)});
    } else if (RibbonPage* page = m_ribbon->currentPage()) {
      for (RibbonGroup* g : page->groups()) {
        if (g->level() == RibbonGroup::Collapsed) {
          if (g->collapsedButton()->isEnabled()) targets.push_back({label(g->collapsedButton()), g->collapsedButton(), g->collapsedButton()->rect(), click(g->collapsedButton())});
          continue;
        }
        for (QToolButton* b : g->buttons())
          if (b->isVisibleTo(g) && b->isEnabled()) targets.push_back({label(b), b, b->rect(), click(b)});
      }
      if (QToolButton* select = m_ribbon->selectButton(); select && select->isVisibleTo(m_ribbon)) targets.push_back({label(select), select, select->rect(), click(select)});
    }
    QSet<QString> digits;
    for (int i = 0; i < numbered.size() && i < 9; ++i) digits << QString::number(i + 1);
    QStringList labels;
    for (const Target& t : targets) labels << t.label;
    const QStringList keys = keytips::assign(labels, digits);
    for (int i = 0; i < targets.size(); ++i) add(keys[i], targets[i].widget, targets[i].rect, targets[i].run);
    for (int i = 0; i < numbered.size() && i < 9; ++i) add(QString::number(i + 1), numbered[i].widget, numbered[i].rect, numbered[i].run);
    services().showMessage(level == 1 ? tr("Key tips: press a tab's key, Esc to leave") : tr("Key tips: press a tool's key, Esc to go back"), 0);
  }

  void clear() {
    for (const Tip& t : std::exchange(m_tips, {}))
      if (t.badge) t.badge->deleteLater();
    if (m_level) services().showMessage(QString(), 1);
    m_level = 0;
    m_typed.clear();
  }

  // A key typed while the tips show: true when it was theirs.
  bool type(QKeyEvent* key) {
    if (key->key() == Qt::Key_Escape || key->key() == Qt::Key_Backspace) {
      if (!m_typed.isEmpty()) {
        m_typed.clear();
        filter();
      } else if (m_level == 2) {
        show(1);
      } else {
        clear();
      }
      return true;
    }
    const QString text = key->text().toUpper();
    if (text.size() != 1 || !text[0].isLetterOrNumber() || (key->modifiers() & ~(Qt::ShiftModifier | Qt::KeypadModifier))) {
      clear();  // arrows, Tab, Enter, a shortcut with Ctrl: the tips go and the key does what it does
      return false;
    }
    const QString typed = m_typed + text;
    if (std::none_of(m_tips.begin(), m_tips.end(), [&](const Tip& t) { return t.keys.startsWith(typed); })) return true;  // no such tip: nothing
    m_typed = typed;
    for (const Tip& t : m_tips)
      if (t.keys == m_typed) {
        const auto run = t.run;
        run();
        return true;
      }
    filter();
    return true;
  }

 protected:
  bool eventFilter(QObject* o, QEvent* e) override {
    auto* w = qobject_cast<QWidget*>(o);
    const bool ours = w && w->window() == services().window();
    switch (e->type()) {
      case QEvent::ShortcutOverride:  // a tip's letter, not the window's one-key shortcut (F, H, 1 ...)
        if (ours && static_cast<QKeyEvent*>(e)->key() == Qt::Key_Alt) {  // Alt alone is the tips', not the menu bar's (its Alt+F stays)
          e->accept();
          return true;
        }
        if (m_level && ours) {
          auto* key = static_cast<QKeyEvent*>(e);
          const QString text = key->text();
          if (key->key() == Qt::Key_Escape || key->key() == Qt::Key_Backspace || (text.size() == 1 && text[0].isLetterOrNumber() && !(key->modifiers() & ~(Qt::ShiftModifier | Qt::KeypadModifier)))) {
            e->accept();
            return true;
          }
        }
        break;
      case QEvent::KeyPress: {
        auto* key = static_cast<QKeyEvent*>(e);
        if (key->key() == Qt::Key_Alt) {
          if (!key->isAutoRepeat()) {
            m_altClean = ours && !(key->modifiers() & ~Qt::AltModifier);  // Alt alone (Qt flips Alt in the press's own modifiers)
            m_altAt = QDateTime::currentMSecsSinceEpoch();
          }
          break;
        }
        m_altClean = false;
        if (m_level && ours) return type(key);
        break;
      }
      case QEvent::KeyRelease: {
        auto* key = static_cast<QKeyEvent*>(e);
        if (key->key() == Qt::Key_Alt && m_altClean && ours && QDateTime::currentMSecsSinceEpoch() - m_altAt < 1500) {
          m_altClean = false;
          if (m_level) clear();
          else show(1);  // the release goes on: whoever follows the modifiers (the view) sees Alt go up
        }
        break;
      }
      case QEvent::MouseButtonPress:
      case QEvent::NonClientAreaMouseButtonPress:
        m_altClean = false;
        if (m_level) clear();
        break;
      case QEvent::WindowDeactivate:
      case QEvent::Resize:
        if (m_level && o == services().window()) clear();
        break;
      default:
        break;
    }
    return false;
  }

 private:
  void add(const QString& keys, QWidget* target, const QRect& rect, std::function<void()> run) {
    QMainWindow* window = services().window();
    const Tokens& t = theme::current();
    auto* badge = new QLabel(keys, window);
    badge->setObjectName("keyTip");
    QFont font = theme::mono(11);
    font.setBold(true);
    badge->setFont(font);
    badge->setAlignment(Qt::AlignCenter);
    badge->setAttribute(Qt::WA_TransparentForMouseEvents);
    badge->setStyleSheet(QString("QLabel#keyTip { background: %1; color: %2; border: 1px solid %3; border-radius: 3px; padding: 0 3px; }").arg(theme::css(t.fg), theme::css(t.bg), theme::css(t.bg)));
    badge->adjustSize();
    badge->resize(std::max(badge->width(), badge->height()), badge->height());
    const QPoint at = target->mapTo(window, QPoint(rect.center().x(), rect.bottom()));
    badge->move(at.x() - badge->width() / 2, at.y() - badge->height() + 2);
    badge->show();
    badge->raise();
    m_tips.push_back({keys, target, std::move(run), badge});
  }

  // Only the tips that start with what was typed stay.
  void filter() {
    for (const Tip& t : m_tips)
      if (t.badge) t.badge->setVisible(t.keys.startsWith(m_typed));
  }

  QPointer<RibbonBar> m_ribbon;
  QList<Tip> m_tips;
  int m_level = 0;
  QString m_typed;
  bool m_altClean = false;
  qint64 m_altAt = 0;
};

OPAD_AREA(KeyTipsArea)

// ---------------------------------------------------------------- bench
#include <QMenuBar>
#include <QMouseEvent>
#include <QStyle>
#include <QTimer>

#include "BenchRegistry.hpp"
#include "HelpWindows.hpp"
#include "MainWindow.hpp"

// OPAD_BENCH_KEYTIPS=<prefix> (a box): Alt tapped shows a key on every tab, the workspace chip and the tab row's buttons
// (1, 2, 3 ...); the window's one-key shortcuts wait meanwhile; a tab's key opens the tab and shows its tools' keys
// (<prefix>.tools.png); Esc goes back to the tabs, Esc again leaves; Fit's key runs Fit and the tips go; a click or Alt
// again takes them away; Alt held with another key (Alt+F, the File menu's mnemonic) shows none.
OPAD_BENCH(OPAD_BENCH_KEYTIPS, keytips) {
  const QString prefix = value;
  auto failed = std::make_shared<QStringList>();
  auto check = [failed](bool ok, const QString& what) {
    trace::log(QString("bench: key tips: %1 %2").arg(what, ok ? "PASS" : "FAIL"));
    if (!ok) *failed << what;
  };
  KeyTipsArea* area = nullptr;
  for (AreaController* a : w.m_areas)
    if (a->objectName() == "KeyTipsArea") area = static_cast<KeyTipsArea*>(a);
  QTimer::singleShot(600, &w, [=, &w] {
    if (!area) return check(false, "the key tips area"), QCoreApplication::exit(2);
    auto send = [&w](QEvent::Type type, int key, Qt::KeyboardModifiers mods, const QString& text = {}) {
      QKeyEvent e(type, key, mods, text);
      QApplication::sendEvent(&w, &e);
      return e.isAccepted();
    };
    auto tapAlt = [&] {
      send(QEvent::KeyPress, Qt::Key_Alt, Qt::AltModifier);
      send(QEvent::KeyRelease, Qt::Key_Alt, Qt::NoModifier);
    };
    auto keyOf = [area](const std::function<bool(QWidget*)>& is) {
      for (const auto& t : area->tips())
        if (t.target && is(t.target)) return t.keys;
      return QString();
    };
    auto typeKeys = [&](const QString& keys) {
      for (QChar c : keys) send(QEvent::KeyPress, c.unicode(), Qt::NoModifier, c);
    };
    trace::log(QString("bench: key tips: the style's Alt menu-bar navigation is %1").arg(w.style()->styleHint(QStyle::SH_MenuBar_AltKeyNavigation) ? "on" : "off"));
    QTabBar* tabs = w.m_ribbon->tabBar();
    w.m_ribbon->setCurrentTab(0);
    tapAlt();
    int tabTips = 0, numbered = 0;
    QSet<QString> keys;
    for (const auto& t : area->tips()) {
      tabTips += t.target == tabs;
      numbered += t.keys.size() == 1 && t.keys[0].isDigit();
      keys << t.keys;
      if (!t.badge || !t.badge->isVisibleTo(&w)) check(false, "a tip shows: " + t.keys);
    }
    int visibleTabs = 0;
    for (int i = 0; i < tabs->count(); ++i) visibleTabs += tabs->isTabVisible(i);
    check(area->level() == 1 && tabTips == visibleTabs && numbered >= 3 && keys.size() == area->tips().size(),
          QString("Alt shows a key on each of the %1 tabs and %2 buttons of the tab row, none twice").arg(tabTips).arg(numbered));
    QKeyEvent over(QEvent::ShortcutOverride, Qt::Key_F, Qt::NoModifier, "f");
    over.ignore();  // as Qt sends it
    QApplication::sendEvent(&w, &over);
    check(over.isAccepted(), "meanwhile a letter is the tips', not the window's shortcut (F)");
    QStringList tabKeys;  // in the tabs' order
    for (const auto& t : area->tips())
      if (t.target == tabs) tabKeys << t.keys;
    const QString second = tabKeys.value(1);
    typeKeys(second);
    check(area->level() == 2 && tabs->currentIndex() == 1 && !area->tips().isEmpty(), QString("the second tab's key (%1) opens it and shows %2 tools' keys").arg(second).arg(area->tips().size()));
    w.grab(QRect(w.m_ribbon->mapTo(&w, QPoint()), w.m_ribbon->size())).save(prefix + ".tools.png");  // the tips are the window's
    send(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    check(area->level() == 1, "Esc goes back to the tabs");
    send(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    check(area->level() == 0 && area->tips().isEmpty(), "Esc again leaves");
    // Fit through its keys: the first tab, then Fit.
    tapAlt();
    typeKeys(keyOf([tabs](QWidget* t) { return t == tabs; }));  // the first tab's tip comes first
    int fits = 0;
    const auto connection = QObject::connect(w.action("view.fit"), &QAction::triggered, &w, [&fits] { ++fits; });
    const QString fit = keyOf([&w](QWidget* t) { auto* b = qobject_cast<QToolButton*>(t); return b && b->defaultAction() == w.action("view.fit"); });
    typeKeys(fit);
    QObject::disconnect(connection);
    check(!fit.isEmpty() && fits == 1 && area->level() == 0, QString("Fit's key (%1) runs Fit and the tips go").arg(fit));
    tapAlt();
    QMouseEvent press(QEvent::MouseButtonPress, QPointF(5, 5), w.mapToGlobal(QPointF(5, 5)), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(&w, &press);
    const bool clicked = area->level() == 0;
    tapAlt();
    tapAlt();
    check(clicked && area->level() == 0 && !w.menuBar()->activeAction() && !w.menuBar()->hasFocus(), "a click takes them away, and so does Alt again; the menu bar is left alone");
    send(QEvent::KeyPress, Qt::Key_Alt, Qt::AltModifier);
    send(QEvent::KeyPress, Qt::Key_F, Qt::AltModifier, "f");
    send(QEvent::KeyRelease, Qt::Key_Alt, Qt::NoModifier);
    check(area->level() == 0, "Alt with another key (Alt+F) shows none");
    bool listed = false;
    for (const auto& g : help::keyGroups(w.m_actions, false, "fusion"))
      for (const auto& r : g.rows) listed = listed || r.text() == "Alt";
    check(listed, "the shortcuts cheat sheet lists them (Alt)");
    trace::log(QString("bench: key tips: %1").arg(failed->isEmpty() ? "PASS" : "FAIL: " + failed->join("; ")));
    QCoreApplication::exit(failed->isEmpty() ? 0 : 2);
  });
  return true;
}
