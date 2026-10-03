#pragma once
// Licences and third-party terms in the UI (TODO 11 UI-13/14): the About box, the third-party notices, the ODA opt-in.
class QAction;
class QWidget;

namespace legal {
// The ODA File Converter switch (setting files/useOda, default off): turning it on first shows ODA's terms (non-members:
// non-commercial use only) and stays off unless the user accepts. Applies the setting to the core.
void setUseOda(QWidget* parent, QAction* action, bool on);
void applySettings();  // at start: files/useOda -> opad::set_use_oda
}  // namespace legal
