#include "BomExport.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPushButton>
#include <QRadioButton>
#include <QSettings>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <filesystem>
#include <utility>

#include "AppDocument.hpp"
#include "I18n.hpp"
#include "Jobs.hpp"
#include "PartProperties.hpp"
#include "Theme.hpp"
#include "opad/drawing/bom.hpp"
#include "opad/util.hpp"

namespace {
QString massText(const opad::json& v, const std::string& unit) { return v.is_number() ? QString::number(v.get<double>(), 'f', unit == "g" ? 2 : 3) : QString(); }
}  // namespace

BomDialog::BomDialog(AppDocument* doc, JobRunner* jobs, const std::vector<std::string>& selection, QWidget* parent)
    : QDialog(parent), m_doc(doc), m_jobs(jobs) {
  setObjectName("bomDialog");
  setWindowTitle(tr("Bill of materials"));
  resize(820, 600);
  for (const auto& id : selection)  // one component selected: its BoM is offered
    if (const opad::Node* n = doc->node(id); selection.size() == 1 && n && n->kind == opad::Node::Kind::Component) m_component = id;
  QSettings settings;
  auto* v = new QVBoxLayout(this);
  v->setContentsMargins(16, 16, 16, 16);
  v->setSpacing(10);
  auto* top = new QHBoxLayout();
  top->setSpacing(24);
  auto* left = new QFormLayout();
  m_mode = new QComboBox(this);
  m_mode->setObjectName("bom.mode");
  m_mode->addItem(tr("Parts only: every part once, quantities summed"), "parts");
  m_mode->addItem(tr("Top level: the assembly's own items"), "top");
  m_mode->addItem(tr("Indented: assemblies with their items below"), "indented");
  m_mode->setCurrentIndex(std::max(0, m_mode->findData(settings.value("bom/mode", "parts"))));
  left->addRow(tr("List"), m_mode);
  auto* scope = new QHBoxLayout();
  m_whole = new QRadioButton(tr("Whole document"), this);
  m_whole->setObjectName("bom.whole");
  m_selected = new QRadioButton(m_component.empty() ? tr("Selected component") : tr("Selected: %1").arg(doc->nodeName(m_component)), this);
  m_selected->setObjectName("bom.selected");
  m_selected->setEnabled(!m_component.empty());
  (m_component.empty() ? m_whole : m_selected)->setChecked(true);
  scope->addWidget(m_whole);
  scope->addWidget(m_selected);
  scope->addStretch();
  left->addRow(tr("Of"), scope);
  top->addLayout(left, 3);
  auto* right = new QFormLayout();
  auto* mass = new QHBoxLayout();
  m_mass = new QCheckBox(tr("Masses in"), this);
  m_mass->setObjectName("bom.mass");
  m_mass->setChecked(settings.value("bom/mass", true).toBool());
  m_unit = new QComboBox(this);
  m_unit->setObjectName("bom.unit");
  for (const char* u : {"g", "kg", "lb"}) m_unit->addItem(u, u);
  m_unit->setCurrentIndex(std::max(0, m_unit->findData(settings.value("bom/unit", "kg"))));
  mass->addWidget(m_mass);
  mass->addWidget(m_unit);
  mass->addStretch();
  right->addRow(mass);
  m_match = new QCheckBox(tr("Copies of one solid are one part"), this);
  m_match->setToolTip(tr("Pattern and mirror copies stored as their own geometry, and occurrences a STEP file wrote out, count as one part when the solid is the same (moved and turned, never mirrored)."));
  m_match->setChecked(settings.value("bom/match", true).toBool());
  right->addRow(m_match);
  m_references = new QCheckBox(tr("Mesh and drawing bodies"), this);
  m_references->setChecked(settings.value("bom/references", false).toBool());
  right->addRow(m_references);
  top->addLayout(right, 2);
  v->addLayout(top);

  m_table = new QTreeWidget(this);
  m_table->setObjectName("bom.preview");
  m_table->setHeaderLabels({tr("Item"), tr("Qty"), tr("Part number"), tr("Name"), tr("Material"), tr("Mass")});
  m_table->setRootIsDecorated(false);
  m_table->setUniformRowHeights(true);
  m_table->setSelectionMode(QAbstractItemView::NoSelection);
  m_table->header()->setStretchLastSection(false);
  m_table->header()->setSectionResizeMode(3, QHeaderView::Stretch);
  for (int c : {0, 1, 2, 4, 5}) m_table->header()->setSectionResizeMode(c, QHeaderView::ResizeToContents);
  v->addWidget(m_table, 1);

  auto* footer = new QHBoxLayout();
  m_status = new QLabel(this);
  m_status->setObjectName("secondary");
  footer->addWidget(m_status, 1);
  m_separator = new QComboBox(this);
  m_separator->setObjectName("bom.separator");
  m_separator->addItem(tr("Comma"), ",");
  m_separator->addItem(tr("Semicolon"), ";");
  m_separator->addItem(tr("Tab"), "\t");
  m_separator->setToolTip(tr("Between the columns of the CSV file: spreadsheets set to a decimal comma read semicolons"));
  m_separator->setCurrentIndex(std::max(0, m_separator->findData(settings.value("bom/separator", ","))));
  footer->addWidget(m_separator);
  auto* cancel = new QPushButton(tr("Cancel   Esc"), this);
  m_export = new QPushButton(tr("Export CSV…"), this);
  m_export->setObjectName("primary");
  m_export->setDefault(true);
  footer->addWidget(cancel);
  footer->addWidget(m_export);
  v->addLayout(footer);
  connect(cancel, &QPushButton::clicked, this, &QDialog::reject);
  connect(m_export, &QPushButton::clicked, this, &BomDialog::exportRequested);
  const auto changed = [this] {
    QSettings s;
    s.setValue("bom/mode", m_mode->currentData());
    s.setValue("bom/mass", m_mass->isChecked());
    s.setValue("bom/unit", m_unit->currentData());
    s.setValue("bom/match", m_match->isChecked());
    s.setValue("bom/references", m_references->isChecked());
    m_unit->setEnabled(m_mass->isChecked());
    compute();
  };
  connect(m_mode, &QComboBox::currentIndexChanged, this, changed);
  connect(m_unit, &QComboBox::currentIndexChanged, this, changed);
  connect(m_whole, &QRadioButton::toggled, this, changed);
  connect(m_mass, &QCheckBox::toggled, this, changed);
  connect(m_match, &QCheckBox::toggled, this, changed);
  connect(m_references, &QCheckBox::toggled, this, changed);
  connect(m_separator, &QComboBox::currentIndexChanged, this, [this] { QSettings().setValue("bom/separator", m_separator->currentData()); });
  m_unit->setEnabled(m_mass->isChecked());
  compute();
}

