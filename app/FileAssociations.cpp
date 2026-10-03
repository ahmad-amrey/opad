#include "FileAssociations.hpp"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shlobj.h>
#endif

#include <QCheckBox>
#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileInfo>
#include <QGridLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QSettings>
#include <QUrl>
#include <QVBoxLayout>
#include <QCoreApplication>

namespace associations {
namespace {

QString root() {
  const QString custom = qEnvironmentVariable("OPAD_ASSOC_ROOT");
  return custom.isEmpty() ? QStringLiteral("HKEY_CURRENT_USER\\Software") : custom;
}
QString progId(const QString& extension) { return QStringLiteral("OPAD") + extension; }  // "OPAD.stl"
const QString kThumbnailClsid = QStringLiteral("{67DD85AE-5101-4C3A-9231-39D7B7EFEC39}");    // shell/thumbnails.cpp
const QString kThumbnailHandler = QStringLiteral("{e357fccd-a995-4576-b01f-234630154e96}");  // IThumbnailProvider
QString command(const QString& exe) { return QStringLiteral("\"%1\" \"%2\"").arg(QDir::toNativeSeparators(exe), QStringLiteral("%1")); }

}  // namespace

const QList<Format>& formats() {
  static const QList<Format> list = {
      {".opad", QObject::tr("OPAD document")},
      {".step", QObject::tr("STEP model")},   {".stp", QObject::tr("STEP model")},
      {".iges", QObject::tr("IGES model")},   {".igs", QObject::tr("IGES model")},
      {".brep", QObject::tr("BREP model")},   {".brp", QObject::tr("BREP model")},
      {".stl", QObject::tr("STL mesh")},      {".3mf", QObject::tr("3MF print file")},
      {".obj", QObject::tr("OBJ mesh")},      {".ply", QObject::tr("PLY mesh")},
      {".gltf", QObject::tr("glTF model")},   {".glb", QObject::tr("glTF model")},
      {".wrl", QObject::tr("VRML model")},    {".vrml", QObject::tr("VRML model")},
      {".dxf", QObject::tr("DXF drawing")},   {".dwg", QObject::tr("DWG drawing")},
      {".svg", QObject::tr("SVG drawing")},
  };
  return list;
}

bool supported() {
#ifdef _WIN32
  return true;
#else
  return false;
#endif
}

QStringList registered(const QString& exe) {
  QStringList out;
  QSettings classes(root() + "\\Classes", QSettings::NativeFormat);
  for (const Format& f : formats())
    if (classes.value(progId(f.extension) + "/shell/open/command/Default").toString().compare(command(exe), Qt::CaseInsensitive) == 0)
      out << f.extension;
  return out;
}

void apply(const QStringList& extensions, const QString& exe) {
  QSettings classes(root() + "\\Classes", QSettings::NativeFormat);
  QSettings software(root(), QSettings::NativeFormat);
  const QString app = QFileInfo(exe).fileName();
  const QString applications = "Applications/" + app;
  for (const Format& f : formats()) {
    const QString id = progId(f.extension);
    if (extensions.contains(f.extension)) {
      classes.setValue(id + "/Default", f.name);
      classes.setValue(id + "/FriendlyTypeName", f.name);
      classes.setValue(id + "/DefaultIcon/Default", QStringLiteral("\"%1\",0").arg(QDir::toNativeSeparators(exe)));
      classes.setValue(id + "/shell/open/command/Default", command(exe));
      classes.setValue(f.extension + "/OpenWithProgids/" + id, QString());
      // A type nothing else claims opens with OPAD at once; a claimed one is offered under "Open with" and Default apps.
      if (classes.value(f.extension + "/Default").toString().isEmpty()) classes.setValue(f.extension + "/Default", id);
      classes.setValue(applications + "/SupportedTypes/" + f.extension, QString());
      software.setValue("OPAD/Capabilities/FileAssociations/" + f.extension, id);
    } else if (classes.contains(id + "/shell/open/command/Default")) {
      classes.remove(id);
      classes.remove(f.extension + "/OpenWithProgids/" + id);
      if (classes.value(f.extension + "/Default").toString() == id) classes.remove(f.extension + "/Default");
      classes.remove(applications + "/SupportedTypes/" + f.extension);
      software.remove("OPAD/Capabilities/FileAssociations/" + f.extension);
    }
  }
  // Explorer thumbnails (shell/thumbnails.cpp, beside the program) for the same types: under the type's
  // SystemFileAssociations (shown whichever app opens it) and under OPAD's own ProgID (which wins where OPAD is the
  // default app). Another app's handler is left alone; ours is removed with the type.
  const QString dll = QFileInfo(exe).absolutePath() + QStringLiteral("/opad-thumbnails.dll");
  const bool thumbnails = QFileInfo::exists(dll) && !extensions.isEmpty();
  for (const Format& f : formats()) {
    for (const QString& owner : {"SystemFileAssociations/" + f.extension, progId(f.extension)}) {
      const QString handler = owner + QStringLiteral("/ShellEx/") + kThumbnailHandler;
      const QString current = classes.value(handler + "/Default").toString();
      if (thumbnails && extensions.contains(f.extension)) {
        if (current.isEmpty()) classes.setValue(handler + "/Default", kThumbnailClsid);  // per user; a system-wide one stays below it
      } else if (current.compare(kThumbnailClsid, Qt::CaseInsensitive) == 0) {
        classes.remove(handler);
      }
    }
  }
  if (thumbnails) {
    classes.setValue("CLSID/" + kThumbnailClsid + "/Default", QStringLiteral("OPAD thumbnails"));
    classes.setValue("CLSID/" + kThumbnailClsid + "/InprocServer32/Default", QDir::toNativeSeparators(dll));
    classes.setValue("CLSID/" + kThumbnailClsid + "/InprocServer32/ThreadingModel", QStringLiteral("Apartment"));
  } else {
    classes.remove("CLSID/" + kThumbnailClsid);
  }
  if (extensions.isEmpty()) {
    classes.remove(applications);
    software.remove("OPAD/Capabilities");
    software.remove("RegisteredApplications/OPAD");
  } else {
    classes.setValue(applications + "/FriendlyAppName", QStringLiteral("OPAD"));
    classes.setValue(applications + "/shell/open/command/Default", command(exe));
    software.setValue("OPAD/Capabilities/ApplicationName", QStringLiteral("OPAD"));
    software.setValue("OPAD/Capabilities/ApplicationDescription", QObject::tr("Views and edits CAD models, meshes and drawings"));
    software.setValue("RegisteredApplications/OPAD", QStringLiteral("Software\\OPAD\\Capabilities"));
  }
  classes.sync();
  software.sync();
#ifdef _WIN32
  SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);  // Explorer picks the change up now
#endif
}

}  // namespace associations

