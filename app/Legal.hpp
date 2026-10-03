#pragma once
// Licences and third-party terms in the UI (TODO 11 UI-13/14): the About box, the third-party notices, the ODA opt-in.
#include <QString>
class QAction;
class QWidget;

namespace legal {
QString aboutText();                // what OPAD is, its licence, what it is built on, the trademark disclaimer
void showAbout(QWidget* parent);    // Help > About OPAD
void showNotices(QWidget* parent);  // Help > Third-party licences: opad::third_party_notices() in a dialog (non-modal)
// The ODA File Converter switch (setting files/useOda, default off): turning it on first shows ODA's terms (non-members:
// non-commercial use only) and stays off unless the user accepts. Applies the setting to the core.
void setUseOda(QWidget* parent, QAction* action, bool on);
void applySettings();  // at start: files/useOda -> opad::set_use_oda
}  // namespace legal
