#include <QFileInfo>
#include <QMessageBox>
#include <QPushButton>
#include <QStatusBar>
#include <memory>

#include "MainWindow.hpp"
#include "opad/kicad_pcb.hpp"

namespace {
std::filesystem::path fsPath(const QString& path) { return std::filesystem::path(path.toStdU16String()); }
}  // namespace

// The models are KiCad's (CC-BY-SA 4.0 with its design exception): never bundled, fetched per user on consent into the
// user cache, where the reader looks for them (opad::kicad_download_models). The download is a job on a worker.
void MainWindow::offerKicadModels() {
  const opad::json& last = m_doc->lastLoad;
  const QString board = QString::fromStdString(last.value("file", ""));
  const int count = last.contains("info") ? last["info"].value("downloadable", 0) : 0;
  if (count <= 0 || !board.endsWith(".kicad_pcb", Qt::CaseInsensitive) || m_kicadOffered.contains(QFileInfo(board).absoluteFilePath())) return;
  m_kicadOffered << QFileInfo(board).absoluteFilePath();
  QString mode = m_settings.value("kicad/download", "ask").toString();
  if (mode == "never") return;
  if (mode != "always") {
    QMessageBox box(QMessageBox::Question, tr("KiCad 3D models"),
                    tr("%1 3D models of this board come from KiCad's library, which is not installed here. Download them from the KiCad library "
                       "(gitlab.com/kicad/libraries/kicad-packages3D)?").arg(count),
                    QMessageBox::NoButton, this);
    box.setInformativeText(tr("They are licensed CC-BY-SA 4.0 with KiCad's design exception: free to use in your own designs. They are saved in "
                              "OPAD's cache for you alone and are not part of OPAD."));
    QPushButton* once = box.addButton(tr("Download"), QMessageBox::AcceptRole);
    QPushButton* always = box.addButton(tr("Always download"), QMessageBox::AcceptRole);
    QPushButton* never = box.addButton(tr("Never"), QMessageBox::DestructiveRole);
    box.addButton(tr("Not now"), QMessageBox::RejectRole);
    box.setDefaultButton(once);
    box.exec();
    if (box.clickedButton() == never) m_settings.setValue("kicad/download", "never");
    if (box.clickedButton() == always) m_settings.setValue("kicad/download", "always");
    if (box.clickedButton() != once && box.clickedButton() != always) return;
  }
  const opad::KicadOptions options = AppDocument::kicadOptions();
  auto result = std::make_shared<opad::json>();
  const QString phase = tr("Downloading KiCad 3D models");
  m_jobs->async(
      phase,
      [board, options, result, phase](Progress p) {
        *result = opad::kicad_download_models(fsPath(board), options, [p, phase](double fraction, const std::string&) {
          p.setPhase(phase, static_cast<int>(fraction * 100));
          return !p.cancelled();
        });
      },
      [this, board, result](bool ok, const QString& error) {
        const int got = ok ? static_cast<int>((*result)["downloaded"].size()) : 0, failed = ok ? static_cast<int>((*result)["failed"].size()) : 0;
        trace::log(QString("kicad: downloaded %1 models, %2 failed%3").arg(got).arg(failed).arg(ok ? QString() : ": " + error));
        if (!ok) {
          if (!error.contains("cancel", Qt::CaseInsensitive)) statusBar()->showMessage(tr("The KiCad 3D models could not be downloaded: %1").arg(error), 8000);
          return;
        }
        if (failed) statusBar()->showMessage(tr("%1 KiCad 3D models downloaded, %2 not found in the library").arg(got).arg(failed), 8000);
        else statusBar()->showMessage(tr("%1 KiCad 3D models downloaded").arg(got), 6000);
        if (!got) return;
        // A viewed board is read again to show them; an imported one keeps its boxes until it is imported again.
        if (m_doc->browse && !m_doc->loading && QFileInfo(m_doc->viewing) == QFileInfo(board)) openPath(m_doc->viewing);
        else if (!failed) statusBar()->showMessage(tr("%1 KiCad 3D models downloaded: import the board again to show them").arg(got), 8000);
      });
}