FileTypesDialog::FileTypesDialog(QWidget* parent) : QDialog(parent) {
  setWindowTitle(tr("File types"));
  auto* layout = new QVBoxLayout(this);
  auto* intro = new QLabel(tr("Open these file types with OPAD. They are registered for your Windows account only. "
                              "Types that no other app handles open in OPAD right away; for the rest, choose OPAD in Default apps. "
                              "Explorer and the Open dialog show their thumbnails too."),
                           this);
  intro->setWordWrap(true);
  layout->addWidget(intro);
  const QString exe = QCoreApplication::applicationFilePath();
  QStringList current = associations::registered(exe);
  const bool none = current.isEmpty();
  auto* grid = new QGridLayout();
  QList<QCheckBox*> boxes;
  int i = 0;
  for (const auto& f : associations::formats()) {
    auto* box = new QCheckBox(QStringLiteral("%1  (%2)").arg(f.name, f.extension), this);
    box->setProperty("extension", f.extension);
    box->setChecked(none || current.contains(f.extension));
    grid->addWidget(box, i / 2, i % 2);
    boxes << box;
    ++i;
  }
  layout->addLayout(grid);
  auto* status = new QLabel(this);
  status->setObjectName("secondary");
  status->setWordWrap(true);
  layout->addWidget(status);
  auto updateStatus = [status, exe] {
    const int n = static_cast<int>(associations::registered(exe).size());
    status->setText(n ? tr("%1 file types open with OPAD.").arg(n) : tr("No file types are registered for OPAD yet."));
  };
  updateStatus();
  auto* buttons = new QDialogButtonBox(this);
  QPushButton* all = buttons->addButton(tr("Select all"), QDialogButtonBox::ResetRole);
  QPushButton* defaults = buttons->addButton(tr("Default apps…"), QDialogButtonBox::HelpRole);
  QPushButton* register_ = buttons->addButton(tr("Open with OPAD"), QDialogButtonBox::AcceptRole);
  buttons->addButton(QDialogButtonBox::Close);
  register_->setDefault(true);
  layout->addWidget(buttons);
  connect(all, &QPushButton::clicked, this, [boxes] { for (auto* b : boxes) b->setChecked(true); });
  connect(defaults, &QPushButton::clicked, this, [] { QDesktopServices::openUrl(QUrl("ms-settings:defaultapps?registeredAppUser=OPAD")); });
  connect(register_, &QPushButton::clicked, this, [boxes, exe, updateStatus] {
    QStringList chosen;
    for (auto* b : boxes) if (b->isChecked()) chosen << b->property("extension").toString();
    associations::apply(chosen, exe);
    updateStatus();
  });
  connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
}
