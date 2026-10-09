#include "PartProperties.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QPushButton>
#include <QVBoxLayout>

#include <algorithm>
#include <set>

#include "AppDocument.hpp"
#include "I18n.hpp"
#include "Theme.hpp"
#include "opad/drawing/bom.hpp"
#include "opad/materials.hpp"
#include "SearchCombo.hpp"

namespace {
constexpr const char* kContext = "PartProperties";
QString tr(const char* text) { return QCoreApplication::translate(kContext, text); }

QString num(double v) { return QString::number(v, 'g', 6); }

// A positive number as typed: a decimal comma, Arabic digits and the Arabic decimal separator are fine.
double positive(QString t, bool* ok) {
  for (QChar& c : t)
    if (c.unicode() >= 0x0660 && c.unicode() <= 0x0669) c = QChar('0' + (c.unicode() - 0x0660));
  t.replace(QChar(0x066B), '.').replace(',', '.');
  const double d = QLocale::c().toDouble(t.trimmed(), ok);
  *ok = *ok && d > 0;
  return d;
}

const opad::Material* library(const std::string& text) {
  if (const opad::Material* m = opad::material(text)) return m;
  for (const auto& m : opad::materials())
    if (m.name == text) return &m;
  return nullptr;
}
}  // namespace

namespace parts {

QString materialName(const std::string& text) {
  const opad::Material* m = library(text);
  return m ? i18n::t(QString::fromStdString(m->name)) : QString::fromStdString(text);
}

Section section(const opad::json& props) {
  const std::string type = props.value("type", "");
  if (type != "body" && type != "component") return {};
  Section s{tr("Part"), {}};
  const opad::json part = props.value("part", opad::json::object());
  const auto text = [&](const char* key) {
    const auto it = part.find(key);
    return it != part.end() && it->is_string() ? QString::fromStdString(it->get<std::string>()) : QString();
  };
  const auto add = [&](const QString& label, const QString& value) {
    if (!value.isEmpty()) s.rows << qMakePair(label, value);
  };
  add(tr("Part number"), text("part_number"));
  add(tr("Description"), text("description"));
  if (const QString m = text("material"); !m.isEmpty()) add(tr("Material"), materialName(m.toStdString()));
  if (const double d = opad::property_number(part.value("density", opad::json())); d > 0) add(tr("Density"), num(d) + QString::fromUtf8(" g/cm³"));
  if (const double m = opad::property_number(part.value("mass", opad::json())); m > 0) add(tr("Mass given"), num(m) + " g");
  add(tr("Vendor"), text("vendor"));
  add(tr("Notes"), text("notes"));
  if (text("bom") == "exclude") add(tr("Bill of materials"), tr("Left out"));
  if (text("bom") == "purchased") add(tr("Bill of materials"), tr("Purchased, as one part"));
  if (part.value("section", opad::json()) == false) add(tr("Section views"), tr("Never cut"));
  static const std::set<std::string> own = {"part_number", "description", "material", "density", "mass", "vendor", "notes", "bom", "section"};
  for (auto it = part.begin(); it != part.end(); ++it)  // custom fields (CLI, agents), as named
    if (!own.count(it.key())) add(QString::fromStdString(it.key()), it->is_string() ? QString::fromStdString(it->get<std::string>()) : QString::fromStdString(it->dump()));
  return s;
}

}  // namespace parts

