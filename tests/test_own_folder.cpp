// A repository in the document's own folder (app/OwnFolder.cpp, Set up repository): the folder decision (the document alone
// with its sidecars and the system's files: set up in place; anything else: ask), the new folder's free name beside the
// document, and the sidecars moved along. Temporary folders only.
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>

#include "OwnFolder.hpp"
#include "check.hpp"

namespace {
void touch(const QString& path, const QByteArray& text = "x") {
  QDir().mkpath(QFileInfo(path).absolutePath());
  QFile f(path);
  if (!f.open(QIODevice::WriteOnly)) throw check::Failure(("cannot write " + path).toStdString());
  f.write(text);
}
}  // namespace

TEST(document_alone_is_set_up_in_place) {
  QTemporaryDir dir;
  CHECK(dir.isValid());
  const QString doc = dir.filePath("Bracket.opad");
  touch(doc);
  touch(dir.filePath("Bracket.opad.tmp"));   // a save's
  touch(dir.filePath("Bracket.opad.orig"));  // a merge's
  touch(dir.filePath("desktop.ini"));
  touch(dir.filePath("Thumbs.db"));
  const ownfolder::Plan p = ownfolder::plan(doc);
  CHECK(!p.needed);
  CHECK(p.others.isEmpty());
}

TEST(other_files_and_folders_ask) {
  QTemporaryDir dir;
  const QString doc = dir.filePath("Bracket.opad");
  touch(doc);
  touch(dir.filePath("notes.txt"));
  touch(dir.filePath("Other.opad"));
  touch(dir.filePath("Bracket.step"));  // the same name, another file: not a sidecar
  QDir(dir.path()).mkdir("assets");
  const ownfolder::Plan p = ownfolder::plan(doc);
  CHECK(p.needed);
  CHECK_EQ(p.others.join(","), QStringLiteral("assets,Bracket.step,notes.txt,Other.opad"));
  CHECK_EQ(QDir::cleanPath(p.folder), QDir::cleanPath(dir.filePath("Bracket")));
  CHECK_EQ(QDir::cleanPath(p.file), QDir::cleanPath(dir.filePath("Bracket/Bracket.opad")));
}

TEST(new_folder_takes_a_free_name) {
  QTemporaryDir dir;
  const QString doc = dir.filePath("Bracket.opad");
  touch(doc);
  QDir(dir.path()).mkdir("Bracket");       // a folder of that name
  touch(dir.filePath("Bracket 2"));        // and a file of the next one
  const ownfolder::Plan p = ownfolder::plan(doc);
  CHECK(p.needed);  // the folder "Bracket" is another entry
  CHECK_EQ(QDir::cleanPath(p.folder), QDir::cleanPath(dir.filePath("Bracket 3")));
  CHECK_EQ(QFileInfo(p.file).fileName(), QStringLiteral("Bracket.opad"));
  CHECK(!QFileInfo::exists(p.folder));  // planned, not made
}

TEST(sidecars_follow_the_document) {
  QTemporaryDir dir;
  const QString doc = dir.filePath("Bracket.opad");
  touch(doc);
  touch(dir.filePath("Bracket.opad.orig"), "orig");
  touch(dir.filePath("Bracket.opad.tmp"), "tmp");
  touch(dir.filePath("Bracket.step"), "step");
  const QString moved = dir.filePath("Bracket/Bracket.opad");
  QDir().mkpath(QFileInfo(moved).absolutePath());
  touch(moved);  // written there by the save
  CHECK(ownfolder::moveSidecars(doc, moved).isEmpty());
  CHECK(QFileInfo::exists(dir.filePath("Bracket/Bracket.opad.orig")));
  CHECK(QFileInfo::exists(dir.filePath("Bracket/Bracket.opad.tmp")));
  CHECK(!QFileInfo::exists(dir.filePath("Bracket.opad.orig")));
  CHECK(QFileInfo::exists(dir.filePath("Bracket.step")));  // never touched
  CHECK(QFileInfo::exists(doc));                           // the caller removes the old document, once the new one is saved
}

TEST(sidecar_names) {
  CHECK(ownfolder::isSidecar("C:/a/Box.opad", "Box.opad.tmp"));
  CHECK(ownfolder::isSidecar("C:/a/Box.opad", "box.OPAD.orig"));
  CHECK(!ownfolder::isSidecar("C:/a/Box.opad", "Box.opad"));
  CHECK(!ownfolder::isSidecar("C:/a/Box.opad", "Box.opad."));
  CHECK(!ownfolder::isSidecar("C:/a/Box.opad", "Box.step"));
  CHECK(!ownfolder::isSidecar("C:/a/Box.opad", "Boxes.opad.tmp"));
  CHECK(ownfolder::isSystemFile(".DS_Store"));
  CHECK(!ownfolder::isSystemFile("readme.txt"));
}

CHECK_MAIN()
