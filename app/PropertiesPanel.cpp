#include "PropertiesPanel.hpp"

#include <QEvent>
#include <QFontMetrics>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QTimer>
#include <QVBoxLayout>

#include <set>
#include <string>

#include "I18n.hpp"
#include "Theme.hpp"

namespace {
constexpr int kPropKeyRole = Qt::UserRole + 3;  // properties table: the untranslated property name
constexpr int kActionRole = Qt::UserRole + 4;   // a provided section's link: index into m_actions

QString fmtNum(double v) { return QString::number(v, 'g', 7); }

QString fmtValue(const opad::json& v) {
  if (v.is_number_float()) return fmtNum(v.get<double>());
  if (v.is_array()) {
    QStringList parts;
    for (const auto& e : v) parts << fmtValue(e);
    return "[" + parts.join(", ") + "]";
  }
  if (v.is_string()) return QString::fromStdString(v.get<std::string>());
  if (v.is_boolean()) return i18n::t(v.get<bool>() ? "yes" : "no");
  return QString::fromStdString(v.dump());
}

// A point or direction on one line, as in the design: "(0.707, 0.707, 0)". Three decimals, trailing zeros dropped.
QString fmtComponent(double v) {
  QString s = QString::number(v, 'f', 3);
  while (s.endsWith('0')) s.chop(1);
  if (s.endsWith('.')) s.chop(1);
  return s == "-0" ? QString("0") : s;
}

bool isVector(const opad::json& v) {
  if (!v.is_array() || v.size() < 2 || v.size() > 4) return false;
  for (const auto& e : v) if (!e.is_number()) return false;
  return true;
}
}  // namespace

// ---------------------------------------------------------------- PropertiesPanel
PropertiesPanel::PropertiesPanel(QWidget* parent) : QWidget(parent) {
  auto* layout = new QVBoxLayout(this);
  layout->setContentsMargins(12, 12, 12, 0);
  layout->setSpacing(4);
  auto* head = new QHBoxLayout();
  m_title = new QLabel(tr("Nothing selected"), this);
  m_title->setObjectName("panelTitle");
  head->addWidget(m_title, 1);
  m_id = new QLabel(this);
  m_id->setObjectName("tertiary");
  m_id->setFont(theme::mono(11));
  head->addWidget(m_id);
  layout->addLayout(head);
  m_subtitle = new QLabel(this);
  m_subtitle->setObjectName("secondary");
  m_subtitle->setWordWrap(true);
  layout->addWidget(m_subtitle);
  m_table = new QTreeWidget(this);
  m_table->setColumnCount(2);
  m_table->setHeaderHidden(true);
  m_table->header()->setSectionResizeMode(0, QHeaderView::Fixed);
  m_table->header()->setSectionResizeMode(1, QHeaderView::Stretch);
  m_table->setColumnWidth(0, 128);
  m_table->setIndentation(0);
  m_table->setRootIsDecorated(false);
  m_table->setSelectionMode(QAbstractItemView::NoSelection);
  m_table->setStyleSheet(QString("QTreeWidget::item { border-bottom: 1px solid %1; }").arg(theme::css(theme::current().line)));
  layout->addWidget(m_table, 1);
  connect(theme::notifier(), &theme::Notifier::changed, this, [this] {
    const Tokens& t = theme::current();
    m_table->setStyleSheet(QString("QTreeWidget::item { border-bottom: 1px solid %1; }").arg(theme::css(t.line)));
    for (int i = 0; i < m_table->topLevelItemCount(); ++i) {
      QTreeWidgetItem* row = m_table->topLevelItem(i);
      if (row->data(0, kActionRole).isValid()) {  // a provided section's link
        row->setForeground(1, t.sel);
        continue;
      }
      row->setForeground(0, t.fg2);
      const QString key = row->data(0, kPropKeyRole).toString();
      row->setForeground(1, key == "key" || key == "source_op" ? t.fg3 : t.fg);
    }
  });
  m_table->viewport()->installEventFilter(this);
  connect(m_table, &QTreeWidget::itemClicked, this, [this](QTreeWidgetItem* it, int) {
    if (it->data(0, Qt::UserRole).isValid()) emit faceChosen(it->data(0, Qt::UserRole).toInt());
    else if (const int i = it->data(0, kActionRole).toInt() - 1; i >= 0 && i < static_cast<int>(m_actions.size())) {
      const std::function<void()> run = m_actions[static_cast<size_t>(i)];  // it may fill the panel again
      if (run) run();
    }
  });
}

