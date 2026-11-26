// OPAD's .gitignore (app/Git.cpp, Git.hpp "the OPAD blocks"): the rule groups written at Set up repository, the merge
// into a file the user has already (their lines kept, only the missing ones added under OPAD's block), the old note's
// lines taken over, and the linked block that keeps a document's linked files tracked whatever their type, following
// their renames and removals. The matcher (git::ignoredBy) is checked against git check-ignore itself in a temporary
// repository, and git status there shows the asset and not the exports. Temporary folders only; git's global and system
// config are left out.
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QProcessEnvironment>
#include <QTemporaryDir>

#include "Git.hpp"
#include "check.hpp"

namespace {
QString read(const QString& path) {
  QFile f(path);
  return f.open(QIODevice::ReadOnly) ? QString::fromUtf8(f.readAll()) : QString();
}
void write(const QString& path, const QByteArray& data) {
  QDir().mkpath(QFileInfo(path).absolutePath());
  QFile f(path);
  CHECK(f.open(QIODevice::WriteOnly));
  f.write(data);
}
git::Context in(const QString& dir) {
  git::Context c;
  c.program = git::findProgram();
  c.dir = dir;
  return c;
}
int lineCount(const QString& text, const QString& line) { return QString(text).remove('\r').split('\n').count(line); }
}  // namespace

TEST(template_has_the_groups_on_by_default) {
  const QString text = git::ignoreText("");
  CHECK(text.startsWith("# >>> OPAD: "));
  CHECK(text.endsWith("# <<< OPAD\n"));
  for (const char* p : {"*.step", "*.stp", "*.iges", "*.stl", "*.3mf", "*.obj", "*.glb", "*.gltf", "*.ply", "*.dxf", "*.dwg", "*.pdf", "*.svg", "*.png",
                        "*.opad.tmp", "*.opad.orig", "Thumbs.db", ".DS_Store", "desktop.ini", "*~"})
    CHECK_EQ(lineCount(text, p), 1);
  CHECK(!text.contains("*.frd"));  // simulation results: off by default
  CHECK(!text.contains("*.opad\n"));  // the documents themselves are never ignored
  CHECK_EQ(git::ignoreText(text), text);  // idempotent
  bool found = false;
  CHECK(git::ignoreGroupsIn(text, &found) == git::defaultIgnoreGroups() && found);
  git::ignoreGroupsIn("build/\n", &found);
  CHECK(!found);
  // Customised: only the chosen groups, in the table's order whatever the order asked.
  const QString two = git::ignoreText("", {"simulation", "opad"});
  CHECK(two.contains("*.frd\n") && two.contains("*.opad.tmp\n") && !two.contains("*.step"));
  CHECK(two.indexOf("*.opad.tmp") < two.indexOf("*.frd"));
  CHECK(git::ignoreGroupsIn(two) == QStringList({"opad", "simulation"}));
  CHECK(git::ignoreText("", {}).isEmpty());  // nothing chosen: no file at all
  // The documents and their linked files' kinds the template leaves alone.
  CHECK(!git::ignoredBy(text, "model.opad") && !git::ignoredBy(text, "sub/model.opad"));
  CHECK(git::ignoredBy(text, "model.opad.tmp") && git::ignoredBy(text, "out/part.step") && git::ignoredBy(text, "a/b/Thumbs.db"));
#if defined(_WIN32) || defined(__APPLE__)
  CHECK(git::ignoredBy(text, "out/part.STEP"));  // git ignores case there (core.ignorecase); on Linux, as git, it does not
#endif
  CHECK(!git::ignoredBy(text, "board.kicad_pcb") && !git::ignoredBy(text, "photo.jpg"));
}

TEST(existing_file_is_merged_never_overwritten) {
  const QString mine = "build/\n*.pdf\n!keep.step\n";
  const QString merged = git::ignoreText(mine);
  CHECK(merged.startsWith(mine + "\n# >>> OPAD"));  // the user's lines first, as they were
  CHECK_EQ(lineCount(merged, "*.pdf"), 1);           // what they have already is not repeated
  CHECK_EQ(git::ignoreText(merged), merged);
  const git::IgnoreChange change = git::ignoreChange(mine, git::defaultIgnoreGroups());
  CHECK(change.added.contains("*.step") && !change.added.contains("*.pdf") && change.removed.isEmpty());
  CHECK(git::ignoreChange(merged, git::defaultIgnoreGroups()).added.isEmpty());
  // The user's lines after OPAD's block still win over it.
  const QString after = merged + "!keep.png\n";
  CHECK(!git::ignoredBy(after, "keep.png") && git::ignoredBy(after, "other.png"));
  // Set up again with a group unticked: only OPAD's block changes, in place.
  const QString less = git::ignoreText(after, {"opad", "system"});
  CHECK(less.startsWith(mine) && less.endsWith("!keep.png\n") && !less.contains("*.step\n"));
  const git::IgnoreChange removed = git::ignoreChange(after, {"opad", "system"});
  CHECK(removed.removed.contains("*.step") && removed.added.isEmpty());
  CHECK(git::ignoreGroupsIn(less) == QStringList({"opad", "system"}));
  // Nothing left to add: the block goes, the user's file stays as it was.
  CHECK_EQ(git::ignoreText(git::ignoreText(mine, {"images"}), {}), mine);
  // CRLF files stay CRLF.
  const QString crlf = QString(mine).replace("\n", "\r\n");
  const QString mergedCrlf = git::ignoreText(crlf);
  CHECK(mergedCrlf.startsWith(crlf) && !QString(mergedCrlf).remove("\r\n").contains('\n'));
  CHECK_EQ(git::ignoreText(mergedCrlf), mergedCrlf);
}

