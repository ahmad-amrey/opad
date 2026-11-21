#pragma once
// A repository in the document's own folder (Set up repository on a document beside other files, GitWatch::setUp): whether
// the folder holds anything but the document, the folder it would move into, and moving its sidecars along. Qt Core only
// (tests/test_own_folder.cpp); the document itself is written at its new place by the caller (AppDocument::saveAsync, so
// its linked files' paths follow it), never moved under it.
#include <QString>
#include <QStringList>

namespace ownfolder {

struct Plan {
  bool needed = false;  // the folder holds other files or folders than the document and its sidecars
  QStringList others;   // their names, sorted (the first few are named in the question)
  QString folder;       // the new folder beside the document: <folder>/<name>, else "<name> 2", "<name> 3"... (free names)
  QString file;         // the document's path in it
};

// The document's sidecars: <name>.opad.<anything> beside it (a save's .tmp, a merge's .orig).
bool isSidecar(const QString& document, const QString& entry);
// Files the system leaves in folders (desktop.ini, Thumbs.db, .DS_Store): never a reason to ask.
bool isSystemFile(const QString& entry);
Plan plan(const QString& document);
// The first free folder name for `base` in `parent` (no file or folder of that name, compared without case on Windows).
QString freeFolder(const QString& parent, const QString& base);
// Moves the old document's sidecars beside the new one (renamed after it); the names that could not be moved.
QStringList moveSidecars(const QString& from, const QString& to);

}  // namespace ownfolder
