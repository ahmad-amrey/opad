#include "DesignPanels.hpp"

#include <QCompleter>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QKeyEvent>
#include <QPainter>
#include <QStringListModel>

#include "I18n.hpp"
#include "Icons.hpp"
#include "Theme.hpp"

namespace {

opad::design::ParamTable paramTable(const opad::Scene& scene) {
  std::vector<opad::design::ParamDef> defs;
  for (const auto& p : scene.params) defs.push_back({p.id, p.name, p.expr, p.comment});
  return opad::design::ParamTable(defs);
}

// "three_points" -> "Three points": choice keys are identifiers in the op, words in the UI.
QString choiceLabel(const std::string& key) {
  QString s = QString::fromStdString(key).replace('_', ' ');
  if (!s.isEmpty()) s[0] = s[0].toUpper();
  return i18n::t(s);
}

bool singlePick(const std::string& type) { return type == "plane" || type == "axis" || type == "path"; }

}  // namespace

// ---------------------------------------------------------------- ExprEdit
ExprEdit::ExprEdit(AppDocument* doc, opad::design::Dim dim, QWidget* parent) : QWidget(parent), m_doc(doc), m_dim(dim) {
  auto* v = new QVBoxLayout(this);
  v->setContentsMargins(0, 0, 0, 0);
  v->setSpacing(1);
  m_edit = new QLineEdit(this);
  m_edit->setFont(theme::mono(12));
  m_value = new QLabel(this);
  m_value->setObjectName("tertiary");
  m_value->setFont(theme::mono(11));
  v->addWidget(m_edit);
  v->addWidget(m_value);
  QStringList names;
  for (const auto& p : doc->scene.params) names << QString::fromStdString(p.name);
  if (!names.isEmpty()) {
    auto* completer = new QCompleter(names, this);
    completer->setCaseSensitivity(Qt::CaseInsensitive);
    completer->setCompletionMode(QCompleter::InlineCompletion);  // a popup would fight with expressions ("width / 2")
    m_edit->setCompleter(completer);
  }
  connect(m_edit, &QLineEdit::textChanged, this, [this] { evaluate(); emit changed(); });
  connect(m_edit, &QLineEdit::returnPressed, this, &ExprEdit::returnPressed);
}

void ExprEdit::setText(const QString& text) {
  m_edit->setText(text);
  evaluate();
}

void ExprEdit::evaluate() {
  const Tokens& t = theme::current();
  try {
    const opad::design::ParamTable table = paramTable(m_doc->scene);
    const std::string text = m_edit->text().trimmed().toStdString();
    const double v = table.as(m_dim, text);
    opad::design::Quantity q{v, m_dim == opad::design::Dim::Length ? 1 : 0, m_dim == opad::design::Dim::Angle};
    m_value->setText(QString::fromUtf8("= ") + QString::fromStdString(opad::design::format_quantity(q)));
    m_value->setStyleSheet(QString("color: %1;").arg(theme::css(t.fg3)));
    m_valid = true;
  } catch (const std::exception& e) {
    m_value->setText(i18n::t(QString::fromUtf8(e.what())));
    m_value->setStyleSheet(QString("color: %1;").arg(theme::css(t.red)));
    m_valid = false;
  }
}

// ---------------------------------------------------------------- PickBox
PickBox::PickBox(QWidget* parent) : QPushButton(parent) {
  setFixedHeight(28);
  setCursor(Qt::PointingHandCursor);
  setFocusPolicy(Qt::NoFocus);
}

void PickBox::set(int count, const QString& what, bool active, bool satisfied) {
  m_count = count;
  m_what = what;
  m_active = active;
  m_satisfied = satisfied;
  update();
}

