#include "KeyTips.hpp"

#include <QRegularExpression>

namespace keytips {

QStringList assign(const QStringList& labels, const QSet<QString>& taken) {
  static const QString pool = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
  QSet<QString> used = taken;
  int free = 0;
  for (QChar c : pool) free += used.contains(c) ? 0 : 1;
  QString prefix;  // two-key tips start with it; no single tip is it
  if (labels.size() > free)
    for (int i = int(pool.size()) - 1; i >= 0 && prefix.isEmpty(); --i)
      if (pool[i].isLetter() && !used.contains(pool[i])) prefix = pool[i];
  if (!prefix.isEmpty()) used.insert(prefix);
  QStringList out;
  for (int i = 0; i < labels.size(); ++i) out << QString();
  auto ascii = [](QChar c) { c = c.toUpper(); return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ? QString(c) : QString(); };
  static const QRegularExpression words(R"(\b\w)");
  for (int i = 0; i < labels.size(); ++i) {
    const QString text = QString(labels[i]).remove('&');
    QStringList candidates;
    for (auto it = words.globalMatch(text); it.hasNext();) candidates << ascii(it.next().captured(0)[0]);
    for (QChar c : text) candidates << ascii(c);
    for (const QString& c : candidates)
      if (!c.isEmpty() && !used.contains(c)) {
        out[i] = c;
        used.insert(c);
        break;
      }
  }
  int second = 0;
  for (int i = 0; i < labels.size(); ++i) {
    if (!out[i].isEmpty()) continue;
    for (QChar c : pool)
      if (!used.contains(c)) {
        out[i] = c;
        used.insert(c);
        break;
      }
    if (out[i].isEmpty() && !prefix.isEmpty() && second < pool.size()) out[i] = prefix + pool[second++];
  }
  return out;
}

}  // namespace keytips
