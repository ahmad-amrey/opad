#include "Panels.hpp"

#include <QAction>
#include <QColorDialog>
#include <QDropEvent>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QKeySequenceEdit>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QSettings>
#include <QToolTip>
#include <QVBoxLayout>

#include <functional>

namespace {
constexpr int kIdRole = Qt::UserRole + 1;
constexpr int kNameRole = Qt::UserRole + 2;
}  // namespace

// ---------------------------------------------------------------- BrowserTree
void BrowserTree::dropEvent(QDropEvent* e) {
  QTreeWidgetItem* target = itemAt(e->position().toPoint());
  DropIndicatorPosition pos = dropIndicatorPosition();
  std::string parent;
  int index = -1;
  if (target) {
    bool onto = pos == QAbstractItemView::OnItem;
    bool target_is_component = target->data(0, Qt::UserRole).toString() == "component";
    if (onto && target_is_component) {
      parent = target->data(0, kIdRole).toString().toStdString();
    } else {
      QTreeWidgetItem* p = target->parent();
      parent = p ? p->data(0, kIdRole).toString().toStdString() : std::string();
      index = p ? p->indexOfChild(target) : indexOfTopLevelItem(target);
      if (pos == QAbstractItemView::BelowItem) ++index;
      if (onto) index = -1;
    }
  }
  std::vector<std::string> ids;
  for (QTreeWidgetItem* it : selectedItems()) ids.push_back(it->data(0, kIdRole).toString().toStdString());
  e->ignore();  // the model rebuild after the reparent op moves the items
  if (!ids.empty()) emit reparentRequested(ids, parent, index);
}

// ---------------------------------------------------------------- BrowserPanel
BrowserPanel::BrowserPanel(AppDocument* doc, QWidget* parent) : QWidget(parent), m_doc(doc) {
  auto* layout = new QVBoxLayout(this);
  layout->setContentsMargins(4, 4, 4, 4);
  m_filter = new QLineEdit(this);
  m_filter->setPlaceholderText(tr("Filter objects (type to search)"));
  m_filter->setClearButtonEnabled(true);
  layout->addWidget(m_filter);
  m_breadcrumb = new QLabel(this);
  m_breadcrumb->setTextFormat(Qt::PlainText);
  m_breadcrumb->setStyleSheet("color: palette(mid); padding: 2px;");
  layout->addWidget(m_breadcrumb);
  m_tree = new BrowserTree(this);
  m_tree->setColumnCount(3);
  m_tree->setHeaderLabels({tr("Object"), "", ""});
  m_tree->header()->setStretchLastSection(false);
  m_tree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
  m_tree->header()->setSectionResizeMode(1, QHeaderView::Fixed);
  m_tree->header()->setSectionResizeMode(2, QHeaderView::Fixed);
  m_tree->setColumnWidth(1, 24);
  m_tree->setColumnWidth(2, 24);
  m_tree->setSelectionMode(QAbstractItemView::ExtendedSelection);
  m_tree->setDragDropMode(QAbstractItemView::InternalMove);
  m_tree->setDefaultDropAction(Qt::MoveAction);
  m_tree->setContextMenuPolicy(Qt::CustomContextMenu);
  m_tree->setEditTriggers(QAbstractItemView::EditKeyPressed | QAbstractItemView::SelectedClicked);
  layout->addWidget(m_tree, 1);

  connect(m_filter, &QLineEdit::textChanged, this, [this] { applyFilter(); });
  connect(m_tree, &QTreeWidget::itemSelectionChanged, this, [this] {
    if (m_updating) return;
    updateBreadcrumb();
    emit selectionChanged(selectedIds());
  });
  connect(m_tree, &QTreeWidget::customContextMenuRequested, this, [this](const QPoint& p) {
    emit contextMenuRequested(m_tree->viewport()->mapToGlobal(p), selectedIds());
  });
  connect(m_tree, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem* it, int col) {
    std::string id = it->data(0, kIdRole).toString().toStdString();
    if (col == 2) {
      const opad::Node* n = m_doc->node(id);
      QColor start = n && n->has_color ? QColor::fromRgbF(n->color[0], n->color[1], n->color[2]) : QColor(190, 190, 195);
      QColor c = QColorDialog::getColor(start, this, tr("Colour of %1").arg(QString::fromStdString(n ? n->name : id)));
      if (c.isValid()) m_doc->run("appearance", opad::json{{"target", id}, {"color", {c.redF(), c.greenF(), c.blueF()}}});
    } else if (col == 0) {
      emit fitRequested({id});
    }
  });
  connect(m_tree, &QTreeWidget::itemClicked, this, [this](QTreeWidgetItem* it, int col) {
    if (col != 1) return;
    std::string id = it->data(0, kIdRole).toString().toStdString();
    const opad::Node* n = m_doc->node(id);
    if (!n) return;
    m_doc->run("appearance", opad::json{{"target", id}, {"visible", !n->visible}});
  });
  connect(m_tree, &QTreeWidget::itemChanged, this, [this](QTreeWidgetItem* it, int col) {
    if (m_updating || col != 0) return;
    std::string id = it->data(0, kIdRole).toString().toStdString();
    QString newName = it->text(0).trimmed();
    QString oldName = it->data(0, kNameRole).toString();
    if (newName.isEmpty() || newName == oldName) {
      rebuild();
      return;
    }
    m_doc->run("rename", opad::json{{"target", id}, {"name", newName.toStdString()}});
  });
  connect(m_tree, &BrowserTree::reparentRequested, this, [this](const std::vector<std::string>& ids, const std::string& parent, int index) {
    for (const auto& id : ids) {
      opad::json op{{"target", id}};
      op["parent"] = parent.empty() ? opad::json(nullptr) : opad::json(parent);
      if (index >= 0) op["index"] = index;
      try {
        m_doc->run("reparent", op);
      } catch (const std::exception& e) {
        emit m_doc->message(QString::fromUtf8(e.what()));
      }
    }
  });
  connect(doc, &AppDocument::changed, this, &BrowserPanel::rebuild);
  rebuild();
}

