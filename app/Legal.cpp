#include "Legal.hpp"

#include <QAction>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFontDatabase>
#include <QLabel>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSettings>
#include <QSignalBlocker>
#include <QVBoxLayout>

#include "opad/drawing_io.hpp"
#include "opad/util.hpp"

namespace legal {

QString aboutText() {
  return QObject::tr("<b>OPAD %1</b><br>Git-native CAD and review: 3D models, meshes and 2D drawings in one append-only "
                     "document that diffs and merges in git.<br><br>Copyright (c) 2026 Ahmad Amrey. MIT licence. Built on "
                     "Open CASCADE Technology (LGPL 2.1 with exception) and Qt (LGPL 3) among other libraries: see Help &gt; "
                     "Third-party licences.<br><br>Headless twin: <code>opad-cli</code>; Python: <code>import opad</code>."
                     "<br><br><small>Autodesk, AutoCAD, DWG, Fusion and ViewCube are trademarks of Autodesk, Inc.; SOLIDWORKS "
                     "of Dassault Systèmes; Onshape of PTC; Blender of the Blender Foundation. They are named only to describe "
                     "compatibility; OPAD is not affiliated with or endorsed by them.</small>")
      .arg(QString::fromStdString(opad::version_string()));
}

void showAbout(QWidget* parent) { QMessageBox::about(parent, QObject::tr("About OPAD"), aboutText()); }

void showNotices(QWidget* parent) {
  auto* dialog = new QDialog(parent);
  dialog->setAttribute(Qt::WA_DeleteOnClose);
  dialog->setObjectName("thirdPartyNotices");
  dialog->setWindowTitle(QObject::tr("Third-party licences"));
  auto* layout = new QVBoxLayout(dialog);
  auto* intro = new QLabel(QObject::tr("OPAD is free software under the MIT licence. It is built on the libraries below, each "
                                       "under its own licence; their versions, sources and licence texts follow."), dialog);
  intro->setWordWrap(true);
  layout->addWidget(intro);
  auto* text = new QPlainTextEdit(dialog);
  text->setObjectName("noticesText");
  text->setReadOnly(true);
  text->setLayoutDirection(Qt::LeftToRight);  // licence texts are English, also in a right-to-left UI
  // Fixed pitch (licence texts are laid out for it); a style sheet, since the theme's sets every widget's font.
  text->setStyleSheet(QString("QPlainTextEdit { font-family: \"%1\"; }").arg(QFontDatabase::systemFont(QFontDatabase::FixedFont).family()));
  text->setPlainText(QString::fromStdString(opad::third_party_notices()));
  layout->addWidget(text, 1);
  auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, dialog);
  QObject::connect(buttons->addButton(QObject::tr("About Qt"), QDialogButtonBox::ActionRole), &QPushButton::clicked, dialog,
                   [dialog] { QMessageBox::aboutQt(dialog); });
  QObject::connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::close);
  layout->addWidget(buttons);
  dialog->resize(900, 640);
  dialog->show();
}

void applySettings() { opad::set_use_oda(QSettings().value("files/useOda", false).toBool()); }

void setUseOda(QWidget* parent, QAction* action, bool on) {
  if (on) {
    const bool installed = !opad::oda_file_converter().empty();
    QMessageBox box(QMessageBox::Information, QObject::tr("ODA File Converter"),
        QObject::tr("The ODA File Converter is third-party software from the Open Design Alliance, not part of OPAD. Its "
                    "licence lets non-members use it for non-commercial purposes only: turn this on only if your use is "
                    "covered.\n\nWhen on, OPAD reads and writes DWG through an installed ODA File Converter; otherwise "
                    "through LibreDWG's dwg2dxf and dxf2dwg, beside OPAD or on PATH (the portable package includes them)."),
        QMessageBox::Cancel, parent);
    if (!installed) box.setInformativeText(QObject::tr("No ODA File Converter is installed on this computer (OPAD looks in the ODA folder under Program Files)."));
    QPushButton* use = box.addButton(QObject::tr("Use it"), QMessageBox::AcceptRole);
    box.setDefaultButton(QMessageBox::Cancel);  // accepting the terms takes a deliberate click, not Enter
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
