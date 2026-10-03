// File associations (Windows): registering writes the ProgIDs, "Open with" entries and Default apps capabilities under
// a scratch registry root (OPAD_ASSOC_ROOT), a narrower set removes the rest, and an empty one removes everything.
#include <QCoreApplication>
#include <QFileInfo>
#include <QSettings>
#include <QUuid>

#include "FileAssociations.hpp"
#include "check.hpp"

TEST(register_narrow_and_remove) {
  if (!associations::supported()) return;
  const QString root = "HKEY_CURRENT_USER\\Software\\OPAD-assoc-test-" + QUuid::createUuid().toString(QUuid::WithoutBraces);
  qputenv("OPAD_ASSOC_ROOT", root.toUtf8());
  const QString exe = "C:\\Program Files\\OPAD\\opad.exe";
  QStringList all;
  for (const auto& f : associations::formats()) all << f.extension;
  associations::apply(all, exe);
  CHECK_EQ(associations::registered(exe).size(), all.size());
  QSettings classes(root + "\\Classes", QSettings::NativeFormat);
  CHECK_EQ(classes.value("OPAD.stl/shell/open/command/Default").toString(), QString("\"C:\\Program Files\\OPAD\\opad.exe\" \"%1\""));
  CHECK_EQ(classes.value(".stl/Default").toString(), QString("OPAD.stl"));  // nothing else claimed it in the scratch root
  CHECK(classes.contains(".stl/OpenWithProgids/OPAD.stl"));
  QSettings software(root, QSettings::NativeFormat);
  CHECK_EQ(software.value("RegisteredApplications/OPAD").toString(), QString("Software\\OPAD\\Capabilities"));
  CHECK(associations::registered("C:\\other\\opad.exe").isEmpty());  // another copy of OPAD is not this one

  associations::apply({".stl", ".step"}, exe);
  QStringList now = associations::registered(exe);
  now.sort();
  CHECK_EQ(now, QStringList({".step", ".stl"}));
  CHECK(!classes.contains("OPAD.dxf/shell/open/command/Default"));
  CHECK(!classes.contains(".dxf/Default"));

  associations::apply({}, exe);
  CHECK(associations::registered(exe).isEmpty());
  CHECK(!software.contains("RegisteredApplications/OPAD"));

  // Thumbnails: with opad-thumbnails.dll beside the program (the build puts it beside this test), the registered types
  // get it as their thumbnail provider; another app's provider stays.
  const QString built = QCoreApplication::applicationDirPath() + "/opad.exe";
  const QString handler = "/ShellEx/{e357fccd-a995-4576-b01f-234630154e96}/Default", ours = "{67DD85AE-5101-4C3A-9231-39D7B7EFEC39}";
  classes.setValue("SystemFileAssociations/.dxf" + handler, "{00000000-1111-2222-3333-444444444444}");
  associations::apply({".stl", ".dxf"}, built);
  classes.sync();
  if (QFileInfo::exists(QCoreApplication::applicationDirPath() + "/opad-thumbnails.dll")) {
    CHECK_EQ(classes.value("SystemFileAssociations/.stl" + handler).toString(), ours);
    CHECK_EQ(classes.value("OPAD.stl" + handler).toString(), ours);
    CHECK(classes.value("CLSID/" + ours + "/InprocServer32/Default").toString().endsWith("opad-thumbnails.dll"));
  }
  associations::apply({}, built);
  classes.sync();
  CHECK(!classes.contains("SystemFileAssociations/.stl" + handler));
  CHECK(!classes.contains("CLSID/" + ours + "/InprocServer32/Default"));
  CHECK_EQ(classes.value("SystemFileAssociations/.dxf" + handler).toString(), QString("{00000000-1111-2222-3333-444444444444}"));
  QSettings parent("HKEY_CURRENT_USER\\Software", QSettings::NativeFormat);
  parent.remove(root.section('\\', -1));
  parent.sync();
}

int main(int argc, char** argv) {
  QCoreApplication app(argc, argv);
  return check::run_all(argc, argv);
}