QTreeWidgetItem* BrowserPanel::build(const std::string& id, QTreeWidgetItem* parent, std::set<std::string>& expanded) {
  const opad::Node* n = m_doc->node(id);
  if (!n) return nullptr;
  auto* item = parent ? new QTreeWidgetItem(parent) : new QTreeWidgetItem(m_tree);
  QString name = QString::fromStdString(n->name);
  QString label = name;
  if (n->kind == opad::Node::Kind::Body) {
    auto it = m_doc->scene.instance_count.find(n->body_key);
    if (it != m_doc->scene.instance_count.end() && it->second > 1) label += QString::fromUtf8("  ×%1").arg(it->second);
    if (n->body_missing) label += tr("  [missing body]");
  }
  if (n->locked) label += QString::fromUtf8("  \U0001F512");
  item->setText(0, label);
  item->setData(0, kIdRole, QString::fromStdString(id));
  item->setData(0, kNameRole, name);
  item->setData(0, Qt::UserRole, n->kind == opad::Node::Kind::Body ? "body" : "component");
  item->setFlags(item->flags() | Qt::ItemIsEditable | Qt::ItemIsDragEnabled | (n->kind == opad::Node::Kind::Component ? Qt::ItemIsDropEnabled : Qt::NoItemFlags));
  item->setText(1, n->visible ? QString::fromUtf8("●") : QString::fromUtf8("○"));
  item->setToolTip(1, n->visible ? tr("Visible (click to hide)") : tr("Hidden (click to show)"));
  item->setTextAlignment(1, Qt::AlignCenter);
  if (n->has_color) {
    item->setBackground(2, QColor::fromRgbF(n->color[0], n->color[1], n->color[2]));
    item->setToolTip(2, tr("Colour (double-click to change)"));
  }
  if (n->body_missing) item->setForeground(0, QBrush(QColor(200, 60, 40)));
  else if (!n->visible) item->setForeground(0, QBrush(QColor(140, 140, 140)));
  item->setToolTip(0, QString::fromStdString(id));
  for (const auto& c : n->children) build(c, item, expanded);
  item->setExpanded(expanded.empty() ? true : expanded.count(id) > 0);
  return item;
}

void BrowserPanel::rebuild() {
  m_updating = true;
  std::set<std::string> expanded;
  std::vector<std::string> selected = selectedIds();
  std::function<void(QTreeWidgetItem*)> collect = [&](QTreeWidgetItem* it) {
    if (it->isExpanded()) expanded.insert(it->data(0, kIdRole).toString().toStdString());
    for (int i = 0; i < it->childCount(); ++i) collect(it->child(i));
  };
  for (int i = 0; i < m_tree->topLevelItemCount(); ++i) collect(m_tree->topLevelItem(i));
  m_tree->clear();
  for (const auto& r : m_doc->scene.roots) build(r, nullptr, expanded);
  applyFilter();
  setSelectedIds(selected);
  m_updating = false;
  updateBreadcrumb();
}