BomDialog::~BomDialog() {
  if (m_job) m_job->cancel();  // the document is released when its worker has stopped
}

opad::json BomDialog::options() const {
  return {{"mode", m_mode->currentData().toString().toStdString()}, {"root", m_selected->isChecked() ? m_component : std::string()},
          {"mass", m_mass->isChecked()}, {"mass_unit", m_unit->currentData().toString().toStdString()}, {"match_shapes", m_match->isChecked()},
          {"references", m_references->isChecked()}};
}

void BomDialog::compute() {
  m_ready = false;
  if (m_running) {  // one worker at a time: this one stops, the next starts when it has
    m_again = true;
    if (m_job) m_job->cancel();
    return;
  }
  const opad::json o = options();
  auto out = std::make_shared<opad::json>();
  QPointer<BomDialog> self(this);
  m_job = m_doc->readAsync(m_jobs, tr("Bill of materials"), [o, out](const opad::Document& doc, const opad::Scene& scene, Progress p) {
    opad::drawing::BomOptions b;
    b.mode = o["mode"].get<std::string>();
    b.root = o["root"].get<std::string>();
    b.mass = o["mass"].get<bool>();
    b.mass_unit = o["mass_unit"].get<std::string>();
    b.match_shapes = o["match_shapes"].get<bool>();
    b.references = o["references"].get<bool>();
    b.cancelled = [p] { return p.cancelled(); };
    *out = opad::drawing::bom(doc, scene, b);
  }, [self, out, o](bool ok, const QString& error) {
    if (!self) return;
    self->m_running = false;
    self->m_job = nullptr;
    if (self->m_again || (ok && o != self->options())) {
      self->m_again = false;
      return self->compute();
    }
    if (!ok) {
      self->m_status->setText(error == "cancelled" ? QString() : i18n::t(error));
      self->m_pending.clear();
      return;
    }
    self->m_bom = *out;
    self->m_ready = true;
    self->showBom();
    emit self->previewed();
    if (!self->m_pending.isEmpty()) self->write();
  });
  if (!m_job) {  // the document is busy (a load, a design change): ask again shortly
    m_status->setText(tr("Waiting for the document…"));
    QTimer::singleShot(300, this, &BomDialog::compute);
    return;
  }
  m_running = true;
  m_generation = m_doc->generation;
  m_revision = m_doc->revision;
  m_status->setText(o["mass"].get<bool>() ? tr("Counting parts and measuring their masses…") : tr("Counting parts…"));
}