void PickBox::paintEvent(QPaintEvent*) {
  const Tokens& t = theme::current();
  QPainter p(this);
  p.setRenderHint(QPainter::Antialiasing);
  const QRectF r = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
  p.setPen(QPen(m_active ? t.sel : t.line, 1));
  p.setBrush(m_active ? t.selbg : t.bg);
  p.drawRoundedRect(r, 3, 3);
  p.drawPixmap(8, 6, icons::pixmap("cursor", m_active ? t.sel : t.fg2, 16, devicePixelRatioF()));
  p.setFont(theme::ui(12));
  p.setPen(m_count > 0 ? t.fg : m_satisfied ? t.fg3 : (m_active ? t.sel : t.fg2));
  const QString text = m_count > 0 ? (m_what.isEmpty() ? tr("%1 selected").arg(m_count) : m_what) : m_active ? tr("Pick in the view…") : m_satisfied ? tr("Optional") : tr("Select");
  p.drawText(QRect(30, 0, width() - 56, height()), Qt::AlignVCenter | Qt::AlignLeft, p.fontMetrics().elidedText(text, Qt::ElideRight, width() - 56));
  if (m_count > 0) p.drawPixmap(width() - 22, 6, icons::pixmap("close", t.fg2, 16, devicePixelRatioF()));
}

void PickBox::mousePressEvent(QMouseEvent* e) {
  if (m_count > 0 && e->pos().x() > width() - 26) return emit cleared();
  QPushButton::mousePressEvent(e);
}

// ---------------------------------------------------------------- FeaturePanel
FeaturePanel::FeaturePanel(AppDocument* doc, QWidget* parent) : QWidget(parent), m_doc(doc) {
  auto* v = new QVBoxLayout(this);
  v->setContentsMargins(12, 10, 12, 0);
  v->setSpacing(6);
  m_name = new QLineEdit(this);
  m_hint = new QLabel(this);
  m_hint->setObjectName("tertiary");
  m_hint->setWordWrap(true);
  v->addWidget(m_name);
  v->addWidget(m_hint);
  auto* body = new QWidget(this);
  m_rows = new QVBoxLayout(body);
  m_rows->setContentsMargins(0, 4, 0, 0);
  m_rows->setSpacing(6);
  v->addWidget(body);
  v->addStretch(1);
  m_status = new QLabel(this);
  m_status->setObjectName("tertiary");
  m_status->setWordWrap(true);
  v->addWidget(m_status);
  auto* footer = new QHBoxLayout();
  footer->setContentsMargins(0, 6, 0, 10);
  auto* cancel = new QPushButton(tr("Cancel   Esc"), this);
  m_ok = new QPushButton(tr("OK   Enter"), this);
  m_ok->setObjectName("primary");
  footer->addStretch(1);
  footer->addWidget(cancel);
  footer->addWidget(m_ok);
  v->addLayout(footer);
  connect(cancel, &QPushButton::clicked, this, &FeaturePanel::cancelled);
  connect(m_ok, &QPushButton::clicked, this, &FeaturePanel::accepted);
}

bool FeaturePanel::isPick(const std::string& type) {
  return type == "bodies" || type == "faces" || type == "edges" || type == "profiles" || type == "points" || type == "plane" || type == "axis" || type == "path";
}

const opad::design::InputSpec* FeaturePanel::input(const QString& name) const {
  if (!m_spec) return nullptr;
  for (const auto& in : m_spec->inputs)
    if (in.name == name.toStdString()) return &in;
  return nullptr;
}