PartPropertiesDialog::PartPropertiesDialog(AppDocument* doc, std::vector<std::string> nodes, QWidget* parent)
    : QDialog(parent), m_doc(doc), m_nodes(std::move(nodes)) {
  setObjectName("partProperties");
  setWindowTitle(m_nodes.size() == 1 ? tr("Part properties · %1").arg(doc->nodeName(m_nodes.front())) : tr("Part properties · %1 objects").arg(m_nodes.size()));
  setMinimumWidth(480);
  m_shared = opad::drawing::shared_part_properties(doc->scene, m_nodes);
  const opad::json values = m_shared["values"];
  std::set<std::string> mixed;
  for (const auto& k : m_shared["mixed"]) mixed.insert(k.get<std::string>());
  auto* v = new QVBoxLayout(this);
  v->setContentsMargins(16, 16, 16, 16);
  v->setSpacing(6);
  auto* form = new QFormLayout();  // one form: the sections' fields line up
  form->setHorizontalSpacing(12);
  form->setVerticalSpacing(6);
  v->addLayout(form);
  const auto header = [&](const QString& t) {
    auto* l = new QLabel(t, this);
    l->setObjectName("sectionHeader");
    form->addRow(l);
  };
  // A field starts as the value the nodes share; one they differ in starts empty and says so, and stays as each has it
  // unless something is typed.
  const auto line = [&](const char* key) {
    auto* e = new QLineEdit(this);
    e->setObjectName(QString("part.") + key);
    const opad::json value = values.value(key, opad::json());
    if (value.is_string()) e->setText(QString::fromStdString(value.get<std::string>()));
    if (value.is_number()) e->setText(num(value.get<double>()));
    e->setProperty("initial", e->text());
    if (mixed.count(key)) e->setPlaceholderText(tr("Several values"));
    connect(e, &QLineEdit::textChanged, this, &PartPropertiesDialog::refresh);
    return e;
  };
  header(tr("PART"));
  form->addRow(tr("Part number"), m_number = line("part_number"));
  form->addRow(tr("Description"), m_description = line("description"));
  m_section = new QCheckBox(tr("Never cut in section views (shafts, pins, keys, fasteners)"), this);
  m_section->setObjectName("part.section");
  m_section->setToolTip(tr("ISO 128-50: drawn whole wherever a section or a broken-out section passes through it; a view can still cut it"));
  if (mixed.count("section")) {
    m_section->setTristate(true);
    m_section->setCheckState(Qt::PartiallyChecked);
  } else {
    m_section->setChecked(values.value("section", opad::json()) == false);
  }
  connect(m_section, &QCheckBox::stateChanged, this, &PartPropertiesDialog::refresh);
  form->addRow(QString(), m_section);

  header(tr("MATERIAL"));
  m_material = new QComboBox(this);
  m_material->setObjectName("part.material");
  m_material->setEditable(true);
  m_material->setInsertPolicy(QComboBox::NoInsert);
  m_material->addItem(QString(), QString());
  for (const auto& m : opad::materials()) m_material->addItem(i18n::t(QString::fromStdString(m.name)), QString::fromStdString(m.id));
  m_material->setCurrentIndex(0);
  if (const opad::json m = values.value("material", opad::json()); m.is_string()) {
    const QString shown = parts::materialName(m.get<std::string>());
    const int i = m_material->findText(shown, Qt::MatchFixedString);
    if (i > 0) m_material->setCurrentIndex(i);
    else m_material->setEditText(shown);
  }
  m_material->lineEdit()->setPlaceholderText(mixed.count("material") ? tr("Several values") : tr("None (from the component or the file)"));
  search_combo::enable(m_material, true);  // any text is a material too: what is typed stays
  m_materialShown = m_material->currentText();
  connect(m_material, &QComboBox::currentTextChanged, this, &PartPropertiesDialog::refresh);
  form->addRow(tr("Material"), m_material);
  m_materialHint = new QLabel(this);
  m_materialHint->setObjectName("secondary");
  m_materialHint->setWordWrap(true);
  form->addRow(QString(), m_materialHint);
  form->addRow(QString::fromUtf8("%1 (g/cm³)").arg(tr("Density")), m_density = line("density"));
  form->addRow(tr("Mass (g)"), m_mass = line("mass"));
  if (!mixed.count("mass")) m_mass->setPlaceholderText(tr("From the volume and the density"));
  m_appearance = new QCheckBox(tr("Colour as the material"), this);
  m_appearance->setObjectName("part.appearance");
  connect(m_appearance, &QCheckBox::toggled, this, &PartPropertiesDialog::refresh);
  form->addRow(QString(), m_appearance);

  header(tr("PURCHASING"));
  form->addRow(tr("Vendor"), m_vendor = line("vendor"));
  form->addRow(tr("Notes"), m_notes = line("notes"));
  m_bom = new QComboBox(this);
  m_bom->setObjectName("part.bom");
  if (mixed.count("bom")) m_bom->addItem(tr("Several values"), QString());
  m_bom->addItem(tr("Listed"), "include");
  m_bom->addItem(tr("Left out"), "exclude");
  m_bom->addItem(tr("Purchased, as one part (its contents not listed)"), "purchased");
  if (!mixed.count("bom")) m_bom->setCurrentIndex(std::max(0, m_bom->findData(QString::fromStdString(values.value("bom", std::string("include"))))));
  connect(m_bom, &QComboBox::currentIndexChanged, this, &PartPropertiesDialog::refresh);
  form->addRow(tr("Bill of materials"), m_bom);

  m_error = new QLabel(this);
  m_error->setObjectName("part.error");
  m_error->setWordWrap(true);
  m_error->setStyleSheet(QString("color: %1;").arg(theme::current().red.name()));
  v->addWidget(m_error);
  v->addStretch();
  auto* footer = new QHBoxLayout();
  footer->addStretch();
  auto* cancel = new QPushButton(tr("Cancel   Esc"), this);
  m_apply = new QPushButton(tr("Apply   Enter"), this);
  m_apply->setObjectName("primary");
  m_apply->setDefault(true);
  footer->addWidget(cancel);
  footer->addWidget(m_apply);
  v->addLayout(footer);
  connect(cancel, &QPushButton::clicked, this, &QDialog::reject);
  connect(m_apply, &QPushButton::clicked, this, &PartPropertiesDialog::apply);
  refresh();
}