void BrowserPanel::applyFilter() {
  QString f = m_filter->text().trimmed();
  std::function<bool(QTreeWidgetItem*)> visit = [&](QTreeWidgetItem* it) {
    bool child_match = false;
    for (int i = 0; i < it->childCount(); ++i) child_match = visit(it->child(i)) || child_match;
    bool self = f.isEmpty() || it->text(0).contains(f, Qt::CaseInsensitive);
    it->setHidden(!(self || child_match));
    if (!f.isEmpty() && child_match) it->setExpanded(true);
    return self || child_match;
  };
  for (int i = 0; i < m_tree->topLevelItemCount(); ++i) visit(m_tree->topLevelItem(i));
}

QTreeWidgetItem* BrowserPanel::itemFor(const std::string& id) const {
  QString q = QString::fromStdString(id);
  std::function<QTreeWidgetItem*(QTreeWidgetItem*)> find = [&](QTreeWidgetItem* it) -> QTreeWidgetItem* {
    if (it->data(0, kIdRole).toString() == q) return it;
    for (int i = 0; i < it->childCount(); ++i)
      if (auto* r = find(it->child(i))) return r;
    return nullptr;
  };
  for (int i = 0; i < m_tree->topLevelItemCount(); ++i)
    if (auto* r = find(m_tree->topLevelItem(i))) return r;
  return nullptr;
}

std::vector<std::string> BrowserPanel::selectedIds() const {
  std::vector<std::string> ids;
  for (QTreeWidgetItem* it : m_tree->selectedItems()) ids.push_back(it->data(0, kIdRole).toString().toStdString());
  return ids;
}

void BrowserPanel::setSelectedIds(const std::vector<std::string>& ids) {
  bool was = m_updating;
  m_updating = true;
  m_tree->clearSelection();
  QTreeWidgetItem* first = nullptr;
  for (const auto& id : ids)
    if (auto* it = itemFor(id)) {
      it->setSelected(true);
      for (QTreeWidgetItem* p = it->parent(); p; p = p->parent()) p->setExpanded(true);
      if (!first) first = it;
    }
  if (first) m_tree->scrollToItem(first);
  m_updating = was;
  updateBreadcrumb();
}

void BrowserPanel::startRename(const std::string& id) {
  if (auto* it = itemFor(id)) m_tree->editItem(it, 0);
}

void BrowserPanel::updateBreadcrumb() {
  auto ids = selectedIds();
  if (ids.empty()) {
    m_breadcrumb->setText(m_doc->hasDocument ? tr("Document root") : QString());
    return;
  }
  QStringList parts;
  for (const auto& p : m_doc->scene.path_to(ids.front())) parts << m_doc->nodeName(p);
  m_breadcrumb->setText(parts.join(QString::fromUtf8("  ›  ")));
}

// ---------------------------------------------------------------- PropertiesPanel
PropertiesPanel::PropertiesPanel(QWidget* parent) : QWidget(parent) {
  auto* layout = new QVBoxLayout(this);
  layout->setContentsMargins(4, 4, 4, 4);
  m_title = new QLabel(tr("Nothing selected"), this);
  m_title->setWordWrap(true);
  m_title->setStyleSheet("font-weight: bold; padding: 2px;");
  layout->addWidget(m_title);
  m_tree = new QTreeWidget(this);
  m_tree->setColumnCount(2);
  m_tree->setHeaderLabels({tr("Property"), tr("Value")});
  m_tree->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
  m_tree->setRootIsDecorated(true);
  layout->addWidget(m_tree, 1);
}

static QString fmt(const opad::json& v) {
  if (v.is_number_float()) return QString::number(v.get<double>(), 'g', 7);
  if (v.is_array()) {
    QStringList parts;
    for (const auto& e : v) parts << fmt(e);
    return "[" + parts.join(", ") + "]";
  }
  if (v.is_string()) return QString::fromStdString(v.get<std::string>());
  return QString::fromStdString(v.dump());
}

