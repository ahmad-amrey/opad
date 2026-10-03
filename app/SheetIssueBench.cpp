#include "MainWindow.hpp"

#include <QApplication>
#include <QCheckBox>
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QLineEdit>
#include <QMouseEvent>
#include <QTemporaryDir>
#include <QToolButton>

#include "AppDocument.hpp"
#include "BenchRegistry.hpp"
#include "DocsArea.hpp"
#include "IssueRevision.hpp"
#include "SheetAnnotate.hpp"
#include "SheetCanvas.hpp"
#include "SheetPage.hpp"
#include "opad/drawing/annotate.hpp"
#include "opad/drawing/sheet.hpp"
#include "opad/drawing/tables.hpp"

// OPAD_BENCH_SHEET_ISSUE=<prefix> (UI-84): a plate and a pin saved into a fresh git repository and drawn front and top. The
// Revision table tool previews the table's header and places it with a click. Issue revision… opens its dialog: the next
// revision (A), the PDF beside the document, git found; issued with a description and an approver: one sheet_issue step on a
// worker's plan with every view's linework frozen in the body store, the PDF written as the drawing shows the revision and its
// SHA-256 in the record, the document saved, committed (with the PDF) and tagged. The revision table lists A, the title block
// says A, the sheet bar names it; the pin moved, the bar warns that the sheet changed since A; revision B issued without a
// PDF or git, the bar names B again. <prefix>.dialog.png, <prefix>.issue.png.
OPAD_BENCH(OPAD_BENCH_SHEET_ISSUE, sheetIssue) {
  using opad::drawing::Vec2;
  const QString& prefix = value;
  DocsArea* docs = DocsArea::of(w.m_areas);
  bool ok = docs && docs->sheetPage();
  const auto check = [&](bool pass, const QString& what) {
    trace::log(QString("bench: issue: %1 %2").arg(what, pass ? "PASS" : "FAIL"));
    ok = ok && pass;
  };
  const auto waitFor = [](const std::function<bool()>& done, int ms) {
    QElapsedTimer t;
    t.start();
    while (!done() && t.elapsed() < ms) QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    return done();
  };
  if (!ok) {
    trace::log("bench: issue: the documentation area FAIL");
    QCoreApplication::exit(2);
    return true;
  }
  SheetPage* page = docs->sheetPage();
  SheetCanvas* canvas = page->canvas();
  SheetAnnotator* tools = page->annotator();
  AppDocument* doc = w.m_doc;
  QTemporaryDir repo;
  const auto settled = [&] {
    const auto states = canvas->viewStates();
    return !canvas->busy() && !tools->busy() && !doc->designBusy && !states.empty() &&
           std::all_of(states.begin(), states.end(), [](const SheetCanvas::ViewState& v) { return v.final || !v.error.isEmpty(); });
  };
  const auto gitSays = [&](const QStringList& args) {  // a git command's output, waited for with the event loop running
    QString out;
    bool done = false;
    git::run(repo.path(), {args}, docs, [&](bool, const QString& text) {
      out = text;
      done = true;
    });
    waitFor([&] { return done; }, 20000);
    return out;
  };
  const auto mouse = [&](QEvent::Type type, const QPointF& scene) {
    QWidget* vp = canvas->viewport();
    const QPoint p = canvas->mapFromScene(scene);
    QMouseEvent e(type, QPointF(p), QPointF(vp->mapToGlobal(p)), type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton,
                  type == QEvent::MouseButtonRelease || type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(vp, &e);
  };
  const auto click = [&](Vec2 paper) {
    const QPointF s = canvas->toScene(paper);
    mouse(QEvent::MouseMove, s);
    mouse(QEvent::MouseButtonPress, s);
    mouse(QEvent::MouseButtonRelease, s);
  };
  const auto items = [&](const std::string& kind) {
    std::vector<const opad::SheetItem*> out;
    for (const auto& t : doc->scene.sheet_items)
      if (t.kind == kind) out.push_back(&t);
    return out;
  };
  try {
    qputenv("GIT_AUTHOR_NAME", "OPAD bench");
    qputenv("GIT_AUTHOR_EMAIL", "bench@opad.invalid");
    qputenv("GIT_COMMITTER_NAME", "OPAD bench");
    qputenv("GIT_COMMITTER_EMAIL", "bench@opad.invalid");
    gitSays({"init", "-q"});
    check(QDir(repo.path()).exists(".git"), "a fresh git repository");
    doc->newDocument();
    doc->run("feature", {{"kind", "box"}, {"inputs", {{"length", "60 mm"}, {"width", "40 mm"}, {"height", "5 mm"}}}});
    const std::string pin = doc->run("feature", {{"kind", "cylinder"},
                                                {"inputs", {{"plane", {{"origin", {0, 0, 5}}, {"normal", {0, 0, 1}}}}, {"x", 10}, {"diameter", 6}, {"height", 15}, {"operation", "new"}}}})["body_ids"][0];
    const QString file = QDir(repo.path()).filePath("Plate.opad");
    bool saved = false;
    doc->saveAsync(docs->services().jobs(), file, true, [&](bool done, const QString&) { saved = done; });
    check(waitFor([&] { return saved; }, 20000), "the document saved into it");
    w.action("workspace.drawings")->trigger();
    std::string sheet;
    docs->createDrawing({{"size", "A3"}, {"orientation", "landscape"}, {"standard", "iso"}, {"projection", "first"}, {"scale", "1:1"}, {"views", {"front", "top"}}},
                        [&](const std::string& id) { sheet = id; });
    check(waitFor([&] { return !sheet.empty(); }, 20000) && waitFor(settled, 30000), "a drawing of it: front and top");
    const opad::Sheet* sh = doc->scene.sheet(sheet);
    if (!sh) throw opad::Error("no sheet");
    check(!page->issueButton()->isVisible() && canvas->sinceIssue().is_null(), "never issued: the sheet bar says nothing about revisions");

    // The revision table: its header follows the pointer, a click places it.
    w.action("drawings.revisionTable")->trigger();
    waitFor([&] { return !tools->busy(); }, 15000);
    const Vec2 corner{sh->width - 10, sh->height - 10};
    mouse(QEvent::MouseMove, canvas->toScene(corner));
    const auto& preview = canvas->preview();
    check(preview && std::any_of(preview->prims.begin(), preview->prims.end(), [](const opad::drawing::Prim& p) { return p.kind == opad::drawing::Prim::Kind::Text && p.text == "REV"; }),
          "Revision table previews its header at the pointer");
    click(corner);
    check(waitFor([&] { return items("revision_table").size() == 1 && !doc->designBusy; }, 15000) && waitFor(settled, 30000), "a click places it");
    tools->cancel();

    // Issue revision…: the dialog, git found, the PDF beside the document.
    w.action("drawings.issue")->trigger();
    IssueDialog* dialog = nullptr;
    waitFor([&] { return (dialog = w.findChild<IssueDialog*>()) != nullptr; }, 5000);
    check(dialog && dialog->revisionEdit()->text() == "A", "Issue revision… opens its dialog on revision A");
    if (!dialog) throw opad::Error("no dialog");
    check(waitFor([&] { return dialog->gitChecked(); }, 20000) && dialog->gitBox()->isEnabled(), "it finds the document's git repository");
    const QString pdf = QDir::fromNativeSeparators(dialog->pdfEdit()->text());
    check(QFileInfo(pdf).absolutePath() == QFileInfo(file).absolutePath() && pdf.endsWith("Drawing 1 rev A.pdf"), "the PDF goes beside the document: " + pdf);
    dialog->descriptionEdit()->setText("First release");
    dialog->approvedEdit()->setText("R. Engineer");
    dialog->gitBox()->setChecked(true);
    dialog->grab().save(prefix + ".dialog.png");
    const size_t ops = doc->doc.ops.size();
    dialog->accept();
    check(waitFor([&] { return docs->lastIssue.is_object() && (docs->lastIssue.contains("git") || docs->lastIssue.contains("error")); }, 60000),
          "issued, saved, committed and tagged");
    const opad::SheetItem* issue = items("issue").empty() ? nullptr : items("issue")[0];
    check(issue && issue->def.value("rev", "") == "A" && issue->def.value("description", "") == "First release" && doc->doc.ops.size() == ops + 1,
          "one step: revision A with its description");
    if (issue) {
      const opad::json keys = issue->def.value("frozen", opad::json::object());
      bool frozen = keys.size() == 2;
      for (const auto& [view, key] : keys.items()) frozen = frozen && doc->doc.has_body(key.get<std::string>());
      check(frozen && issue->def.value("fingerprints", opad::json::object()).size() == 2, "both views' linework frozen in the body store, their fingerprints kept");
      QFile f(pdf);
      const bool read = f.open(QIODevice::ReadOnly);
      const QByteArray data = read ? f.readAll() : QByteArray();
      check(read && data.startsWith("%PDF-") && QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex().toStdString() == issue->def.value("pdf_sha256", ""),
            "the PDF written, its SHA-256 in the record");
    }
    check(docs->lastIssue.value("git", "") == "tagged Drawing-1-rev-A" && gitSays({"tag", "-l"}).split('\n').contains("Drawing-1-rev-A") &&
              gitSays({"log", "-1", "--format=%s"}) == "Drawing 1 rev A" && gitSays({"ls-files"}).contains("Drawing 1 rev A.pdf") && !doc->doc.dirty,
          "saved, committed with the PDF and tagged: " + gitSays({"log", "--oneline", "--decorate"}));
    check(waitFor(settled, 30000) && waitFor([&] { return page->issueButton()->isVisible() && page->issueButton()->text() == "Rev A"; }, 10000),
          "the sheet bar names revision A: " + page->issueButton()->text());
    {
      const opad::SheetItem* table = items("revision_table")[0];
      const opad::json rows = opad::drawing::measure_item(doc->doc, doc->scene, *doc->scene.sheet(sheet), *table, nullptr)["rows"];
      const opad::json values = opad::drawing::title_values(doc->doc, doc->scene, *doc->scene.sheet(sheet), false);
      check(rows.size() == 1 && rows[0]["description"] == "First release" && rows[0]["approved"] == "R. Engineer" && values["revision"] == "A",
            "the revision table lists A, the title block says A");
    }

    // The pin moved: the bar warns.
    bool moved = false;
    docs->run("transform", {{"target", pin}, {"matrix", {1, 0, 0, 5, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1}}}, [&](const opad::json& out) { moved = !out.is_null(); });
    check(waitFor([&] { return moved; }, 10000) && waitFor(settled, 30000) &&
              waitFor([&] { return page->issueButton()->text() == "Changed since rev A"; }, 10000) && page->issueButton()->toolTip().contains("2 views"),
          "the pin moved: the bar says the sheet changed since A (" + page->issueButton()->toolTip().replace('\n', " / ") + ")");
    canvas->fitSheet();
    waitFor(settled, 10000);
    page->grab().save(prefix + ".issue.png");
    // Revision B, without a PDF or git.
    docs->lastIssue = nullptr;
    docs->issue({{"sheet", sheet}, {"description", "Pin moved"}}, QString(), false);
    check(waitFor([&] { return docs->lastIssue.is_object(); }, 60000) && docs->lastIssue.value("rev", "") == "B" && !docs->lastIssue.contains("pdf_sha256"),
          "revision B issued without a PDF");
    check(waitFor(settled, 30000) && waitFor([&] { return page->issueButton()->text() == "Rev B"; }, 10000), "the bar names B: " + page->issueButton()->text());
  } catch (const std::exception& e) {
    check(false, QString("bench: %1").arg(QString::fromUtf8(e.what())));
  }
  trace::log(QString("bench: issue: done %1").arg(ok ? "PASS" : "FAIL"));
  QCoreApplication::exit(ok ? 0 : 2);
  return true;
}