TEST(old_note_becomes_the_block) {
  QString old = "build/\n\n# OPAD: temporary saves, merge leftovers, portable data, caches and recovery snapshots\n";
  for (auto p = git::kIgnored; *p; ++p) old += QString::fromLatin1(*p) + "\n";
  CHECK(git::ignoreGroupsIn(old) == QStringList({"opad"}));
  const QString text = git::ignoreText(old);
  CHECK(text.startsWith("build/\n\n# >>> OPAD"));
  CHECK(!text.contains("# OPAD: temporary saves"));
  for (auto p = git::kIgnored; *p; ++p) CHECK_EQ(lineCount(text, QString::fromLatin1(*p)), 1);
  CHECK(text.contains("*.step\n"));
}

TEST(linked_files_stay_tracked) {
  const QString base = git::ignoreText("");
  // A linked STEP and drawing (ignored types) get exceptions, a linked KiCad board (not ignored) none.
  QString text = git::keepTracked(base, "model.opad", {"parts/bracket.step", "drawings/plan.dxf", "board.kicad_pcb"});
  CHECK(text.startsWith(base.trimmed()));
  CHECK(text.contains("# linked by model.opad\n!/drawings/plan.dxf\n!/parts/bracket.step\n# <<< OPAD\n"));
  CHECK(!text.contains("kicad_pcb"));
  CHECK(!git::ignoredBy(text, "parts/bracket.step") && git::ignoredBy(text, "parts/other.step") && git::ignoredBy(text, "bracket.step"));
  CHECK_EQ(git::keepTracked(text, "model.opad", {"drawings/plan.dxf", "parts/bracket.step", "board.kicad_pcb"}), text);  // idempotent
  // A second document has its own section; renaming one asset moves its exception; the groups can change meanwhile.
  text = git::keepTracked(text, "sub/other.opad", {"sub/ref.png"});
  CHECK(git::trackedDocuments(text) == QStringList({"model.opad", "sub/other.opad"}));
  text = git::keepTracked(text, "model.opad", {"assets/bracket.step", "drawings/plan.dxf"});
  CHECK(!git::ignoredBy(text, "assets/bracket.step") && git::ignoredBy(text, "parts/bracket.step"));
  CHECK(!git::ignoredBy(text, "sub/ref.png"));
  text = git::ignoreText(text, {"models", "drawings", "images", "opad", "system", "simulation"});
  CHECK(text.endsWith("!/sub/ref.png\n# <<< OPAD\n") && text.contains("*.frd\n"));  // the linked block stays last
  CHECK(!git::ignoredBy(text, "assets/bracket.step"));
  // Removed: the section goes, then the block.
  text = git::keepTracked(text, "model.opad", {});
  CHECK(git::trackedDocuments(text) == QStringList({"sub/other.opad"}));
  text = git::keepTracked(text, "sub/other.opad", {});
  CHECK(!text.contains("linked") && git::ignoredBy(text, "sub/ref.png"));
  // The user's own rule ignoring a type is overridden for a linked file too; a file outside the top never is written.
  const QString mine = git::keepTracked("*.jpg\n", "m.opad", {"photo.jpg", "../outside.jpg", "C:/abs.jpg"});
  CHECK(mine.startsWith("*.jpg\n\n# >>> OPAD: files the documents link") && !git::ignoredBy(mine, "photo.jpg"));
  CHECK(!mine.contains("outside") && !mine.contains("abs.jpg"));
  CHECK(git::keepTracked("", "m.opad", {"a.step"}).isEmpty());  // nothing ignores it: no file
  // Names with gitignore's special characters are written literally.
  const QString odd = git::keepTracked(base, "m.opad", {"odd [1]*.step", "#hash.stl", "!bang.stl"});
  CHECK(odd.contains("!/odd \\[1\\]\\*.step\n") && !git::ignoredBy(odd, "odd [1]*.step") && git::ignoredBy(odd, "odd 1x.step"));
  CHECK(!git::ignoredBy(odd, "#hash.stl") && !git::ignoredBy(odd, "!bang.stl"));
}