void PropertiesPanel::add(QTreeWidgetItem* parent, const QString& key, const opad::json& v) {
  auto* item = parent ? new QTreeWidgetItem(parent) : new QTreeWidgetItem(m_tree);
  item->setText(0, key);
  if (v.is_object()) {
    for (auto it = v.begin(); it != v.end(); ++it) add(item, QString::fromStdString(it.key()), it.value());
    item->setExpanded(true);
  } else if (v.is_array() && !v.empty() && (v[0].is_object() || v[0].is_array()) && !(v.size() <= 4 && v[0].is_number())) {
    int i = 0;
    for (const auto& e : v) add(item, QString::number(i++), e);
  } else {
    item->setText(1, fmt(v));
    item->setToolTip(1, fmt(v));
  }
}

void PropertiesPanel::showJson(const QString& title, const opad::json& j) {
  m_title->setText(title);
  m_tree->clear();
  if (j.is_object())
    for (auto it = j.begin(); it != j.end(); ++it) add(nullptr, QString::fromStdString(it.key()), it.value());
  else
    add(nullptr, tr("value"), j);
}

void PropertiesPanel::clear() {
  m_title->setText(tr("Nothing selected"));
  m_tree->clear();
}

// ---------------------------------------------------------------- AnnotationsPanel
AnnotationsPanel::AnnotationsPanel(AppDocument* doc, QWidget* parent) : QWidget(parent), m_doc(doc) {
  auto* layout = new QVBoxLayout(this);
  layout->setContentsMargins(4, 4, 4, 4);
  auto* row = new QHBoxLayout();
  m_author = new QComboBox(this);
  m_author->addItem(tr("All authors"));
  row->addWidget(m_author, 1);
  auto* add = new QPushButton(tr("Add note"), this);
  auto* resolve = new QPushButton(tr("Resolve"), this);
  row->addWidget(add);
  row->addWidget(resolve);
  layout->addLayout(row);
  m_list = new QListWidget(this);
  m_list->setWordWrap(true);
  layout->addWidget(m_list, 1);
  connect(add, &QPushButton::clicked, this, &AnnotationsPanel::addRequested);
  connect(resolve, &QPushButton::clicked, this, [this] {
    std::string id = currentOpId();
    if (!id.empty()) emit resolveRequested(id);
  });
  connect(m_author, &QComboBox::currentIndexChanged, this, [this](int) { rebuild(); });
  connect(m_list, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem* it) {
    std::string body = it->data(Qt::UserRole + 1).toString().toStdString();
    if (!body.empty()) emit selectNode(body);
  });
  connect(doc, &AppDocument::changed, this, &AnnotationsPanel::rebuild);
}

std::string AnnotationsPanel::currentOpId() const {
  QListWidgetItem* it = m_list->currentItem();
  return it ? it->data(Qt::UserRole).toString().toStdString() : std::string();
}

void AnnotationsPanel::rebuild() {
  QString current = m_author->currentText();
  std::set<std::string> authors;
  for (const auto& a : m_doc->scene.annotations) authors.insert(a.by);
  m_author->blockSignals(true);
  m_author->clear();
  m_author->addItem(tr("All authors"));
  for (const auto& a : authors) m_author->addItem(QString::fromStdString(a));
  int idx = m_author->findText(current);
  m_author->setCurrentIndex(idx < 0 ? 0 : idx);
  m_author->blockSignals(false);
  std::string filter = m_author->currentIndex() > 0 ? m_author->currentText().toStdString() : std::string();
  m_list->clear();
  for (const auto& a : m_doc->scene.annotations) {
    if (!filter.empty() && a.by != filter) continue;
    QString where = a.anchor.kind == opad::Ref::Kind::Point ? tr("point") : m_doc->nodeName(a.anchor.body);
    if (a.anchor.kind == opad::Ref::Kind::Face || a.anchor.kind == opad::Ref::Kind::Edge || a.anchor.kind == opad::Ref::Kind::Vertex)
      where += QString(" / %1 %2").arg(opad::Ref::kind_name(a.anchor.kind)).arg(a.anchor.index);
    auto* it = new QListWidgetItem(QString("%1\n%2 - %3%4").arg(QString::fromStdString(a.text), QString::fromStdString(a.by), where, a.unresolved ? tr("  [unresolved anchor]") : ""));
    it->setData(Qt::UserRole, QString::fromStdString(a.id));
    it->setData(Qt::UserRole + 1, QString::fromStdString(a.anchor.body));
    it->setToolTip(QString::fromStdString(a.ts));
    if (a.unresolved) it->setForeground(QBrush(QColor(200, 60, 40)));
    m_list->addItem(it);
  }
  for (const auto& m : m_doc->scene.measurements) {
    auto* it = new QListWidgetItem(tr("Measurement (%1): %2 %3").arg(QString::fromStdString(m.kind), fmt(m.result.value("value", opad::json(0.0))), QString::fromStdString(m.result.value("unit", ""))));
    it->setData(Qt::UserRole, QString::fromStdString(m.id));
    if (!m.refs.empty()) it->setData(Qt::UserRole + 1, QString::fromStdString(m.refs.front().body));
    it->setForeground(QBrush(QColor(90, 120, 200)));
    m_list->addItem(it);
  }
}

