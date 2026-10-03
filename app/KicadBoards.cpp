#include "KicadBoards.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSettings>
#include <QStandardItemModel>
#include <QStatusBar>
#include <QVBoxLayout>
#include <memory>

#include "MainWindow.hpp"
#include "opad/kicad_pcb.hpp"

namespace {
std::filesystem::path fsPath(const QString& path) { return std::filesystem::path(path.toStdU16String()); }
}  // namespace

KicadDialog::KicadDialog(QWidget* parent, bool import) : QDialog(parent) {
  setObjectName("kicadDialog");
  setWindowTitle(import ? tr("Import KiCad board") : tr("KiCad boards"));
  QSettings settings;
  auto* layout = new QVBoxLayout(this);
  auto* form = new QFormLayout();
  // KiCad's own export when kicad-cli is installed (found by its install folder, PATH or OPAD_KICAD_CLI: no process here).
  const opad::KicadCli cli = opad::kicad_cli();
  m_reader = new QComboBox(this);
  m_reader->setObjectName("reader");
  m_reader->addItem(tr("OPAD's board reader"), "opad");
  m_reader->addItem(cli.program.empty() ? tr("KiCad's own STEP export (KiCad not found)")
                    : cli.version.empty() ? tr("KiCad's own STEP export (kicad-cli)")
                                          : tr("KiCad's own STEP export (KiCad %1)").arg(QString::fromStdString(cli.version)),
                    "kicad-cli");
  if (cli.program.empty()) qobject_cast<QStandardItemModel*>(m_reader->model())->item(1)->setEnabled(false);
  m_reader->setCurrentIndex(!cli.program.empty() && settings.value("kicad/reader", "opad").toString() == "kicad-cli" ? 1 : 0);
  m_reader->setToolTip(tr("OPAD's reader is fast and shares each model between its footprints. KiCad's export is exactly what KiCad makes, "
                          "with the copper and silkscreen on request; it takes longer, and the import stays linked to the board."));
  form->addRow(tr("Read with"), m_reader);
  m_tracks = new QCheckBox(tr("Copper tracks"), this);
  m_tracks->setObjectName("tracks");
  m_tracks->setChecked(settings.value("kicad/tracks", false).toBool());
  m_pads = new QCheckBox(tr("Pads"), this);
  m_pads->setObjectName("pads");
  m_pads->setChecked(settings.value("kicad/pads", false).toBool());
  m_silkscreen = new QCheckBox(tr("Silkscreen"), this);
  m_silkscreen->setObjectName("silkscreen");
  m_silkscreen->setChecked(settings.value("kicad/silkscreen", false).toBool());
  auto* extras = new QHBoxLayout();
  for (auto* box : {m_tracks, m_pads, m_silkscreen}) extras->addWidget(box);
  extras->addStretch();
  form->addRow(tr("KiCad adds"), extras);
  m_readerNote = new QLabel(this);
  m_readerNote->setObjectName("secondary");
  m_readerNote->setWordWrap(true);
  form->addRow(QString(), m_readerNote);
  // Tracks came with KiCad 8, pads and silkscreen with KiCad 9 (an unknown version is taken as the newest).
  const int major = cli.major();
  auto update = [this, major] {
    const bool kicad = m_reader->currentData().toString() == "kicad-cli";
    m_tracks->setEnabled(kicad && (!major || major >= 8));
    m_pads->setEnabled(kicad && (!major || major >= 9));
    m_silkscreen->setEnabled(kicad && (!major || major >= 9));
    m_vias->setEnabled(!kicad);
    m_height->setEnabled(!kicad);
    m_readerNote->setText(kicad ? tr("KiCad finds the models itself and leaves out footprints whose model it cannot find. Imported, the board stays "
                                     "linked: it is watched and synced, and its STEP is made again where it is missing.")
                                : QString());
    m_readerNote->setVisible(kicad);
  };
  connect(m_reader, &QComboBox::currentIndexChanged, this, update);
  m_components = new QCheckBox(tr("Footprints' 3D models"), this);
  m_components->setObjectName("components");
  m_components->setChecked(settings.value("kicad/components", true).toBool());
  m_dnp = new QCheckBox(tr("Parts marked do not populate"), this);
  m_dnp->setObjectName("dnp");
  m_dnp->setChecked(settings.value("kicad/dnp", true).toBool());
  m_vias = new QCheckBox(tr("Drill the vias (slower on dense boards)"), this);
  m_vias->setObjectName("vias");
  m_vias->setChecked(settings.value("kicad/vias", false).toBool());
  auto* include = new QVBoxLayout();
  for (auto* box : {m_components, m_dnp, m_vias}) include->addWidget(box);
  form->addRow(tr("Include"), include);
  m_origin = new QComboBox(this);
  m_origin->setObjectName("origin");
  m_origin->addItem(tr("Drill/place origin, else the board's centre"), "auto");
  m_origin->addItem(tr("The board's centre"), "center");
  m_origin->addItem(tr("KiCad's page origin"), "page");
  m_origin->setCurrentIndex(std::max(0, m_origin->findData(settings.value("kicad/origin", "auto").toString())));
  form->addRow(tr("Origin"), m_origin);
  m_height = new QDoubleSpinBox(this);
  m_height->setObjectName("placeholderHeight");
  m_height->setRange(0.1, 50);
  m_height->setDecimals(2);
  m_height->setSingleStep(0.5);
  m_height->setSuffix(" mm");
  m_height->setValue(settings.value("kicad/placeholderHeight", 1.0).toDouble());
  m_height->setToolTip(tr("A footprint whose 3D model is not found shows as a translucent box over its courtyard, this tall unless it has a Height property."));
  form->addRow(tr("Boxes for missing models"), m_height);
  m_dirs = new QPlainTextEdit(this);
  m_dirs->setObjectName("modelDirs");
  m_dirs->setPlaceholderText(tr("Folders with 3D models (STEP or VRML), one per line"));
  m_dirs->setPlainText(settings.value("kicad/modelDirs").toStringList().join('\n'));
  m_dirs->setFixedHeight(m_dirs->fontMetrics().lineSpacing() * 4 + 12);
  auto* add = new QPushButton(tr("Add folder…"), this);
  connect(add, &QPushButton::clicked, this, [this] {
    const QString dir = QFileDialog::getExistingDirectory(this, tr("3D model folder"));
    if (!dir.isEmpty()) m_dirs->appendPlainText(QDir::fromNativeSeparators(dir));
  });
  auto* dirs = new QVBoxLayout();
  dirs->addWidget(m_dirs);
  dirs->addWidget(add, 0, Qt::AlignLeft);
  form->addRow(tr("Model folders"), dirs);
  m_download = new QComboBox(this);
  m_download->setObjectName("download");
  m_download->addItem(tr("Ask before downloading"), "ask");
  m_download->addItem(tr("Download without asking"), "always");
  m_download->addItem(tr("Never download"), "never");
  m_download->setCurrentIndex(std::max(0, m_download->findData(settings.value("kicad/download", "ask").toString())));
  form->addRow(tr("KiCad library models"), m_download);
  layout->addLayout(form);
  auto* note = new QLabel(tr("Models are looked for as KiCad does (the project, its variables, KiCad's settings and install), then in these folders. "
                             "Models of KiCad's library that are not installed can be downloaded from gitlab.com/kicad/libraries/kicad-packages3D "
                             "into OPAD's cache for you alone: CC-BY-SA 4.0 with KiCad's design exception, free to use in your own designs; "
                             "they are not part of OPAD."),
                          this);
  note->setObjectName("secondary");
  note->setWordWrap(true);
  layout->addWidget(note);
  auto* buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, this);
  QPushButton* ok = buttons->addButton(import ? tr("Import") : tr("Save"), QDialogButtonBox::AcceptRole);
  ok->setDefault(true);
  layout->addWidget(buttons);
  connect(buttons, &QDialogButtonBox::accepted, this, [this] {
    save();
    accept();
  });
  connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
  setMinimumWidth(520);
  update();
}

void KicadDialog::save() const {
  QSettings settings;
  settings.setValue("kicad/components", m_components->isChecked());
  settings.setValue("kicad/dnp", m_dnp->isChecked());
  settings.setValue("kicad/vias", m_vias->isChecked());
  settings.setValue("kicad/origin", m_origin->currentData());
  settings.setValue("kicad/placeholderHeight", m_height->value());
  QStringList dirs;
  for (const QString& line : m_dirs->toPlainText().split('\n'))
    if (!line.trimmed().isEmpty()) dirs << QDir::fromNativeSeparators(line.trimmed());
  settings.setValue("kicad/modelDirs", dirs);
  settings.setValue("kicad/download", m_download->currentData());
  settings.setValue("kicad/reader", m_reader->currentData());
  settings.setValue("kicad/tracks", m_tracks->isChecked());
  settings.setValue("kicad/pads", m_pads->isChecked());
  settings.setValue("kicad/silkscreen", m_silkscreen->isChecked());
}

bool KicadDialog::linked() { return QSettings().value("kicad/reader", "opad").toString() == "kicad-cli" && !opad::kicad_cli().program.empty(); }

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