void FeaturePanel::begin(const opad::design::FeatureSpec& spec, const opad::json& inputs, const QString& name, bool editing) {
  m_spec = &spec;
  m_values = inputs.is_object() ? inputs : opad::json::object();
  m_active.clear();
  m_widgets.clear();
  while (QLayoutItem* it = m_rows->takeAt(0)) {
    delete it->widget();
    delete it;
  }
  m_name->setText(name);
  m_hint->setText(i18n::t(QString::fromStdString(spec.hint)));
  m_ok->setText(editing ? tr("Update   Enter") : tr("OK   Enter"));
  setStatus(QString(), false);
  for (const auto& in : spec.inputs) {
    const QString key = QString::fromStdString(in.name);
    Row w;
    w.row = new QWidget(this);
    auto* h = new QHBoxLayout(w.row);
    h->setContentsMargins(0, 0, 0, 0);
    h->setSpacing(8);
    auto* label = new QLabel(i18n::t(QString::fromStdString(in.label)), w.row);
    label->setObjectName("secondary");
    label->setFixedWidth(118);
    label->setWordWrap(true);
    const opad::json value = m_values.contains(in.name) ? m_values[in.name] : in.def;
    if (in.type == "length" || in.type == "angle" || in.type == "number" || in.type == "count") {
      const auto dim = in.type == "length" ? opad::design::Dim::Length : in.type == "angle" ? opad::design::Dim::Angle : opad::design::Dim::None;
      w.expr = new ExprEdit(m_doc, dim, w.row);
      w.expr->setText(value.is_string() ? QString::fromStdString(value.get<std::string>()) : value.is_number() ? QString::number(value.get<double>()) : QString());
      connect(w.expr, &ExprEdit::changed, this, &FeaturePanel::inputsChanged);
      connect(w.expr, &ExprEdit::returnPressed, this, &FeaturePanel::accepted);
      h->addWidget(label, 0, Qt::AlignTop);
      h->addWidget(w.expr, 1);
    } else if (in.type == "choice") {
      w.combo = new QComboBox(w.row);
      for (const auto& c : in.choices) w.combo->addItem(choiceLabel(c), QString::fromStdString(c));
      const int at = w.combo->findData(value.is_string() ? QString::fromStdString(value.get<std::string>()) : QString());
      w.combo->setCurrentIndex(std::max(0, at));
      connect(w.combo, &QComboBox::currentIndexChanged, this, [this] { refreshVisibility(); emit inputsChanged(); });
      h->addWidget(label);
      h->addWidget(w.combo, 1);
    } else if (in.type == "bool") {
      w.check = new QCheckBox(i18n::t(QString::fromStdString(in.label)), w.row);
      w.check->setChecked(value.is_boolean() && value.get<bool>());
      connect(w.check, &QCheckBox::toggled, this, [this] { refreshVisibility(); emit inputsChanged(); });
      label->setText(QString());
      h->addWidget(label);
      h->addWidget(w.check, 1);
    } else if (isPick(in.type)) {
      w.pick = new PickBox(w.row);
      if (!m_values.contains(in.name) && !in.def.is_null()) m_values[in.name] = in.def;
      connect(w.pick, &QPushButton::clicked, this, [this, key] { activate(m_active == key ? QString() : key); });
      connect(w.pick, &PickBox::cleared, this, [this, key] {
        setPicks(key, opad::json());
        activate(key);
        emit inputsChanged();
      });
      h->addWidget(label);
      h->addWidget(w.pick, 1);
    } else {
      delete w.row;
      continue;
    }
    m_rows->addWidget(w.row);
    m_widgets[key] = w;
  }
  refreshVisibility();
  activateNextPick();
}