// ---------------------------------------------------------------- TimelineWidget
TimelineWidget::TimelineWidget(AppDocument* doc, QWidget* parent) : QWidget(parent), m_doc(doc) {
  setMouseTracking(true);
  setMinimumHeight(44);
  connect(doc, &AppDocument::changed, this, &TimelineWidget::rebuild);
}

void TimelineWidget::rebuild() {
  m_deleted = std::set<std::string>(m_doc->scene.deleted_ops.begin(), m_doc->scene.deleted_ops.end());
  update();
}

double TimelineWidget::spacing() const {
  size_t n = m_doc->doc.ops.size();
  if (n == 0) return 24;
  return std::clamp((width() - 40.0) / static_cast<double>(n), 6.0, 26.0);
}

int TimelineWidget::indexAt(const QPoint& p) const {
  double s = spacing();
  int i = static_cast<int>((p.x() - 20 + s / 2) / s);
  if (i < 0 || i >= static_cast<int>(m_doc->doc.ops.size())) return -1;
  return i;
}

static QColor opColor(const std::string& type) {
  if (type == "import") return QColor(70, 130, 220);
  if (type == "annotation") return QColor(230, 160, 40);
  if (type == "measurement") return QColor(90, 180, 120);
  if (type == "delete") return QColor(200, 70, 60);
  if (type == "reparent" || type == "transform") return QColor(160, 100, 200);
  if (type == "section" || type == "view") return QColor(80, 180, 200);
  return QColor(140, 140, 150);
}

void TimelineWidget::paintEvent(QPaintEvent*) {
  QPainter p(this);
  p.setRenderHint(QPainter::Antialiasing);
  p.fillRect(rect(), palette().base());
  const auto& ops = m_doc->doc.ops;
  const double s = spacing();
  const int y = height() / 2;
  p.setPen(QPen(palette().mid().color(), 1));
  p.drawLine(12, y, width() - 12, y);
  for (size_t i = 0; i < ops.size(); ++i) {
    const double x = 20 + i * s;
    QColor c = opColor(ops[i].type);
    bool deleted = m_deleted.count(ops[i].id) > 0;
    QRectF r(x - 5, y - 8, 10, 16);
    if (static_cast<int>(i) == m_hover) r.adjust(-2, -2, 2, 2);
    if (deleted) {
      p.setPen(QPen(c, 1.5));
      p.setBrush(Qt::NoBrush);
    } else {
      p.setPen(QPen(c.darker(130), 1));
      p.setBrush(c);
    }
    if (ops[i].type == "import") p.drawRect(r);
    else if (ops[i].type == "delete") p.drawEllipse(r);
    else p.drawRoundedRect(r, 3, 3);
  }
  if (ops.empty()) {
    p.setPen(palette().mid().color());
    p.drawText(rect(), Qt::AlignCenter, tr("Timeline: one marker per operation"));
  }
}

void TimelineWidget::mouseMoveEvent(QMouseEvent* e) {
  int i = indexAt(e->pos());
  if (i != m_hover) {
    m_hover = i;
    update();
  }
  if (i >= 0) {
    const opad::Op& op = m_doc->doc.ops[static_cast<size_t>(i)];
    QString tip = QString("%1  %2\n%3  %4").arg(QString::fromStdString(op.type), m_deleted.count(op.id) ? tr("(deleted)") : "",
                                                 QString::fromStdString(op.data.value("ts", "")), QString::fromStdString(op.data.value("by", "")));
    for (const char* k : {"name", "text", "kind", "source"})
      if (op.data.contains(k) && op.data[k].is_string()) tip += "\n" + QString(k) + ": " + QString::fromStdString(op.data[k].get<std::string>());
    if (op.data.contains("target")) tip += "\ntarget: " + m_doc->nodeName(op.data["target"].get<std::string>());
    QToolTip::showText(e->globalPosition().toPoint(), tip, this);
  }
}