void PropertiesPanel::addRow(const QString& key, const opad::json& v) {
  const Tokens& t = theme::current();
  auto* row = new QTreeWidgetItem(m_table);
  // Property names come from the core as data. Untranslated (English), the key is shown as words: "center_of_mass"
  // and "bbox min" read "Center of mass" and "Box min".
  QString label = i18n::t(key);
  if (label == key) {
    label.replace('_', ' ');
    if (label.startsWith("bbox")) label.replace(0, 4, "box");
    if (!label.isEmpty()) label[0] = label[0].toUpper();
  }
  row->setText(0, label);
  row->setData(0, kPropKeyRole, key);
  row->setForeground(0, t.fg2);
  row->setToolTip(1, fmtValue(v));
  if (isVector(v)) {
    // Never elide a coordinate: a vector that does not fit the value column gets one row per component.
    QStringList parts;
    for (const auto& e : v) parts << fmtComponent(e.get<double>());
    const QString line = "(" + parts.join(", ") + ")";
    if (QFontMetrics(theme::mono(12)).horizontalAdvance(line) + 16 > m_filledWidth) {
      m_splitVectors = true;
      static const char* axes[] = {"X", "Y", "Z", "W"};
      for (int i = 0; i < parts.size(); ++i) {
        auto* c = new QTreeWidgetItem(m_table);
        c->setText(0, QString("    ") + axes[i]);
        c->setForeground(0, t.fg3);
        c->setText(1, QChar(0x202A) + parts[i] + QChar(0x202C));
        c->setFont(1, theme::mono(12));
        c->setToolTip(1, fmtNum(v[i].get<double>()));
      }
      return;
    }
    row->setText(1, QChar(0x202A) + line + QChar(0x202C));
    row->setFont(1, theme::mono(12));
    return;
  }
  // Measures say what they measure in (the geometry is stored in mm): "9593.088" alone read as a bare count.
  QString unit;
  if (v.is_number()) {
    if (key == "area") unit = QString::fromUtf8(" mm²");
    else if (key == "volume") unit = QString::fromUtf8(" mm³");
    else if (key == "length" || key == "radius" || key == "diameter" || key == "distance" || key == "thickness") unit = " mm";
  }
  row->setText(1, QChar(0x202A) + fmtValue(v) + unit + QChar(0x202C));  // LRE..PDF: numbers and vectors keep their order in a right-to-left UI
  if (v.is_number() || v.is_array() || (v.is_string() && key == "key")) row->setFont(1, theme::mono(12));
  if (key == "key" || key == "source_op") row->setForeground(1, t.fg3);
}

void PropertiesPanel::showEntity(const QString& title, const QString& subtitle, const QString& id, const opad::json& props) {
  m_title->setText(title);
  m_subtitle->setText(subtitle);
  m_id->setText(id);
  m_props = props;
  fill();
}

// Vectors are laid out for the value column's width (one line, or a row per component): redo them when it changes.
// The width is the table viewport's, which settles after the panel's own resize (and moves with the scroll bar).
bool PropertiesPanel::eventFilter(QObject* o, QEvent* e) {
  if (o == m_table->viewport() && e->type() == QEvent::Resize)
    QTimer::singleShot(0, this, [this] {
      const int w = m_table->viewport()->width() - m_table->columnWidth(0);
      if (w != m_filledWidth && !m_props.is_null() && (m_splitVectors || w < m_filledWidth)) fill();
    });
  return QWidget::eventFilter(o, e);
}