// The matcher against git check-ignore, path by path.
TEST(matcher_agrees_with_git) {
  QTemporaryDir tmp;
  const QString dir = tmp.path();
  CHECK(git::run(in(dir), {"init", "-q"}).ok());
  const QString text = git::keepTracked(git::ignoreText("/build/\nlogs/**/*.txt\n*.b[a-c]k\n!important.pdf\ndoc/**/keep/\n?.tmp\n", {"models", "drawings", "images", "opad", "system", "simulation"}),
                                        "model.opad", {"parts/a.step", "x/important.png"});
  write(dir + "/.gitignore", text.toUtf8());
  const QStringList paths = {"model.opad", "model.opad.tmp", "parts/a.step", "parts/b.step", "a.STEP", "x/important.png", "important.pdf", "sub/important.pdf",
                             "build/x.txt", "sub/build/x.txt", "logs/a/b/c.txt", "logs/c.txt", "other/logs/c.txt", "f.bak", "f.bdk", "f.bck", "a.tmp", "ab.tmp",
                             "doc/x/keep/f.txt", "doc/keep/f.txt", "Thumbs.db", "deep/x/.DS_Store", "notes.txt~", "opad-data/x.ini", "data/cache/x.bin",
                             "sub/data/cache/x.bin", "opad-ccx-1a2b/job.frd", "r.vtu", "board.kicad_pcb", "photo.jpg", "x/y/model.svg"};
  for (const QString& p : paths) {
    const git::Result r = git::run(in(dir), {"check-ignore", "-q", "--no-index", "--", p});
    CHECK(r.code == 0 || r.code == 1);
    if ((r.code == 0) != git::ignoredBy(text, p)) std::printf("disagrees with git on %s (git: %d)\n", qPrintable(p), r.code);
    CHECK_EQ(r.code == 0, git::ignoredBy(text, p));
  }
}

// In a repository: status shows the linked STEP and the document, not the exports; the first commit takes the .gitignore.
TEST(status_leaves_exports_out_and_keeps_assets) {
  QTemporaryDir tmp;
  const QString dir = tmp.path();
  CHECK(git::run(in(dir), {"init", "-q", "-b", "main"}).ok());
  write(dir + "/model.opad", "{}\n");
  write(dir + "/model.step", "export\n");
  write(dir + "/model.pdf", "export\n");
  write(dir + "/model.opad.tmp", "save\n");
  write(dir + "/assets/bracket.step", "linked\n");
  write(dir + "/.gitignore", git::keepTracked(git::ignoreText(""), "model.opad", {"assets/bracket.step"}).toUtf8());
  git::Repo r;
  git::readStatus(in(dir), r);
  QStringList shown;
  for (const git::Entry& e : r.status.entries) shown << e.path;
  shown.sort();
  CHECK(shown == QStringList({".gitignore", "assets/", "model.opad"}) || shown == QStringList({".gitignore", "assets/bracket.step", "model.opad"}));
  CHECK(git::run(in(dir), {"add", "-A"}).ok());
  const QStringList staged = QString::fromUtf8(git::run(in(dir), {"diff", "--cached", "--name-only"}).out).split('\n', Qt::SkipEmptyParts);
  CHECK(staged == QStringList({".gitignore", "assets/bracket.step", "model.opad"}));
  // The asset renamed and the exception moved with it: the new name is tracked, the old one (now an export) is not.
  QFile::rename(dir + "/assets/bracket.step", dir + "/assets/bracket-v2.step");
  write(dir + "/.gitignore", git::keepTracked(read(dir + "/.gitignore"), "model.opad", {"assets/bracket-v2.step"}).toUtf8());
  CHECK(git::run(in(dir), {"add", "-A"}).ok());
  const QString names = QString::fromUtf8(git::run(in(dir), {"diff", "--cached", "--name-only"}).out);
  CHECK(names.contains("assets/bracket-v2.step") && !names.contains("model.step"));
}

int main(int argc, char** argv) {
  QCoreApplication app(argc, argv);
  QTemporaryDir home;
  qputenv("GIT_CONFIG_GLOBAL", QFile::encodeName(home.filePath("gitconfig")));
  qputenv("GIT_CONFIG_NOSYSTEM", "1");
  qputenv("GIT_TERMINAL_PROMPT", "0");
  if (git::findProgram().isEmpty()) {
    std::printf("git not found: skipped\n");
    return 0;
  }
  return check::run_all(argc, argv);
}