void TimelineWidget::mousePressEvent(QMouseEvent* e) {
  int i = indexAt(e->pos());
  if (i < 0) return;
  const std::string id = m_doc->doc.ops[static_cast<size_t>(i)].id;
  if (e->button() == Qt::RightButton) emit deleteRequested(id);
  else emit opClicked(id);
}

void TimelineWidget::leaveEvent(QEvent*) {
  m_hover = -1;
  update();
}

// ---------------------------------------------------------------- CommandPalette
CommandPalette::CommandPalette(const QList<QAction*>& actions, QWidget* parent) : QDialog(parent), m_actions(actions) {
  setWindowTitle(tr("Command search"));
  setWindowFlags(windowFlags() | Qt::Popup);
  resize(520, 360);
  auto* layout = new QVBoxLayout(this);
  m_edit = new QLineEdit(this);
  m_edit->setPlaceholderText(tr("Type a command..."));
  m_list = new QListWidget(this);
  layout->addWidget(m_edit);
  layout->addWidget(m_list, 1);
  connect(m_edit, &QLineEdit::textChanged, this, &CommandPalette::refill);
  connect(m_edit, &QLineEdit::returnPressed, this, &CommandPalette::runCurrent);
  connect(m_list, &QListWidget::itemActivated, this, [this](QListWidgetItem*) { runCurrent(); });
  m_edit->installEventFilter(this);
  refill(QString());
  m_edit->setFocus();
}

void CommandPalette::refill(const QString& filter) {
  m_list->clear();
  for (QAction* a : m_actions) {
    if (a->text().isEmpty() || a->isSeparator()) continue;
    QString text = a->text().remove('&');
    if (!filter.isEmpty() && !text.contains(filter, Qt::CaseInsensitive)) continue;
    auto* it = new QListWidgetItem(QString("%1    %2").arg(text, a->shortcut().toString(QKeySequence::NativeText)));
    it->setData(Qt::UserRole, QVariant::fromValue(static_cast<void*>(a)));
    if (!a->isEnabled()) it->setForeground(QBrush(QColor(150, 150, 150)));
    m_list->addItem(it);
  }
  if (m_list->count() > 0) m_list->setCurrentRow(0);
}

void CommandPalette::runCurrent() {
  QListWidgetItem* it = m_list->currentItem();
  if (!it) return;
  auto* a = static_cast<QAction*>(it->data(Qt::UserRole).value<void*>());
  accept();
  if (a && a->isEnabled()) a->trigger();
}

// ---------------------------------------------------------------- ShortcutEditor
ShortcutEditor::ShortcutEditor(const QList<QAction*>& actions, QWidget* parent) : QDialog(parent), m_actions(actions) {
  setWindowTitle(tr("Keyboard shortcuts"));
  resize(520, 480);
  auto* layout = new QVBoxLayout(this);
  m_tree = new QTreeWidget(this);
  m_tree->setColumnCount(2);
  m_tree->setHeaderLabels({tr("Command"), tr("Shortcut")});
  m_tree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
  layout->addWidget(m_tree, 1);
  for (QAction* a : actions) {
    if (a->text().isEmpty() || a->isSeparator()) continue;
    auto* it = new QTreeWidgetItem(m_tree);
    it->setText(0, a->text().remove('&'));
    auto* edit = new QKeySequenceEdit(a->shortcut(), m_tree);
    m_tree->setItemWidget(it, 1, edit);
  }
  auto* row = new QHBoxLayout();
  auto* ok = new QPushButton(tr("Apply"), this);
  auto* cancel = new QPushButton(tr("Cancel"), this);
  row->addStretch();
  row->addWidget(ok);
  row->addWidget(cancel);
  layout->addLayout(row);
  connect(ok, &QPushButton::clicked, this, &ShortcutEditor::accept);
  connect(cancel, &QPushButton::clicked, this, &ShortcutEditor::reject);
}

void ShortcutEditor::accept() {
  QSettings settings;
  int row = 0;
  for (QAction* a : m_actions) {
    if (a->text().isEmpty() || a->isSeparator()) continue;
    auto* edit = qobject_cast<QKeySequenceEdit*>(m_tree->itemWidget(m_tree->topLevelItem(row++), 1));
    if (!edit) continue;
    a->setShortcut(edit->keySequence());
    settings.setValue("shortcuts/" + a->objectName(), edit->keySequence().toString());
  }
  QDialog::accept();
}
