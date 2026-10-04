#include "KeyText.hpp"

#include "ShortcutEditor.hpp"

#include <QAction>
#include <QHash>
#include <QRegularExpression>

namespace {
std::function<QAction*(const QString&)>& lookup() {
  static std::function<QAction*(const QString&)> l;
  return l;
}

keys::Style resolved(keys::Style s) {
#ifdef Q_OS_MACOS
  return s == keys::Style::Native ? keys::Style::Mac : s;
#else
  return s == keys::Style::Native ? keys::Style::Pc : s;
#endif
}

// The key without its modifiers.
QString keyName(int key, keys::Style s) {
  const bool mac = s == keys::Style::Mac;
  switch (key) {
    case Qt::Key_Return:
    case Qt::Key_Enter: return mac ? QStringLiteral("↩") : QStringLiteral("Enter");
    case Qt::Key_Escape: return QStringLiteral("Esc");
    case Qt::Key_Delete: return mac ? QStringLiteral("⌦") : QStringLiteral("Del");
    case Qt::Key_Backspace: return mac ? QStringLiteral("⌫") : QStringLiteral("Backspace");
    case Qt::Key_Tab:
    case Qt::Key_Backtab: return mac ? QStringLiteral("⇥") : QStringLiteral("Tab");
    case Qt::Key_PageUp: return mac ? QStringLiteral("⇞") : QStringLiteral("PgUp");
    case Qt::Key_PageDown: return mac ? QStringLiteral("⇟") : QStringLiteral("PgDn");
    case Qt::Key_Home: return mac ? QStringLiteral("↖") : QStringLiteral("Home");
    case Qt::Key_End: return mac ? QStringLiteral("↘") : QStringLiteral("End");
    case Qt::Key_Insert: return QStringLiteral("Ins");
    case Qt::Key_Up: return QStringLiteral("↑");
    case Qt::Key_Down: return QStringLiteral("↓");
    case Qt::Key_Left: return QStringLiteral("←");
    case Qt::Key_Right: return QStringLiteral("→");
    case Qt::Key_Space: return QStringLiteral("Space");
    case Qt::Key_Menu: return QStringLiteral("Menu");
    case Qt::Key_Shift: return mac ? QStringLiteral("⇧") : QStringLiteral("Shift");
    case Qt::Key_Control: return mac ? QStringLiteral("⌘") : QStringLiteral("Ctrl");
    case Qt::Key_Alt: return mac ? QStringLiteral("⌥") : QStringLiteral("Alt");
    case Qt::Key_Meta: return mac ? QStringLiteral("⌃") : QStringLiteral("Meta");
    default: break;
  }
  // Letters, digits, F keys and punctuation: Qt's portable name, never translated ("F1", "A", "/", "+").
  return QKeySequence(QKeyCombination(Qt::NoModifier, Qt::Key(key))).toString(QKeySequence::PortableText);
}

QStringList chordCaps(QKeyCombination c, keys::Style s) {
  const Qt::KeyboardModifiers m = c.keyboardModifiers();
  QStringList out;
  if (s == keys::Style::Mac) {  // Apple's order: Control, Option, Shift, Command (Qt's Control is Command there)
    if (m & Qt::MetaModifier) out << QStringLiteral("⌃");
    if (m & Qt::AltModifier) out << QStringLiteral("⌥");
    if (m & Qt::ShiftModifier) out << QStringLiteral("⇧");
    if (m & Qt::ControlModifier) out << QStringLiteral("⌘");
  } else {  // Qt's: Meta, Ctrl, Alt, Shift
    if (m & Qt::MetaModifier) out << QStringLiteral("Meta");
    if (m & Qt::ControlModifier) out << QStringLiteral("Ctrl");
    if (m & Qt::AltModifier) out << QStringLiteral("Alt");
    if (m & Qt::ShiftModifier) out << QStringLiteral("Shift");
  }
  if (c.key() != Qt::Key_unknown && c.key() != 0) out << keyName(c.key(), s);
  return out;
}

QKeySequence standard(QKeySequence::StandardKey k) { return QKeySequence::keyBindings(k).value(0); }
}  // namespace