void FeaturePanel::refreshVisibility() {
  if (!m_spec) return;
  const opad::json now = inputs();
  for (const auto& in : m_spec->inputs) {
    auto it = m_widgets.find(QString::fromStdString(in.name));
    if (it == m_widgets.end()) continue;
    bool shown = true;
    if (!in.show_if.empty()) {
      const size_t eq = in.show_if.find('=');
      const std::string key = in.show_if.substr(0, eq);
      std::string have = now.contains(key) ? (now[key].is_string() ? now[key].get<std::string>() : now[key].dump()) : std::string();
      shown = false;
      for (const QString& v : QString::fromStdString(in.show_if.substr(eq + 1)).split('|'))
        if (v.toStdString() == have) shown = true;
    }
    it->second.row->setVisible(shown);
    if (!shown && m_active == it->first) activate(QString());
    if (it->second.pick) {
      const opad::json p = picks(it->first);
      const int n = p.is_array() ? static_cast<int>(p.size()) : p.is_null() ? 0 : 1;
      QString what;
      if (n == 1 && singlePick(in.type)) {
        const opad::json& one = p;
        if (one.contains("base")) what = (in.type == "plane" ? tr("%1 plane") : tr("%1 axis")).arg(QString::fromStdString(one["base"].get<std::string>()).toUpper());
        else if (one.contains("sketch")) what = tr("Sketch");
        else if (one.contains("feature")) what = tr("Construction");
        else what = in.type == "plane" ? tr("Face") : tr("Edge");
      }
      it->second.pick->set(n, what, m_active == it->first, in.optional || n >= std::max(1, in.min_count) || in.min_count == 0);
    }
  }
}

opad::json FeaturePanel::inputs() const {
  opad::json out = m_values;
  if (!m_spec) return out;
  for (const auto& in : m_spec->inputs) {
    auto it = m_widgets.find(QString::fromStdString(in.name));
    if (it == m_widgets.end()) continue;
    const Row& w = it->second;
    if (w.expr) out[in.name] = w.expr->text().toStdString();
    else if (w.combo) out[in.name] = w.combo->currentData().toString().toStdString();
    else if (w.check) out[in.name] = w.check->isChecked();
  }
  return out;
}

bool FeaturePanel::complete(QString* missing) const {
  if (!m_spec) return false;
  for (const auto& in : m_spec->inputs) {
    auto it = m_widgets.find(QString::fromStdString(in.name));
    if (it == m_widgets.end() || it->second.row->isHidden()) continue;
    const Row& w = it->second;
    const QString label = i18n::t(QString::fromStdString(in.label));
    if (w.expr && !w.expr->valid()) {
      if (missing) *missing = tr("%1: check the expression").arg(label);
      return false;
    }
    if (w.pick) {
      const opad::json p = picks(it->first);
      const int n = p.is_array() ? static_cast<int>(p.size()) : p.is_null() ? 0 : 1;
      if (n < in.min_count || (singlePick(in.type) && !in.optional && n == 0)) {
        if (missing) *missing = tr("Pick: %1").arg(label);
        return false;
      }
    }
  }
  return true;
}

void FeaturePanel::setPicks(const QString& name, const opad::json& picks) {
  const opad::design::InputSpec* in = input(name);
  if (!in) return;
  if (picks.is_null() || (picks.is_array() && picks.empty())) m_values.erase(in->name);
  else if (singlePick(in->type)) m_values[in->name] = picks.is_array() ? picks.back() : picks;
  else m_values[in->name] = picks;
  refreshVisibility();
}

opad::json FeaturePanel::picks(const QString& name) const {
  const std::string key = name.toStdString();
  return m_values.contains(key) ? m_values[key] : opad::json();
}

void FeaturePanel::setValue(const QString& name, const opad::json& value) {
  auto it = m_widgets.find(name);
  if (it == m_widgets.end()) return;
  if (it->second.expr && value.is_string()) it->second.expr->setText(QString::fromStdString(value.get<std::string>()));
  else if (it->second.combo && value.is_string()) it->second.combo->setCurrentIndex(std::max(0, it->second.combo->findData(QString::fromStdString(value.get<std::string>()))));
  else if (it->second.check && value.is_boolean()) it->second.check->setChecked(value.get<bool>());
  refreshVisibility();
  emit inputsChanged();
}

void FeaturePanel::setStatus(const QString& text, bool error) {
  const Tokens& t = theme::current();
  m_status->setText(text);
  m_status->setStyleSheet(QString("color: %1;").arg(theme::css(error ? t.red : t.fg3)));
}