void PropertiesPanel::fill() {
  const Tokens& t = theme::current();
  const opad::json& props = m_props;
  m_filledWidth = m_table->viewport()->width() - m_table->columnWidth(0);
  m_splitVectors = false;
  m_table->clear();
  static const char* order[] = {"surface", "curve", "area", "length", "volume", "radius", "diameter", "normal", "axis", "center", "center_of_mass",
                                "start", "end", "origin", "bbox", "faces", "edges", "vertices", "solid", "representation", "material", "instances",
                                "opacity", "visible", "locked", "transform", "world", "component", "source", "key", "source_op"};
  std::set<std::string> done;
  auto addKey = [&](const std::string& k) {
    if (!props.contains(k) || done.count(k)) return;
    done.insert(k);
    const opad::json& v = props[k];
    if (k == "bbox" && v.is_object()) {
      addRow("bbox min", v.value("min", opad::json::array()));
      addRow("bbox max", v.value("max", opad::json::array()));
      addRow("bbox size", v.value("size", opad::json::array()));
      return;
    }
    if (k == "edges" && v.is_array()) return;  // listed as a section below
    addRow(QString::fromStdString(k), v);
  };
  for (const char* k : order) addKey(k);
  for (auto it = props.begin(); it != props.end(); ++it) {
    const std::string& k = it.key();
    if (done.count(k) || k == "id" || k == "ref" || k == "type" || k == "name" || k == "path" || k == "adjacent_faces" || k == "edges" || k == "modified_by" || k == "body" || k == "body_name" || k == "index" || k == "parent" || k == "effectively_visible" || k == "missing")
      continue;
    addKey(k);
  }
  auto section = [&](const QString& title, const opad::json& list, const QString& prefix) {
    if (!list.is_array() || list.empty()) return;
    auto* h = new QTreeWidgetItem(m_table);
    h->setText(0, title);
    h->setFont(0, theme::ui(11, QFont::Medium));
    h->setForeground(0, t.fg3);
    h->setFlags(Qt::ItemIsEnabled);
    for (const auto& e : list) {
      auto* r = new QTreeWidgetItem(m_table);
      r->setText(0, QString("%1 %2").arg(i18n::t(prefix)).arg(e.get<int>()));
      r->setFont(0, theme::mono(12));
      r->setData(0, Qt::UserRole, e.get<int>());
      r->setToolTip(0, tr("Click to inspect"));
    }
  };
  section(tr("ADJACENT FACES"), props.value("adjacent_faces", opad::json()), "face");
  section(tr("BOUNDING EDGES"), props.value("edges", opad::json()), "edge");
  m_actions.clear();
  QList<PropertySection> sections;
  for (const PropertySectionProvider& provide : m_providers) provide(m_subject, props, sections);
  for (const PropertySection& s : sections) {
    auto* h = new QTreeWidgetItem(m_table);
    h->setText(0, s.title.toUpper());
    h->setFont(0, theme::ui(11, QFont::Medium));
    h->setForeground(0, t.fg3);
    h->setFlags(Qt::ItemIsEnabled);
    h->setFirstColumnSpanned(true);
    for (const auto& [label, value] : s.rows) {
      auto* r = new QTreeWidgetItem(m_table);
      r->setText(0, label);
      r->setForeground(0, t.fg2);
      r->setText(1, QChar(0x202A) + value + QChar(0x202C));  // like the built-in rows: paths and numbers keep their order
      r->setToolTip(1, value);
    }
    for (const auto& [label, run] : s.actions) {
      auto* r = new QTreeWidgetItem(m_table);
      r->setText(1, label);
      r->setForeground(1, t.sel);
      r->setData(0, kActionRole, static_cast<int>(m_actions.size()) + 1);
      r->setToolTip(1, label);
      m_actions.push_back(run);
    }
  }
}

void PropertiesPanel::setSubject(PropertySubject subject) { m_subject = std::move(subject); }

void PropertiesPanel::addSectionProvider(PropertySectionProvider provider) {
  m_providers.push_back(std::move(provider));
  refresh();
}

void PropertiesPanel::refresh() {
  if (!m_props.is_null()) fill();
}

void PropertiesPanel::clear() {
  m_title->setText(tr("Nothing selected"));
  m_subtitle->setText(tr("Click a body, or pick faces, edges and vertices with the Select filter (1–4)."));
  m_id->clear();
  m_props = opad::json();
  m_subject = PropertySubject();
  m_actions.clear();
  m_table->clear();
}
