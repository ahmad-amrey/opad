#include "DesignPanels.hpp"

#include <QColorDialog>
#include <QCompleter>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QKeyEvent>
#include <QResizeEvent>
#include <QScrollArea>
#include <QPainter>
#include <QSettings>
#include <QStringListModel>

#include <algorithm>
#include <cmath>
#include <set>

#include "HelpClip.hpp"
#include "I18n.hpp"
#include "Icons.hpp"
#include "KeyText.hpp"
#include "Theme.hpp"
#include "Units.hpp"

namespace {

opad::design::ParamTable paramTable(const opad::Scene& scene) {
  std::vector<opad::design::ParamDef> defs;
  for (const auto& p : scene.params) defs.push_back({p.id, p.name, p.expr, p.comment});
  return opad::design::ParamTable(defs,scene.units);
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
  m_value->setFixedHeight(14);  // a row of its own under the box, never on its border
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

QString ExprEdit::text() const {
  const auto text=m_edit->text().trimmed();
  if(m_dim!=opad::design::Dim::Length)return text;
  try{return QString::fromStdString(paramTable(m_doc->scene).explicit_length(text.toStdString()));}catch(...){return text;}
}
void ExprEdit::evaluate() {
  const Tokens& t = theme::current();
  try {
    const opad::design::ParamTable table = paramTable(m_doc->scene);
    const std::string text = m_edit->text().trimmed().toStdString();
    const double v = table.as(m_dim, text);
    opad::design::Quantity q{v, m_dim == opad::design::Dim::Length ? 1 : 0, m_dim == opad::design::Dim::Angle};
    // Lengths and angles in the shown unit and precision (UI-123), anything else as the expression engine says it.
    const QString shown = m_dim == opad::design::Dim::Length  ? units::format(units::Kind::Length, v)
                          : m_dim == opad::design::Dim::Angle ? units::format(units::Kind::Angle, v * 180 / M_PI)
                                                              : QString::fromStdString(opad::design::format_quantity(q));
    // A number as the box would start it ("10 mm", or a bare 10 in the shown unit) says nothing the echo would add: the
    // echo stays empty, its row kept so the panel does not jump while typing.
    const QString typed = m_edit->text().trimmed();
    bool bare = false;
    const double number = typed.toDouble(&bare);
    const double expected = m_dim == opad::design::Dim::Length  ? units::toDisplay(units::Kind::Length, v)
                            : m_dim == opad::design::Dim::Angle ? units::toDisplay(units::Kind::Angle, v * 180 / M_PI)
                                                                : v;
    const auto squeezed = [](QString s) { return s.remove(' '); };
    const bool same = (bare && std::fabs(number - expected) <= 1e-9 * std::max(1.0, std::fabs(expected))) || squeezed(typed) == squeezed(shown) ||
                      (m_dim == opad::design::Dim::Length && squeezed(typed) == squeezed(units::editable(units::Kind::Length, v))) ||
                      (m_dim == opad::design::Dim::Angle && squeezed(typed) == squeezed(units::editable(units::Kind::Angle, v * 180 / M_PI)));
    m_value->setText(same ? QString() : QString::fromUtf8("= ") + shown);
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
  setToolTip(count > 0 ? what : QString());  // the whole of it when the box is too narrow ("… moves as one")
  update();
}

void PickBox::setNote(const QString& note) {
  if (m_note == note) return;
  m_note = note;
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
  QString text = m_count > 0 ? (m_what.isEmpty() ? tr("%1 selected").arg(m_count) : m_what) : m_active ? tr("Pick in the view…") : m_satisfied ? tr("Optional") : tr("Select");
  if (!m_note.isEmpty()) {
    text = m_note;
    p.setPen(t.sel);
  }
  const bool clears = m_count > 0 && m_note.isEmpty();
  const int room = width() - 30 - (clears ? 26 : 8);  // the clear button's place only when there is something to clear
  p.drawText(QRect(30, 0, room, height()), Qt::AlignVCenter | Qt::AlignLeft, p.fontMetrics().elidedText(text, Qt::ElideRight, room));
  if (clears) p.drawPixmap(width() - 22, 6, icons::pixmap("close", t.fg2, 16, devicePixelRatioF()));
}

void PickBox::mousePressEvent(QMouseEvent* e) {
  if (m_count > 0 && m_note.isEmpty() && e->pos().x() > width() - 26) return emit cleared();
  QPushButton::mousePressEvent(e);
}

// ---------------------------------------------------------------- FeaturePanel
FeaturePanel::FeaturePanel(AppDocument* doc, QWidget* parent) : QWidget(parent), m_doc(doc) {
  auto* outer = new QVBoxLayout(this);
  outer->setContentsMargins(0, 0, 0, 0);
  outer->setSpacing(0);
  // The form scrolls when the panel is shorter than it (a small view, the guide open): squeezed, its rows overlapped
  // (the value echo sat on its box's border, UI-116).
  m_scroll = new QScrollArea(this);
  m_scroll->setFrameShape(QFrame::NoFrame);
  m_scroll->setWidgetResizable(true);
  m_scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  m_scroll->setMinimumSize(0, 0);
  auto* form = m_form = new QWidget(m_scroll);
  form->setAutoFillBackground(false);
  m_scroll->setWidget(form);
  m_scroll->viewport()->setAutoFillBackground(false);
  outer->addWidget(m_scroll, 1);
  auto* v = new QVBoxLayout(form);
  v->setContentsMargins(12, 10, 12, 8);
  v->setSpacing(6);
  m_name = new QLineEdit(this);
  m_hint = new QLabel(this);
  m_hint->setObjectName("tertiary");
  m_hint->setWordWrap(true);
  v->addWidget(m_name);
  v->addWidget(m_hint);
  m_guide = new ToolGuide(this);
  v->addWidget(m_guide);
  connect(m_guide, &ToolGuide::resized, this, &FeaturePanel::contentResized);
  m_hiddenWarning=new QLabel(tr("The object being edited is hidden. Show it in the browser to see the result."),this);m_hiddenWarning->setWordWrap(true);m_hiddenWarning->hide();v->addWidget(m_hiddenWarning);
  auto* body = new QWidget(this);
  m_rows = new QVBoxLayout(body);
  m_rows->setContentsMargins(0, 4, 0, 0);
  m_rows->setSpacing(6);
  v->addWidget(body);
  // New body (TODO 10 B14): one line until it is opened; what is set there overrides the defaults.
  m_newBody = new QWidget(this);
  auto* nb = new QVBoxLayout(m_newBody);
  nb->setContentsMargins(0, 8, 0, 0);
  nb->setSpacing(6);
  m_newBodyToggle = new QCheckBox(m_newBody);
  m_newBodyToggle->setToolTip(tr("Name, colour and component of the bodies this feature makes"));
  nb->addWidget(m_newBodyToggle);
  m_newBodyRows = new QWidget(m_newBody);
  auto* rows = new QVBoxLayout(m_newBodyRows);
  rows->setContentsMargins(0, 0, 0, 0);
  rows->setSpacing(6);
  auto row = [&](const QString& text, QWidget* field, QWidget* extra = nullptr) {
    auto* h = new QHBoxLayout();
    h->setSpacing(8);
    auto* label = new QLabel(text, m_newBodyRows);
    label->setObjectName("secondary");
    label->setFixedWidth(118);
    h->addWidget(label);
    h->addWidget(field, 1);
    if (extra) h->addWidget(extra);
    rows->addLayout(h);
  };
  m_bodyName = new QLineEdit(m_newBodyRows);
  m_bodyName->setToolTip(tr("Several bodies are numbered; {n} marks where the number goes"));
  row(tr("Name"), m_bodyName);
  m_bodyColour = new QPushButton(m_newBodyRows);
  m_bodyColourReset = new QPushButton(tr("Automatic"), m_newBodyRows);
  m_bodyColourReset->setToolTip(tr("Back to the automatic colour"));
  row(tr("Colour"), m_bodyColour, m_bodyColourReset);
  m_bodyParent = new QComboBox(m_newBodyRows);
  row(tr("Component"), m_bodyParent);
  nb->addWidget(m_newBodyRows);
  v->addWidget(m_newBody);
  v->addStretch(1);
  m_newBodyToggle->setChecked(QSettings().value("design/newBodyOpen", false).toBool());
  connect(m_newBodyToggle, &QCheckBox::toggled, this, [this](bool on) {
    QSettings().setValue("design/newBodyOpen", on);
    refreshNewBody();
  });
  connect(m_name, &QLineEdit::textChanged, this, [this] { refreshNewBody(); });
  connect(m_bodyParent, &QComboBox::currentIndexChanged, this, [this] { refreshNewBody(); });
  connect(m_bodyColour, &QPushButton::clicked, this, [this] {
    const QColor chosen = QColorDialog::getColor(m_colour.isValid() ? m_colour : QColor(190, 190, 195), this, tr("Colour of the new bodies"));
    if (chosen.isValid()) setBodyColour(chosen);
  });
  connect(m_bodyColourReset, &QPushButton::clicked, this, [this] { setBodyColour(QColor()); });
  m_status = new QLabel(this);
  m_status->setObjectName("tertiary");
  m_status->setWordWrap(true);
  v->addWidget(m_status);
  m_footer = new PanelFooter(this);  // Cancel (Esc) and OK (Enter): it commits and closes the panel
  outer->addWidget(m_footer);
  connect(m_footer, &PanelFooter::cancelled, this, &FeaturePanel::cancelled);
  connect(m_footer, &PanelFooter::accepted, this, &FeaturePanel::accepted);
}

void FeaturePanel::setEditHidden(bool hidden){
  m_hiddenWarning->setStyleSheet(QString("color: %1;").arg(theme::css(theme::current().warning)));  // the theme's warning, as it is now
  m_hiddenWarning->setVisible(hidden);
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
  m_notes.clear();
  m_suggested.clear();
  m_guideStep = m_guideCount = 0;
  while (QLayoutItem* it = m_rows->takeAt(0)) {
    delete it->widget();
    delete it;
  }
  m_name->setText(name);
  m_editingFeature = editing;
  m_bodyName->clear();
  m_colour = QColor();
  {
    const QSignalBlocker quiet(m_bodyParent);
    m_bodyParent->clear();
    if (makesCopies()) m_bodyParent->addItem(tr("With the picked bodies"), QString("*"));
    m_bodyParent->addItem(tr("Document root"), QString());
    std::vector<std::pair<QString, QString>> components;
    for (const auto& [id, n] : m_doc->scene.nodes)
      if (n.kind == opad::Node::Kind::Component) {
        QStringList path;
        for (const auto& p : m_doc->scene.path_to(id)) path << m_doc->nodeName(p);
        components.push_back({path.join(QString::fromUtf8(" › ")), QString::fromStdString(id)});
      }
    std::sort(components.begin(), components.end());
    for (const auto& [label, id] : components) m_bodyParent->addItem(label, id);
    m_bodyParent->setCurrentIndex(0);
  }
  // A feature that picks on bodies: its preview stands in for them, and Ctrl held shows them as they are to pick more.
  const bool picks = std::any_of(spec.inputs.begin(), spec.inputs.end(), [](const auto& in) { return in.type == "bodies" || in.type == "faces" || in.type == "edges" || in.type == "points"; });
  m_hint->setText(i18n::t(QString::fromStdString(spec.hint)) +
                  (picks ? "\n" + tr("Hold %1 to see the bodies without the preview and pick more on them.").arg(keys::fixedText("ctrl")) : QString()));
  m_guide->setCommand(editing ? QString() : "design." + QString::fromStdString(spec.kind));
  m_footer->setPrimary(PanelFooter::Primary::Close);setEditHidden(false);
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
      QString shown=value.is_string()?QString::fromStdString(value.get<std::string>()):value.is_number()?QString::number(value.get<double>()):QString();
      if(dim==opad::design::Dim::Length)try {shown=QString::fromStdString(opad::design::ParamTable(paramTable(m_doc->scene).defs()).explicit_length(shown.toStdString()));}catch(...){}
      w.expr->setText(shown);
      connect(w.expr, &ExprEdit::changed, this, &FeaturePanel::inputsChanged);
      connect(w.expr, &ExprEdit::returnPressed, this, &FeaturePanel::accepted);
      h->addWidget(label, 0, Qt::AlignTop);
      h->addWidget(w.expr, 1);
    } else if (in.type == "choice") {
      w.combo = new QComboBox(w.row);
      // "auto" (the extrusion's automatic operation) heads the list, saying what it was taken as (setSuggestion).
      std::vector<std::string> order = in.choices;
      if (const auto a = std::find(order.begin(), order.end(), "auto"); a != order.end()) std::rotate(order.begin(), a, a + 1);
      for (const auto& c : order) w.combo->addItem(c == "auto" ? tr("Automatic") : choiceLabel(c), QString::fromStdString(c));
      const int at = w.combo->findData(value.is_string() ? QString::fromStdString(value.get<std::string>()) : QString());
      w.combo->setCurrentIndex(std::max(0, at));
      connect(w.combo, &QComboBox::currentIndexChanged, this, [this] {
        const auto shown = shownPicks();
        const QString was = m_active;
        refreshVisibility();
        focusRevealed(shown, was);
        emit inputsChanged();
      });
      h->addWidget(label);
      h->addWidget(w.combo, 1);
    } else if (in.type == "bool") {
      w.check = new QCheckBox(i18n::t(QString::fromStdString(in.label)), w.row);
      w.check->setChecked(value.is_boolean() && value.get<bool>());
      connect(w.check, &QCheckBox::toggled, this, [this] {
        const auto shown = shownPicks();
        const QString was = m_active;
        refreshVisibility();
        focusRevealed(shown, was);
        emit inputsChanged();
      });
      label->setText(QString());
      h->addWidget(label);
      h->addWidget(w.check, 1);
    } else if (isPick(in.type)) {
      w.pick = new PickBox(w.row);
      if (!m_values.contains(in.name) && !in.def.is_null()) m_values[in.name] = in.def;
      connect(w.pick, &QPushButton::clicked, this, [this, key] { activate(m_active == key ? QString() : key); });
      connect(w.pick, &PickBox::cleared, this, [this, key] {
        setPicks(key, opad::json());
        // Already active: say so again, so the view drops the picks too (they stayed selected, and the next click
        // added to them: a cleared shell face came back as "2 selected").
        if (m_active == key) emit activeInputChanged(key);
        else activate(key);
        emit inputsChanged();
      });
      h->addWidget(label);
      h->addWidget(w.pick, 1);
      if (in.type == "faces" || in.type == "edges") {
        w.rule = new QPushButton(tr("By rule…"), w.row);
        w.rule->setToolTip(tr("Pick every face or edge like the picked one by a rule the feature keeps: it picks them again when the body changes"));
        connect(w.rule, &QPushButton::clicked, this, [this, key, button = w.rule] { emit ruleRequested(key, button); });
        h->addWidget(w.rule);
      }
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
      int n = p.is_array() ? static_cast<int>(p.size()) : p.is_null() ? 0 : 1;
      QString what;
      // A rule stands for what it matched; one plain pick can become a rule.
      bool rule = false, plain_one = n == 1 && p.is_array() && p[0].is_object() && p[0].value("kind", "") != "body" && p[0].contains("index");
      if (p.is_array())
        for (const auto& one : p)
          if (one.is_object() && one.contains("select")) rule = true;
      if (rule) {
        int count = 0;
        for (const auto& one : p) count += one.is_object() && one.contains("select") ? one.value("expect", 1) : 1;
        n = count;
        what = tr("%1 by rule").arg(count);
      }
      if (it->second.rule) it->second.rule->setEnabled(plain_one && !rule);
      // A face input that takes planes too (To face): an origin or construction plane says so.
      if (!rule && n == 1 && !singlePick(in.type) && p.is_array() && p[0].is_object() && (p[0].contains("base") || p[0].contains("feature")))
        what = p[0].contains("base") ? tr("%1 plane").arg(QString::fromStdString(p[0]["base"].get<std::string>()).toUpper()) : tr("Construction");
      if (!rule && n == 1 && singlePick(in.type)) {
        const opad::json& one = p;
        if (one.contains("base")) what = (in.type == "plane" ? tr("%1 plane") : tr("%1 axis")).arg(QString::fromStdString(one["base"].get<std::string>()).toUpper());
        else if (one.contains("sketch")) what = tr("Sketch");
        else if (one.contains("feature")) what = tr("Construction");
        else if (one.contains("direction")) what = tr("Direction");
        else what = in.type == "plane" || one.contains("face") ? tr("Face") : tr("Edge");  // an axis through a round face
      }
      if (!rule && in.type == "bodies" && m_spec->kind == "move") what = linkedMoveText(p, what);
      it->second.pick->set(n, what, m_active == it->first, in.optional || n >= std::max(1, in.min_count) || in.min_count == 0);
      const auto note = m_notes.find(it->first);
      it->second.pick->setNote(note == m_notes.end() ? QString() : note->second);
    }
  }
  // The guide's steps: the shown picks the feature needs, in order, then its values; it waits at the first one missing.
  int needed = 0, waiting = -1;
  for (const auto& in : m_spec->inputs) {
    auto it = m_widgets.find(QString::fromStdString(in.name));
    if (it == m_widgets.end() || !it->second.pick || it->second.row->isHidden() || in.optional || (in.min_count < 1 && !singlePick(in.type))) continue;
    const opad::json p = picks(it->first);
    if (waiting < 0 && (p.is_array() ? static_cast<int>(p.size()) : p.is_null() ? 0 : 1) < std::max(1, in.min_count)) waiting = needed;
    ++needed;
  }
  if (m_guideCount > 0) m_guide->setWaiting(m_guideStep, m_guideCount);  // a primitive being placed: its stage
  else m_guide->setWaiting(waiting < 0 ? needed : waiting, needed + 1);
  refreshNewBody();
}

bool FeaturePanel::makesCopies() const {
  return m_spec && (m_spec->kind == "mirror" || m_spec->kind == "pattern_rect" || m_spec->kind == "pattern_circ" || m_spec->kind == "pattern_points");
}

void FeaturePanel::setBodyDefaults(const std::string& component) {
  if (makesCopies() || component.empty()) return;  // copies go with the bodies they copy
  const int at = m_bodyParent->findData(QString::fromStdString(component));
  if (at >= 0) m_bodyParent->setCurrentIndex(at);
  refreshNewBody();
}

void FeaturePanel::setBodyName(const QString& name) {
  m_bodyName->setText(name);
  if (!name.isEmpty()) m_newBodyToggle->setChecked(true);
}

void FeaturePanel::setBodyColour(const QColor& colour) {
  m_colour = colour;
  if (colour.isValid()) m_newBodyToggle->setChecked(true);
  refreshNewBody();
}

void FeaturePanel::refreshNewBody() {
  bool shown = false;
  if (m_spec && !m_editingFeature)
    for (const auto& in : m_spec->inputs)
      if (in.name == "operation") {
        // Automatic: while it makes a new body (or before the preview has said what it makes).
        const std::string op = inputs().value("operation", "");
        const QString decided = suggestion("operation");
        shown = op == "new" || (op == "auto" && (decided.isEmpty() || decided == "new"));
      }
  m_newBody->setVisible(shown);
  const bool open = m_newBodyToggle->isChecked();
  m_newBodyRows->setVisible(open);
  // Closed, the line says where the bodies go when that is not the obvious place.
  const QString parent = m_bodyParent->currentData().toString();
  m_newBodyToggle->setText(!open && !parent.isEmpty() && parent != "*" ? tr("New body, in %1").arg(m_bodyParent->currentText()) : tr("New body"));
  const QString placeholder = makesCopies() ? tr("Named after the picked bodies") : m_name->text().trimmed();
  m_bodyName->setPlaceholderText(placeholder);
  if (m_colour.isValid()) {
    QPixmap swatch(14, 14);
    swatch.fill(m_colour);
    m_bodyColour->setIcon(QIcon(swatch));
    m_bodyColour->setText(m_colour.name());
  } else {
    m_bodyColour->setIcon(QIcon());
    m_bodyColour->setText(makesCopies() ? tr("As the picked bodies") : tr("Automatic"));
  }
  m_bodyColourReset->setVisible(m_colour.isValid());
  emit contentResized();  // after every change of which rows show (refreshVisibility ends here too)
}

opad::json FeaturePanel::bodyStyle() const {
  opad::json style = opad::json::object();
  if (!m_newBody->isVisibleTo(this)) return style;
  const QString parent = m_bodyParent->currentData().toString();
  // The component applies open or closed (it may be the browser's default); new bodies land at the root anyway.
  if (parent != "*" && !(parent.isEmpty() && !makesCopies())) style["parent"] = parent.isEmpty() ? opad::json(nullptr) : opad::json(parent.toStdString());
  if (!m_newBodyToggle->isChecked()) return style;
  if (!m_bodyName->text().trimmed().isEmpty()) style["body_name"] = m_bodyName->text().trimmed().toStdString();
  if (m_colour.isValid()) style["color"] = {m_colour.redF(), m_colour.greenF(), m_colour.blueF()};
  return style;
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

void FeaturePanel::setValues(const std::vector<std::pair<QString, opad::json>>& values) {
  bool any = false, shown = false;  // shown: a choice or a flag changed, which can show or hide rows
  for (const auto& [name, value] : values) {
    auto it = m_widgets.find(name);
    if (it == m_widgets.end()) continue;
    const QSignalBlocker quiet(this);  // the one inputsChanged below
    if (it->second.expr && value.is_string()) {
      if (it->second.expr->lineEdit()->text() == QString::fromStdString(value.get<std::string>())) continue;
      it->second.expr->setText(QString::fromStdString(value.get<std::string>()));
    } else if (it->second.combo && value.is_string()) {
      it->second.combo->setCurrentIndex(std::max(0, it->second.combo->findData(QString::fromStdString(value.get<std::string>()))));
      shown = true;
    } else if (it->second.check && value.is_boolean()) {
      it->second.check->setChecked(value.get<bool>());
      shown = true;
    }
    any = true;
  }
  if (!any) return;
  if (shown) refreshVisibility();  // values alone (a primitive sized by the pointer, every move) change no row
  emit inputsChanged();
}

QString FeaturePanel::pickText(const QString& input) const {
  const auto it = m_widgets.find(input);
  return it != m_widgets.end() && it->second.pick ? it->second.pick->what() : QString();
}

QString FeaturePanel::linkedMoveText(const opad::json& picks, const QString& plain) const {
  if (!picks.is_array()) return plain;
  QStringList files;
  std::set<std::string> imports;
  int others = 0;
  for (const auto& pick : picks) {
    const opad::Node* n = pick.is_object() ? m_doc->scene.node(pick.value("body", "")) : nullptr;
    if (!n || !n->linked) {
      ++others;
      continue;
    }
    if (!imports.insert(n->source_op).second) continue;
    const opad::Op* op = m_doc->doc.find_op(n->source_op);
    const std::string source = op ? op->data.value("source", "") : std::string();
    files << (source.empty() ? QString::fromStdString(n->name) : QFileInfo(QString::fromStdString(source)).fileName());
  }
  if (files.isEmpty()) return plain;
  const QString moving = files.size() == 1 ? tr("%1 moves as one").arg(files.front()) : tr("%1 linked files, each as one").arg(files.size());
  return others > 0 ? tr("%1 selected · %2").arg(others).arg(moving) : moving;
}

void FeaturePanel::setPickNote(const QString& input, const QString& note) {
  if (note.isEmpty()) m_notes.erase(input);
  else m_notes[input] = note;
  refreshVisibility();
}

void FeaturePanel::setGuideStep(int step, int count) {
  m_guideStep = step;
  m_guideCount = count;
  refreshVisibility();
}

QStringList FeaturePanel::valueInputs() const {
  QStringList out;
  if (!m_spec) return out;
  for (const auto& in : m_spec->inputs) {
    const auto it = m_widgets.find(QString::fromStdString(in.name));
    if (it != m_widgets.end() && it->second.expr && !it->second.row->isHidden()) out << it->first;
  }
  return out;
}

QString FeaturePanel::valueText(const QString& input) const {
  const auto it = m_widgets.find(input);
  return it != m_widgets.end() && it->second.expr ? it->second.expr->lineEdit()->text() : QString();
}

QString FeaturePanel::problem(const QString& input) const {
  const auto it = m_widgets.find(input);
  return it != m_widgets.end() && it->second.expr ? it->second.expr->problem() : QString();
}

QString FeaturePanel::statusText() const { return m_status->text(); }

void FeaturePanel::setStatus(const QString& text, bool error) {
  const Tokens& t = theme::current();
  m_status->setText(text);
  m_status->setStyleSheet(QString("color: %1;").arg(theme::css(error ? t.error : t.fg3)));
}

void FeaturePanel::activate(const QString& name) {
  if (m_active == name) return;
  m_active = name;
  refreshVisibility();
  emit activeInputChanged(m_active);
}

std::set<QString> FeaturePanel::shownPicks() const {
  std::set<QString> out;
  for (const auto& [name, w] : m_widgets)
    if (w.pick && !w.row->isHidden()) out.insert(name);
  return out;
}

void FeaturePanel::focusRevealed(const std::set<QString>& before, const QString& wasActive) {
  if (!m_spec || signalsBlocked()) return;  // several values set at once (setValues): no input changes hands unannounced
  auto needs = [this](const opad::design::InputSpec& in) {
    const opad::json p = picks(QString::fromStdString(in.name));
    const int n = p.is_array() ? static_cast<int>(p.size()) : p.is_null() ? 0 : 1;
    return !in.optional && n < std::max(1, in.min_count);
  };
  const opad::design::InputSpec* active = input(m_active);
  if (!active || !needs(*active))
    for (const auto& in : m_spec->inputs) {
      const QString key = QString::fromStdString(in.name);
      const auto it = m_widgets.find(key);
      if (it == m_widgets.end() || !it->second.pick || it->second.row->isHidden() || before.count(key) || !needs(in)) continue;
      return activate(key);
    }
  if (m_active.isEmpty() && !wasActive.isEmpty()) activateNextPick();
}

void FeaturePanel::setSuggestion(const QString& name, const QString& value) {
  if (suggestion(name) == value) return;
  if (value.isEmpty()) m_suggested.erase(name);
  else m_suggested[name] = value;
  const auto it = m_widgets.find(name);
  if (it != m_widgets.end() && it->second.combo) {
    const int at = it->second.combo->findData(QString("auto"));
    if (at >= 0) it->second.combo->setItemText(at, value.isEmpty() ? tr("Automatic") : tr("Automatic: %1").arg(choiceLabel(value.toStdString())));
  }
  refreshNewBody();
}

QString FeaturePanel::suggestion(const QString& name) const {
  const auto it = m_suggested.find(name);
  return it == m_suggested.end() ? QString() : it->second;
}

QString FeaturePanel::choiceText(const QString& name) const {
  const auto it = m_widgets.find(name);
  return it != m_widgets.end() && it->second.combo ? it->second.combo->currentText() : QString();
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
  // Nothing is missing: stay on (or go to) the first pick input so clicks still mean something. Not a plane that has
  // one already: that opens the plane picker, which a box on its default XY plane does not need at the start.
  if (!m_active.isEmpty()) return;
  for (const auto& in : m_spec->inputs) {
    auto it = m_widgets.find(QString::fromStdString(in.name));
    if (it != m_widgets.end() && it->second.pick && !it->second.row->isHidden() && in.type != "plane") return activate(it->first);
  }
}

QSize FeaturePanel::preferredSize(int width) const {
  const int w = width > 0 ? width : 372;
  QLayout* form = m_form->layout();
  form->activate();
  const int height = std::max(form->totalMinimumSize().height(), form->hasHeightForWidth() ? form->totalHeightForWidth(w) : form->totalSizeHint().height());
  return QSize(372, height + m_footer->sizeHint().height());
}

void FeaturePanel::keyPressEvent(QKeyEvent* e) {
  if (e->key() == Qt::Key_Escape) return emit escapePressed();
  if (e->key() == Qt::Key_Return || e->key() == Qt::Key_Enter) return emit accepted();
  QWidget::keyPressEvent(e);
}

// ---------------------------------------------------------------- ParametersDialog
ParametersDialog::ParametersDialog(AppDocument* doc, std::function<void(std::vector<opad::json>, QString)> apply, QWidget* parent)
    : QWidget(parent), m_doc(doc), m_apply(std::move(apply)) {
  setWindowTitle(tr("Parameters"));
  resize(720, 420);
  auto* v = new QVBoxLayout(this);
  v->setContentsMargins(16, 16, 16, 12);
  v->setSpacing(8);
  auto* intro = new QLabel(tr("User parameters can be used in every dimension and feature input: width / 2, 2 * PI * r, 30 deg. Double-click a cell to change it."), this);
  intro->setObjectName("secondary");
  intro->setWordWrap(true);
  v->addWidget(intro);
  auto* units=new QComboBox(this);units->addItems({"mm","cm","m","um","in","ft"});units->setCurrentText(QString::fromStdString(doc->scene.units));
  v->addWidget(new QLabel(tr("Document length unit"),this));v->addWidget(units);
  connect(units,&QComboBox::textActivated,this,[this](const QString& unit){m_apply({opad::json{{"op","units"},{"length",unit.toStdString()}}},tr("Change document units"));});
  connect(doc,&AppDocument::changed,this,[this,units]{units->setCurrentText(QString::fromStdString(m_doc->scene.units));});
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
  m_table->viewport()->installEventFilter(this);
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
  connect(close, &QPushButton::clicked, this, &ParametersDialog::closeRequested);
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
    QString tip = QString::fromStdString(p.comment);
    if (!users.isEmpty()) tip += (tip.isEmpty() ? QString() : QString("\n")) + tr("Used by") + ": " + users.join(", ");
    for (int c = 0; c < 3; ++c) it->setToolTip(c, tip);
    if (it->data(0, Qt::UserRole).toString() == current) m_table->setCurrentItem(it);
  }
  m_filling = false;
}

bool ParametersDialog::eventFilter(QObject* watched, QEvent* event) {
  if (watched == m_table->viewport() && event->type() == QEvent::Resize) {
    const int w = static_cast<QResizeEvent*>(event)->size().width();
    if (w > 0 && w < 670) {  // else the dialog's widths fit
      const int name = w * 34 / 100, expr = w * 36 / 100;
      m_table->setColumnWidth(0, name);
      m_table->setColumnWidth(1, expr);
      m_table->setColumnWidth(2, w - name - expr);
    }
  }
  return QWidget::eventFilter(watched, event);
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
    name = QString("parameter%1").arg(i);
    if (!m_doc->scene.param(name.toStdString())) break;
  }
  m_apply({opad::design::make_param_op(name.toStdString(), "10 "+m_doc->scene.units)}, tr("new parameter"));
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