void FeaturePanel::activate(const QString& name) {
  if (m_active == name) return;
  m_active = name;
  refreshVisibility();
  emit activeInputChanged(m_active);
}

void FeaturePanel::activateNextPick() {
  if (!m_spec) return;
  for (const auto& in : m_spec->inputs) {
    auto it = m_widgets.find(QString::fromStdString(in.name));
    if (it == m_widgets.end() || !it->second.pick || it->second.row->isHidden()) continue;
    const opad::json p = picks(it->first);
    const int n = p.is_array() ? static_cast<int>(p.size()) : p.is_null() ? 0 : 1;
    if (n < std::max(1, in.min_count) && !in.optional) return activate(it->first);
  }
  // Nothing is missing: stay on (or go to) the first pick input so clicks still mean something.
  if (!m_active.isEmpty()) return;
  for (const auto& in : m_spec->inputs) {
    auto it = m_widgets.find(QString::fromStdString(in.name));
    if (it != m_widgets.end() && it->second.pick && !it->second.row->isHidden()) return activate(it->first);
  }
}

void FeaturePanel::keyPressEvent(QKeyEvent* e) {
  if (e->key() == Qt::Key_Escape) return emit cancelled();
  if (e->key() == Qt::Key_Return || e->key() == Qt::Key_Enter) return emit accepted();
  QWidget::keyPressEvent(e);
}

// ---------------------------------------------------------------- ParametersDialog
ParametersDialog::ParametersDialog(AppDocument* doc, std::function<void(std::vector<opad::json>, QString)> apply, QWidget* parent)
    : QDialog(parent), m_doc(doc), m_apply(std::move(apply)) {
  setWindowTitle(tr("Parameters"));
  resize(720, 420);
  auto* v = new QVBoxLayout(this);
  v->setContentsMargins(16, 16, 16, 12);
  v->setSpacing(8);
  auto* intro = new QLabel(tr("User parameters can be used in every dimension and feature input: width / 2, 2 * PI * r, 30 deg. Double-click a cell to change it."), this);
  intro->setObjectName("secondary");
  intro->setWordWrap(true);
  v->addWidget(intro);
  m_table = new QTreeWidget(this);
  m_table->setColumnCount(5);
  m_table->setHeaderLabels({tr("Name"), tr("Expression"), tr("Value"), tr("Comment"), tr("Used by")});
  m_table->setRootIsDecorated(false);
  m_table->setAlternatingRowColors(false);
  m_table->setEditTriggers(QAbstractItemView::DoubleClicked | QAbstractItemView::EditKeyPressed | QAbstractItemView::SelectedClicked);
  m_table->header()->setSectionResizeMode(QHeaderView::Interactive);
  m_table->setColumnWidth(0, 130);
  m_table->setColumnWidth(1, 170);
  m_table->setColumnWidth(2, 100);
  m_table->setColumnWidth(3, 170);
  v->addWidget(m_table, 1);
  m_status = new QLabel(this);
  m_status->setObjectName("tertiary");
  m_status->setWordWrap(true);
  v->addWidget(m_status);
  auto* row = new QHBoxLayout();
  auto* add = new QPushButton(tr("Add parameter"), this);
  auto* remove = new QPushButton(tr("Delete"), this);
  auto* close = new QPushButton(tr("Close"), this);
  add->setObjectName("primary");
  row->addWidget(add);
  row->addWidget(remove);
  row->addStretch(1);
  row->addWidget(close);
  v->addLayout(row);
  connect(add, &QPushButton::clicked, this, &ParametersDialog::addParameter);
  connect(remove, &QPushButton::clicked, this, &ParametersDialog::removeCurrent);
  connect(close, &QPushButton::clicked, this, &QDialog::close);
  connect(m_table, &QTreeWidget::itemChanged, this, &ParametersDialog::itemEdited);
  connect(doc, &AppDocument::changed, this, &ParametersDialog::rebuild);
  rebuild();
}