void BomDialog::showBom() {
  const Tokens& t = theme::current();
  m_table->clear();
  const std::string unit = m_bom.value("mass_unit", "g");
  const bool indented = m_bom.value("mode", "") == "indented";
  m_table->setRootIsDecorated(indented);
  m_table->headerItem()->setText(5, m_bom["totals"].contains("mass") ? tr("Mass (%1)").arg(QString::fromStdString(unit)) : tr("Mass"));
  std::vector<QTreeWidgetItem*> open;  // indented: the last row of each level
  int missing = 0;
  for (const auto& r : m_bom["rows"]) {
    const size_t level = static_cast<size_t>(std::max(1, r.value("level", 1)));
    QTreeWidgetItem* parent = indented && level > 1 && open.size() >= level - 1 ? open[level - 2] : nullptr;
    auto* it = parent ? new QTreeWidgetItem(parent) : new QTreeWidgetItem(m_table);
    if (indented) {
      open.resize(level);
      open[level - 1] = it;
    }
    const auto text = [&](const char* k) { return QString::fromStdString(r.value(k, std::string())); };
    it->setText(0, text("item"));
    it->setText(1, QString::number(r.value("qty", 0LL)));
    it->setText(2, text("part_number"));
    it->setText(3, text("name"));
    it->setToolTip(3, text("description"));
    it->setText(4, r.contains("material_id") ? parts::materialName(r["material_id"].get<std::string>()) : text("material"));
    if (r.contains("mass")) {
      it->setText(5, massText(r["total_mass"], unit));
      it->setToolTip(5, tr("%1 %2 each").arg(massText(r["mass"], unit), QString::fromStdString(unit)));
    } else if (r.contains("mass_error")) {
      ++missing;
      it->setText(5, QString::fromUtf8("—"));
      it->setForeground(5, t.amber);
      it->setToolTip(5, i18n::t(QString::fromStdString(r["mass_error"].get<std::string>())));
    }
    if (r.value("kind", "") == "assembly") it->setFont(3, theme::ui(13, QFont::Medium));
    for (int c : {0, 1, 5}) it->setTextAlignment(c, Qt::AlignRight | Qt::AlignVCenter);
  }
  m_table->expandAll();
  const opad::json& totals = m_bom["totals"];
  QString status = tr("%1 rows · %2 parts").arg(totals.value("rows", 0)).arg(totals.value("parts", 0));
  if (totals.contains("mass") && missing < static_cast<int>(m_bom["rows"].size())) status += " · " + tr("%1 %2 in all").arg(massText(totals["mass"], unit), QString::fromStdString(unit));
  if (missing) status += " · " + tr("%1 without a mass (no material?)").arg(missing);
  m_status->setText(status);
}

// Headers, library materials and "yes" in the UI's language: the file is read by people (purchasing, the shop).
std::string BomDialog::csv() const {
  opad::json b = m_bom;
  for (auto& r : b["rows"])
    if (r.contains("material_id")) r["material"] = parts::materialName(r["material_id"].get<std::string>()).toStdString();
  opad::json labels = opad::json::object();
  for (const auto& [k, label] : std::initializer_list<std::pair<const char*, QString>>{
           {"item", tr("Item")}, {"level", tr("Level")}, {"qty", tr("Qty")}, {"total_qty", tr("Total qty")}, {"part_number", tr("Part number")},
           {"name", tr("Name")}, {"description", tr("Description")}, {"material", tr("Material")}, {"mass", tr("Mass")}, {"total_mass", tr("Total mass")},
           {"vendor", tr("Vendor")}, {"purchased", tr("Purchased")}, {"source", tr("Source")}, {"notes", tr("Notes")}, {"yes", tr("yes")}})
    labels[k] = label.toStdString();
  return opad::drawing::bom_csv(b, m_separator->currentData().toString().at(0).toLatin1(), labels);
}

void BomDialog::exportTo(const QString& path) {
  m_pending = path;
  if (m_ready && (m_generation != m_doc->generation || m_revision != m_doc->revision)) compute();  // changed since: list it again
  if (m_ready) write();
  else m_status->setText(tr("Exporting once the list is complete…"));
}

void BomDialog::write() {
  const QString path = std::exchange(m_pending, QString());
  const std::string text = csv();
  m_export->setEnabled(false);
  QPointer<BomDialog> self(this);
  m_jobs->async(tr("Writing the bill of materials"), [path, text](Progress) { opad::write_text_file(std::filesystem::path(path.toStdU16String()), text); },
                [self, path](bool ok, const QString& error) {
                  if (!self) return;
                  self->m_export->setEnabled(true);
                  emit self->exported(path, ok ? QString() : error);
                  if (ok) self->accept();
                  else self->m_status->setText(i18n::t(error));
                });
}
