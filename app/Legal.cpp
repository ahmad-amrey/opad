#include "Legal.hpp"

#include <QAction>
#include <QMessageBox>
#include <QPushButton>
#include <QSettings>
#include <QSignalBlocker>

#include "opad/drawing_io.hpp"

namespace legal {

void applySettings() { opad::set_use_oda(QSettings().value("files/useOda", false).toBool()); }

void setUseOda(QWidget* parent, QAction* action, bool on) {
  if (on) {
    const bool installed = !opad::oda_file_converter().empty();
    QMessageBox box(QMessageBox::Information, QObject::tr("ODA File Converter"),
        QObject::tr("The ODA File Converter is third-party software from the Open Design Alliance, not part of OPAD. Its "
                    "licence lets non-members use it for non-commercial purposes only: turn this on only if your use is "
                    "covered.\n\nWhen on, OPAD reads and writes DWG through an installed ODA File Converter; otherwise "
                    "through LibreDWG, which comes with OPAD."), QMessageBox::Cancel, parent);
    if (!installed) box.setInformativeText(QObject::tr("No ODA File Converter is installed on this computer (OPAD looks in Program Files\\ODA)."));
    QPushButton* use = box.addButton(QObject::tr("Use it"), QMessageBox::AcceptRole);
    box.setObjectName("odaTerms");
    box.exec();
    if (box.clickedButton() != use) {
      const QSignalBlocker block(action);
      action->setChecked(false);
      return;
    }
  }
  QSettings().setValue("files/useOda", on);
  opad::set_use_oda(on);
}

}  // namespace legal