void ParametersDialog::rebuild() {
  if (!isVisible() && m_table->topLevelItemCount() == 0 && m_doc->scene.params.empty()) return;
  const Tokens& t = theme::current();
  m_filling = true;
  const QString current = m_table->currentItem() ? m_table->currentItem()->data(0, Qt::UserRole).toString() : QString();
  m_table->clear();
  for (const auto& p : m_doc->scene.params) {
    auto* it = new QTreeWidgetItem(m_table);
    it->setFlags(it->flags() | Qt::ItemIsEditable);
    it->setData(0, Qt::UserRole, QString::fromStdString(p.id));
    it->setText(0, QString::fromStdString(p.name));
    it->setText(1, QString::fromStdString(p.expr));
    it->setFont(1, theme::mono(12));
    it->setText(2, p.error.empty() ? QString::fromStdString(p.shown) : i18n::t(QString::fromStdString(p.error)));
    it->setFont(2, theme::mono(12));
    it->setForeground(2, p.error.empty() ? t.fg2 : t.red);
    it->setText(3, QString::fromStdString(p.comment));
    QStringList users;
    for (const auto& u : opad::design::param_users(m_doc->doc, p.name)) users << QString::fromStdString(u);
    it->setText(4, users.join(", "));
    it->setForeground(4, t.fg3);
    if (it->data(0, Qt::UserRole).toString() == current) m_table->setCurrentItem(it);
  }
  m_filling = false;
}

void ParametersDialog::failed(const QString& error) {
  m_status->setText(i18n::t(error));
  m_status->setStyleSheet(QString("color: %1;").arg(theme::css(theme::current().red)));
  rebuild();  // put the refused text back to what the document holds
}

void ParametersDialog::addParameter() {
  m_status->clear();
  QString name;
  for (int i = 1;; ++i) {
    name = QString("d%1").arg(i);
    if (!m_doc->scene.param(name.toStdString())) break;
  }
  m_apply({opad::design::make_param_op(name.toStdString(), "10 mm")}, tr("new parameter"));
}

void ParametersDialog::removeCurrent() {
  m_status->clear();
  QTreeWidgetItem* it = m_table->currentItem();
  if (!it) return;
  const std::string name = it->text(0).toStdString();
  const auto users = opad::design::param_users(m_doc->doc, name);
  if (!users.empty()) return failed(tr("%1 is used by %2").arg(it->text(0), QString::fromStdString(users.front())));
  m_apply({opad::json{{"op", "delete"}, {"target", it->data(0, Qt::UserRole).toString().toStdString()}}}, tr("delete parameter"));
}

void ParametersDialog::itemEdited(QTreeWidgetItem* item, int column) {
  if (m_filling || column == 2 || column == 4) return;
  m_status->clear();
  const std::string id = item->data(0, Qt::UserRole).toString().toStdString();
  const opad::Param* p = nullptr;
  for (const auto& q : m_doc->scene.params)
    if (q.id == id) p = &q;
  if (!p) return;
  const std::string text = item->text(column).trimmed().toStdString();
  if (column == 0) {
    if (text == p->name) return;
    if (!opad::design::valid_param_name(text)) return failed(tr("“%1” is not a valid name: letters, digits and _, not a unit or a function.").arg(item->text(0)));
    if (m_doc->scene.param(text)) return failed(tr("A parameter called %1 already exists.").arg(item->text(0)));
    m_apply(opad::design::rename_param_ops(m_doc->doc, p->name, text), tr("rename parameter"));
  } else if (column == 1) {
    if (text == p->expr) return;
    m_apply({opad::design::make_edit_op(id, opad::json{{"expr", text}})}, tr("change %1").arg(QString::fromStdString(p->name)));
  } else if (column == 3) {
    if (text == p->comment) return;
    m_apply({opad::design::make_edit_op(id, opad::json{{"comment", text}})}, tr("comment"));
  }
}