namespace keys {

Notifier* notifier() {
  static Notifier* n = new Notifier;  // lives as long as the process: listeners connect from anywhere, any time
  return n;
}

void announce() { emit notifier()->changed(); }

void setLookup(std::function<QAction*(const QString&)> l) { lookup() = std::move(l); }
bool hasLookup() { return bool(lookup()); }
QAction* action(const QString& id) { return lookup() && !id.isEmpty() ? lookup()(id) : nullptr; }

bool pressable(const QKeySequence& key) {
  for (int i = 0; i < key.count(); ++i)
    if (key[i].key() == Qt::Key_Exit) return false;
  return !key.isEmpty();
}

QList<QKeySequence> bindings(const QAction* a) {
  QList<QKeySequence> out;
  for (const QKeySequence& key : a ? shortcuts::bindings(a) : QList<QKeySequence>())
    if (pressable(key)) out << key;
  return out;
}
QKeySequence binding(const QAction* a) { return bindings(a).value(0); }
QKeySequence binding(const QString& id) { return binding(action(id)); }

QStringList caps(const QKeySequence& key, Style style) {
  const Style s = resolved(style);
  QStringList out;
  for (int i = 0; i < key.count(); ++i) {
    if (i) out << kThen;
    out << chordCaps(key[i], s);
  }
  return out;
}

QString joined(const QStringList& caps, Style style) {
  const QString plus = resolved(style) == Style::Mac ? QString() : QStringLiteral("+");
  QString out;
  bool first = true;
  for (const QString& c : caps) {
    if (c == kThen) { out += QStringLiteral(", "); first = true; continue; }
    if (!first) out += plus;
    out += c;
    first = false;
  }
  return out;
}

QString plain(const QKeySequence& key, Style style) { return joined(caps(key, style), style); }
QString isolate(const QString& text) { return text.isEmpty() ? text : QChar(0x2066) + text + QChar(0x2069); }
QString text(const QKeySequence& key, Style style) { return isolate(plain(key, style)); }
QString text(const QString& id, Style style) { return text(binding(id), style); }

QStringList fixedNames() { return {"esc", "enter", "ctrlEnter", "tab", "shiftTab", "shift", "alt", "ctrl", "del", "shiftDel", "backspace", "space", "f2", "undo", "redo", "copy"}; }

QStringList fixedCaps(const QString& name, Style style) {
  static const QHash<QString, QKeyCombination> table{
      {"esc", QKeyCombination(Qt::Key_Escape)},
      {"enter", QKeyCombination(Qt::Key_Return)},
      {"ctrlEnter", QKeyCombination(Qt::ControlModifier, Qt::Key_Return)},
      {"tab", QKeyCombination(Qt::Key_Tab)},
      {"shiftTab", QKeyCombination(Qt::ShiftModifier, Qt::Key_Tab)},
      {"shift", QKeyCombination(Qt::Key_Shift)},
      {"alt", QKeyCombination(Qt::Key_Alt)},
      {"ctrl", QKeyCombination(Qt::Key_Control)},
      {"del", QKeyCombination(Qt::Key_Delete)},
      {"shiftDel", QKeyCombination(Qt::ShiftModifier, Qt::Key_Delete)},  // the browser's and the timeline's own keys
      {"f2", QKeyCombination(Qt::Key_F2)},
      {"backspace", QKeyCombination(Qt::Key_Backspace)},
      {"space", QKeyCombination(Qt::Key_Space)}};
  if (const auto it = table.constFind(name); it != table.constEnd()) return chordCaps(*it, resolved(style));
  if (name == "undo") return caps(standard(QKeySequence::Undo), style);
  if (name == "redo") return caps(standard(QKeySequence::Redo), style);
  if (name == "copy") return caps(standard(QKeySequence::Copy), style);
  return {};
}

QString fixedText(const QString& name, Style style) { return isolate(joined(fixedCaps(name, style), style)); }

QString hint(const QString& id, const QString& verb) {
  const QString key = text(id);
  return key.isEmpty() ? QString() : key + ' ' + verb;
}

QString span(const QStringList& ids) {
  QStringList bound;
  bool consecutive = true;
  for (const QString& id : ids) {
    const QString key = plain(binding(id));
    if (key.isEmpty()) {
      consecutive = false;
      continue;
    }
    if (key.size() != 1 || (!bound.isEmpty() && (bound.last().size() != 1 || key[0].unicode() != bound.last()[0].unicode() + 1))) consecutive = false;
    bound << key;
  }
  if (bound.isEmpty()) return QString();
  if (consecutive && bound.size() > 2) return isolate(bound.first() + QChar(0x2013) + bound.last());
  return isolate(bound.join('/'));
}

bool isCommandId(const QString& text) {
  static const QRegularExpression id(QStringLiteral(R"(^[a-z][A-Za-z0-9_]*(\.[A-Za-z0-9_]+)+$)"));
  return id.match(text).hasMatch();
}

QString spec(const QString& keyOrId) {
  if (fixedNames().contains(keyOrId)) return joined(fixedCaps(keyOrId));
  if (isCommandId(keyOrId)) return plain(binding(keyOrId));
  return keyOrId;
}

}  // namespace keys