opad::json PartPropertiesDialog::fields() const {
  const opad::json& values = m_shared["values"];
  opad::json f = opad::json::object();
  for (auto [key, edit] : {std::pair<const char*, QLineEdit*>{"part_number", m_number}, {"description", m_description}, {"vendor", m_vendor}, {"notes", m_notes}})
    f[key] = edit->text().toStdString();
  // Numbers: as typed, or as stored while the text is the one shown at first (it may have been rounded for showing).
  for (auto [key, edit] : {std::pair<const char*, QLineEdit*>{"density", m_density}, {"mass", m_mass}}) {
    bool ok = false;
    const double d = positive(edit->text(), &ok);
    if (edit->text() == edit->property("initial").toString()) f[key] = values.value(key, opad::json());
    else if (edit->text().trimmed().isEmpty()) f[key] = nullptr;
    else f[key] = ok ? opad::json(d) : opad::json(edit->text().toStdString());  // text: refresh() refuses it
  }
  const QString shown = m_material->currentText().trimmed();
  if (shown == m_materialShown.trimmed()) {
    f["material"] = values.value("material", opad::json());
  } else {
    const int i = m_material->findText(shown, Qt::MatchFixedString);
    f["material"] = i > 0 ? m_material->itemData(i).toString().toStdString() : shown.toStdString();  // a library material by id
  }
  if (const QString b = m_bom->currentData().toString(); !b.isEmpty()) f["bom"] = b == "include" ? opad::json() : opad::json(b.toStdString());
  if (m_section->checkState() != Qt::PartiallyChecked) {  // ticked: false; unticked: whatever it had unless that was false
    const opad::json was = values.value("section", opad::json());
    f["section"] = m_section->isChecked() ? opad::json(false) : was == false ? opad::json() : was;
  }
  return f;
}

opad::json PartPropertiesDialog::command() const {
  const opad::json f = fields();
  opad::json set = opad::drawing::part_properties_change(m_shared, f);
  const bool colour = m_appearance->isChecked() && m_appearance->isEnabled();
  if (colour) set["material"] = f["material"];  // part_properties colours by the material it sets
  if (set.empty()) return nullptr;
  opad::json args = {{"targets", m_nodes}, {"set", set}};
  if (colour) args["appearance"] = true;
  return args;
}

void PartPropertiesDialog::refresh() {
  const opad::json f = fields();
  QStringList errors;
  if (f["density"].is_string()) errors << tr("Density is a positive number, in g/cm³.");
  if (f["mass"].is_string()) errors << tr("Mass is a positive number, in grams.");
  const opad::json chosen = f["material"];
  const opad::Material* lib = chosen.is_string() ? opad::find_material(chosen.get<std::string>()) : nullptr;
  QString hint;
  if (lib) {
    hint = tr("%1 · %2 g/cm³").arg(i18n::t(QString::fromStdString(lib->name)), num(lib->density));
    if (lib->id != chosen.get<std::string>() && lib->name != chosen.get<std::string>()) hint += " · " + tr("taken as the library's");
  } else if (chosen.is_string() && !chosen.get<std::string>().empty()) {
    hint = tr("Not in the library: give its density for the mass.");
  } else if (m_nodes.size() == 1) {
    // Nothing set here: what decides now (a component above it, or the material its file names).
    const opad::MaterialChoice now = opad::material_of(m_doc->doc, m_doc->scene, m_nodes.front());
    if (!now.text.empty() && now.from != m_nodes.front())
      hint = tr("Now %1, from %2").arg(parts::materialName(now.shown()), now.from.empty() ? tr("its file") : m_doc->nodeName(now.from));
  }
  m_materialHint->setText(hint);
  m_materialHint->setVisible(!hint.isEmpty());
  if (const opad::json& mixed = m_shared["mixed"]; std::find(mixed.begin(), mixed.end(), "density") == mixed.end())
    m_density->setPlaceholderText(lib ? tr("%1, the material's").arg(num(lib->density)) : QString());
  m_appearance->setEnabled(lib != nullptr);
  m_error->setText(errors.join('\n'));
  m_error->setVisible(!errors.isEmpty());
  m_apply->setEnabled(errors.isEmpty() && !command().is_null());
}

void PartPropertiesDialog::apply() {
  const opad::json args = command();
  if (args.is_null()) return reject();
  try {
    m_doc->run("part_properties", args);
  } catch (const std::exception& e) {
    m_error->setText(i18n::t(QString::fromUtf8(e.what())));
    m_error->show();
    return;
  }
  emit applied();
  accept();
}
