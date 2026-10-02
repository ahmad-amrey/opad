#pragma once
// "Open with OPAD" for the formats it reads (Windows): file types registered for the current user only (HKCU, no
// administrator rights), listed under Default apps, and removable again. Windows lets only the user pick the default
// app for a type, so registering offers OPAD there; a type nothing else claims opens with OPAD at once.
#include <QDialog>
#include <QList>
#include <QString>
#include <QStringList>

namespace associations {
struct Format {
  QString extension;  // ".stl"
  QString name;       // "STL mesh"
};
const QList<Format>& formats();
bool supported();  // Windows only; elsewhere the desktop file (Linux) and the bundle's Info.plist (macOS) declare them
// The extensions registered for this executable now.
QStringList registered(const QString& exe);
// Registers `extensions` for `exe` and removes OPAD's registration of the others. The registry root is
// HKEY_CURRENT_USER\Software unless OPAD_ASSOC_ROOT names another (tests).
void apply(const QStringList& extensions, const QString& exe);
}  // namespace associations

// Settings > File types: which formats open with OPAD.
class FileTypesDialog : public QDialog {
  Q_OBJECT
 public:
  explicit FileTypesDialog(QWidget* parent = nullptr);
};
