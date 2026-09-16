#include "Panels.hpp"

#include <QAction>
#include <QApplication>
#include <QColorDialog>
#include <QDockWidget>
#include <QDropEvent>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QItemSelection>
#include <QItemSelectionModel>
#include <QKeyEvent>
#include <QKeySequenceEdit>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QProgressBar>
#include <QSizePolicy>
#include <QPushButton>
#include <QSettings>
#include <QToolButton>
#include <QToolTip>
#include <QVBoxLayout>

#include <algorithm>
#include <functional>

#include "Icons.hpp"
#include "Jobs.hpp"
#include "Theme.hpp"
#include "opad/inspect.hpp"

namespace {
constexpr int kIdRole = Qt::UserRole + 1;
constexpr int kNameRole = Qt::UserRole + 2;
constexpr int kEyeX = 2, kSwatchX = 24, kTypeX = 42, kNameX = 66;

QString fmtNum(double v) { return QString::number(v, 'g', 7); }

QString fmtValue(const opad::json& v) {
  if (v.is_number_float()) return fmtNum(v.get<double>());
  if (v.is_array()) {
    QStringList parts;
    for (const auto& e : v) parts << fmtValue(e);
    return "[" + parts.join(", ") + "]";
  }
  if (v.is_string()) return QString::fromStdString(v.get<std::string>());
  if (v.is_boolean()) return v.get<bool>() ? "yes" : "no";
  return QString::fromStdString(v.dump());
}

QColor authorColor(const std::string& by) {
  uint h = qHash(QString::fromStdString(by));
  return QColor::fromHsl(static_cast<int>(h % 360), 140, 130);
}

QString shortId(const std::string& id) { return QString::fromStdString(id.substr(0, 8)); }
}  // namespace

// Ops the timeline draws. Visibility toggles, notes and pinned measurements are view state with their own
// UI (browser eye, Annotations tab, measurement card) and would bury the structural history; their
// tombstones go with them.
bool timelineShows(const opad::Document& doc, const opad::Op& op) {
  if (op.type == "annotation" || op.type == "measurement") return false;
  if (op.type == "appearance" && op.data.contains("visible")) return false;
  if (op.type == "delete") {
    const opad::Op* t = doc.find_op(op.data.value("target", ""));
    return !t || timelineShows(doc, *t);
  }
  return true;
}

QString opTypeIcon(const std::string& type) {
  if (type == "import") return "import";
  if (type == "rename") return "rename";
  if (type == "transform") return "move";
  if (type == "appearance") return "eye";
  if (type == "reparent") return "reparent";
  if (type == "annotation") return "annotate";
  if (type == "measurement") return "distance";
  if (type == "section") return "section";
  if (type == "view") return "home";
  if (type == "delete") return "delete";
  return "dot";
}

QString opGroup(const QAction* a) {
  QString id = a->objectName();
  if (id.startsWith("file.")) return "File";
  if (id.startsWith("view.")) return "View";
  if (id.startsWith("nav.")) return "Navigation";
  if (id.startsWith("select.")) return "Select";
  if (id.startsWith("inspect.")) return "Inspect";
  if (id.startsWith("annotate.") || id.startsWith("edit.")) return "Edit";
  if (id.startsWith("tools.")) return "Tools";
  return "Help";
}

// ---------------------------------------------------------------- DockHeader
DockHeader::DockHeader(const QString& title, QDockWidget* dock) : QWidget(dock) {
  setObjectName("dockHeader");
  setAttribute(Qt::WA_StyledBackground);
  setFixedHeight(28);
  auto* l = new QHBoxLayout(this);
  l->setContentsMargins(8, 0, 6, 0);
  l->setSpacing(8);
  m_title = new QLabel(title, this);
  m_title->setObjectName("dockTitle");
  l->addWidget(m_title, 1);
  const Tokens& t = theme::current();
  auto* flt = new QToolButton(this);
  flt->setObjectName("dockButton");
  flt->setIcon(icons::icon("float", t.fg2));
  flt->setIconSize(QSize(16, 16));
  flt->setFixedSize(20, 20);
  flt->setToolTip(tr("Float / dock"));
  auto* close = new QToolButton(this);
  close->setObjectName("dockButton");
  close->setIcon(icons::icon("close", t.fg2));
  close->setIconSize(QSize(16, 16));
  close->setFixedSize(20, 20);
  close->setToolTip(tr("Close panel"));
  l->addWidget(flt);
  l->addWidget(close);
  connect(flt, &QToolButton::clicked, dock, [dock] { dock->setFloating(!dock->isFloating()); });
  connect(close, &QToolButton::clicked, dock, &QDockWidget::close);
  connect(theme::notifier(), &theme::Notifier::changed, this, [flt, close] {
    flt->setIcon(icons::icon("float", theme::current().fg2));
    close->setIcon(icons::icon("close", theme::current().fg2));
  });
}

void DockHeader::setTitle(const QString& t) { m_title->setText(t); }

// ---------------------------------------------------------------- BrowserTree
BrowserTree::BrowserTree(AppDocument* doc, QWidget* parent) : QTreeWidget(parent), m_doc(doc) {
  setObjectName("browserTree");  // the delegate paints whole rows; the style must not add its own selection (Theme.cpp)
  setIndentation(16);
  setRootIsDecorated(true);
  setHeaderHidden(true);
  setColumnCount(1);
  setMouseTracking(true);
  setUniformRowHeights(true);
  setSelectionMode(QAbstractItemView::ExtendedSelection);
  setDragDropMode(QAbstractItemView::InternalMove);
  setDefaultDropAction(Qt::MoveAction);
  setEditTriggers(QAbstractItemView::EditKeyPressed);
  setAttribute(Qt::WA_Hover);
}

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
  e->ignore();
  if (!ids.empty()) emit reparentRequested(ids, parent, index);
}

void BrowserTree::mousePressEvent(QMouseEvent* e) {
  QModelIndex idx = indexAt(e->pos());
  if (idx.isValid() && e->button() == Qt::LeftButton) {
    QRect r = visualRect(idx);
    int x = e->pos().x() - r.left();
    std::string id = idx.data(kIdRole).toString().toStdString();
    if (x >= kEyeX && x < kEyeX + 18) { emit eyeClicked(id); return; }
    if (x >= kSwatchX - 2 && x < kSwatchX + 14) { emit swatchClicked(id); return; }
  }
  QTreeWidget::mousePressEvent(e);
}

void BrowserTree::drawBranches(QPainter* painter, const QRect& rect, const QModelIndex& index) const {
  if (!model()->hasChildren(index)) return;
  const Tokens& t = theme::current();
  QRect r(rect.right() - 16, rect.top() + 6, 16, 16);
  painter->drawPixmap(r.topLeft(), icons::pixmap(isExpanded(index) ? "chevronDown" : "chevronRight", t.fg3, 16, devicePixelRatioF()));
}

// ---------------------------------------------------------------- BrowserDelegate
void BrowserDelegate::paint(QPainter* p, const QStyleOptionViewItem& opt, const QModelIndex& index) const {
  const Tokens& t = theme::current();
  std::string id = index.data(kIdRole).toString().toStdString();
  const bool isDoc = index.data(Qt::UserRole).toString() == "document";
  const opad::Node* n = isDoc ? nullptr : m_doc->node(id);
  p->save();
  p->setRenderHint(QPainter::Antialiasing);
  QRect r = opt.rect;
  const int fullW = opt.widget ? opt.widget->width() : r.right();
  QRect full(0, r.top(), fullW, r.height());
  if (opt.state & QStyle::State_Selected) {
    p->fillRect(full, t.selbg);  // design: one translucent tint across the whole row, indent included
  } else if (opt.state & QStyle::State_MouseOver) {
    p->fillRect(full, t.bg3);
  }
  if (!n && !isDoc) { p->restore(); return; }
  bool hidden = n && !n->visible;
  QColor text = hidden ? t.fg3 : t.fg;
  QColor iconColor = hidden ? t.fg3 : t.fg2;
  const qreal dpr = p->device()->devicePixelRatioF();
  int y = r.top() + 6;
  p->drawPixmap(r.left() + kEyeX, y, icons::pixmap(hidden ? "hide" : "eye", iconColor, 16, dpr));
  QRectF sw(r.left() + kSwatchX, r.top() + 9, 10, 10);
  p->setPen(QPen(t.line, 1));
  const bool isBody = n && n->kind == opad::Node::Kind::Body;
  if (isBody) p->setBrush(n->has_color ? QColor::fromRgbF(n->color[0], n->color[1], n->color[2]) : (hidden ? t.fg3 : t.fg2));
  else p->setBrush(t.bg);  // components and the document: hollow square
  p->drawRoundedRect(sw, 2, 2);
  QString typeIcon = isDoc ? "doc" : isBody ? "body" : "component";
  p->drawPixmap(r.left() + kTypeX, y, icons::pixmap(typeIcon, n && n->body_missing ? t.red : iconColor, 16, dpr));
  if (isDoc) {
    p->setFont(theme::ui(13));
    p->setPen(t.fg);
    p->drawText(QRect(r.left() + kNameX, r.top(), r.width() - kNameX, r.height()), Qt::AlignVCenter | Qt::AlignLeft, index.data(kNameRole).toString());
    p->restore();
    return;
  }

  int x = r.left() + kNameX;
  int right = r.right() - 6;
  // trailing badges, right to left
  if (n->kind == opad::Node::Kind::Body) {
    auto it = m_doc->scene.instance_count.find(n->body_key);
    if (it != m_doc->scene.instance_count.end() && it->second > 1) {
      QString badge = QString::fromUtf8("×%1").arg(it->second);
      QFontMetrics mm(theme::mono(11));
      int w = mm.horizontalAdvance(badge) + 10;
      QRect br(right - w, r.top() + 6, w, 16);
      p->setPen(Qt::NoPen);
      p->setBrush(t.bg4);
      p->drawRoundedRect(br, 8, 8);
      p->setPen(text);
      p->setFont(theme::mono(11));
      p->drawText(br, Qt::AlignCenter, badge);
      right -= w + 6;
    }
  }
  if (n->locked) {
    p->drawPixmap(right - 12, r.top() + 8, icons::pixmap("lock", t.fg2, 12, dpr));
    right -= 18;
  }
  if (n->body_missing) {
    p->setFont(theme::ui(11));
    p->setPen(t.red);
    QString msg = tr("missing body");
    int w = QFontMetrics(theme::ui(11)).horizontalAdvance(msg);
    p->drawText(QRect(right - w, r.top(), w, r.height()), Qt::AlignVCenter, msg);
    right -= w + 4;
    p->drawPixmap(right - 14, r.top() + 7, icons::pixmap("warning", t.red, 14, dpr));
    right -= 18;
  }
  p->setFont(theme::ui(13));
  p->setPen(text);
  QString name = index.data(kNameRole).toString();
  p->drawText(QRect(x, r.top(), std::max(10, right - x), r.height()), Qt::AlignVCenter | Qt::AlignLeft, QFontMetrics(theme::ui(13)).elidedText(name, Qt::ElideRight, std::max(10, right - x)));
  p->restore();
}

QWidget* BrowserDelegate::createEditor(QWidget* parent, const QStyleOptionViewItem&, const QModelIndex&) const {
  auto* e = new QLineEdit(parent);
  e->setFrame(true);
  return e;
}

void BrowserDelegate::updateEditorGeometry(QWidget* editor, const QStyleOptionViewItem& opt, const QModelIndex&) const {
  editor->setGeometry(QRect(opt.rect.left() + kNameX - 4, opt.rect.top() + 1, opt.rect.width() - kNameX, opt.rect.height() - 2));
}

// ---------------------------------------------------------------- BrowserPanel
BrowserPanel::BrowserPanel(AppDocument* doc, QWidget* parent) : QWidget(parent), m_doc(doc) {
  auto* layout = new QVBoxLayout(this);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->setSpacing(0);
  // Filter + breadcrumb block, divided from the tree by a 1 px line (design: "browser dock").
  auto* head = new QWidget(this);
  head->setObjectName("browserHead");
  head->setAttribute(Qt::WA_StyledBackground);
  head->setStyleSheet(QString("QWidget#browserHead { border-bottom: 1px solid %1; }").arg(theme::css(theme::current().line)));
  auto* hl = new QVBoxLayout(head);
  hl->setContentsMargins(8, 12, 8, 12);
  hl->setSpacing(8);
  m_filter = new QLineEdit(head);
  m_filter->setPlaceholderText(tr("Filter objects"));
  m_filter->setClearButtonEnabled(true);
  QAction* searchIcon = m_filter->addAction(icons::icon("search", theme::current().fg3), QLineEdit::LeadingPosition);
  m_filter->setToolTip(tr("Filter objects (Ctrl+F)"));
  connect(theme::notifier(), &theme::Notifier::changed, this, [this, head, searchIcon] {
    head->setStyleSheet(QString("QWidget#browserHead { border-bottom: 1px solid %1; }").arg(theme::css(theme::current().line)));
    searchIcon->setIcon(icons::icon("search", theme::current().fg3));
    m_parentBtn->setIcon(icons::icon("chevronUp", theme::current().fg3));
    m_locateBtn->setIcon(icons::icon("locate", theme::current().fg3));
    m_expandBtn->setIcon(icons::icon("expandAll", theme::current().fg3));
    m_collapseBtn->setIcon(icons::icon("collapseAll", theme::current().fg3));
    updateBreadcrumb();
    m_tree->viewport()->update();
  });
  m_filter->setTextMargins(0, 0, 44, 0);
  {
    auto* hint = new QLabel("Ctrl+F", m_filter);
    hint->setObjectName("tertiary");
    hint->setFont(theme::mono(11));
    auto* fl = new QHBoxLayout(m_filter);
    fl->setContentsMargins(0, 0, 8, 0);
    fl->addStretch();
    fl->addWidget(hint);
  }
  hl->addWidget(m_filter);
  auto* crumbRow = new QHBoxLayout();
  crumbRow->setContentsMargins(0, 0, 0, 0);
  crumbRow->setSpacing(2);
  m_breadcrumb = new QLabel(head);
  m_breadcrumb->setTextFormat(Qt::RichText);
  m_breadcrumb->setFont(theme::ui(12));
  m_breadcrumb->setFixedHeight(16);
  m_breadcrumb->setOpenExternalLinks(false);
  m_breadcrumb->setTextInteractionFlags(Qt::LinksAccessibleByMouse);  // each parent is a link that selects it
  connect(m_breadcrumb, &QLabel::linkActivated, this, [this](const QString& href) { selectIds({href.toStdString()}); });
  crumbRow->addWidget(m_breadcrumb, 1);
  auto button = [&](const char* icon, const QString& tip) {
    auto* b = new QToolButton(head);
    b->setAutoRaise(true);
    b->setFixedSize(18, 16);
    b->setIconSize(QSize(14, 14));
    b->setToolTip(tip);
    b->setCursor(Qt::PointingHandCursor);
    b->setIcon(icons::icon(icon, theme::current().fg3));
    crumbRow->addWidget(b);
    return b;
  };
  m_parentBtn = button("chevronUp", tr("Select parent (Ctrl+Up)"));
  m_locateBtn = button("locate", tr("Scroll to the selected object"));
  m_expandBtn = button("expandAll", tr("Expand all"));
  m_collapseBtn = button("collapseAll", tr("Collapse all"));
  connect(m_parentBtn, &QToolButton::clicked, this, &BrowserPanel::selectParent);
  connect(m_locateBtn, &QToolButton::clicked, this, &BrowserPanel::scrollToSelected);
  connect(m_expandBtn, &QToolButton::clicked, this, &BrowserPanel::expandAll);
  connect(m_collapseBtn, &QToolButton::clicked, this, &BrowserPanel::collapseAll);
  hl->addLayout(crumbRow);
  layout->addWidget(head);
  m_tree = new BrowserTree(doc, this);
  m_tree->setStyleSheet("QTreeWidget { padding: 4px 0; }");
  m_tree->setItemDelegate(new BrowserDelegate(doc, m_tree));
  m_tree->setContextMenuPolicy(Qt::CustomContextMenu);
  layout->addWidget(m_tree, 1);
  m_empty = new QLabel(tr("No document"), this);
  m_empty->setObjectName("secondary");
  m_empty->setAlignment(Qt::AlignCenter);
  layout->addWidget(m_empty, 1);

  connect(m_filter, &QLineEdit::textChanged, this, [this] { applyFilter(); });
  connect(m_tree, &QTreeWidget::itemSelectionChanged, this, [this] {
    if (m_updating) return;
    updateBreadcrumb();
    emit selectionChanged(selectedIds());
  });
  connect(m_tree, &QTreeWidget::customContextMenuRequested, this, [this](const QPoint& p) { emit contextMenuRequested(m_tree->viewport()->mapToGlobal(p), selectedIds()); });
  connect(m_tree, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem* it, int) { emit fitRequested({it->data(0, kIdRole).toString().toStdString()}); });
  connect(m_tree, &BrowserTree::eyeClicked, this, [this](const std::string& id) {
    if (id.empty()) {  // document row: toggle every root
      bool anyVisible = false;
      for (const auto& r : m_doc->scene.roots) anyVisible = anyVisible || m_doc->node(r)->visible;
      for (const auto& r : m_doc->scene.roots) m_doc->run("appearance", opad::json{{"target", r}, {"visible", !anyVisible}});
      return;
    }
    const opad::Node* n = m_doc->node(id);
    if (n) m_doc->run("appearance", opad::json{{"target", id}, {"visible", !n->visible}});
  });
  connect(m_tree, &BrowserTree::swatchClicked, this, [this](const std::string& id) {
    if (m_viewer) return;
    const opad::Node* n = m_doc->node(id);
    QColor start = n && n->has_color ? QColor::fromRgbF(n->color[0], n->color[1], n->color[2]) : QColor(190, 190, 195);
    QColor c = QColorDialog::getColor(start, this, tr("Colour of %1").arg(QString::fromStdString(n ? n->name : id)));
    if (c.isValid()) m_doc->run("appearance", opad::json{{"target", id}, {"color", {c.redF(), c.greenF(), c.blueF()}}});
  });
  connect(m_tree, &QTreeWidget::itemChanged, this, [this](QTreeWidgetItem* it, int col) {
    if (m_updating || col != 0 || m_viewer) return;
    std::string id = it->data(0, kIdRole).toString().toStdString();
    QString newName = it->text(0).trimmed();
    QString oldName = it->data(0, kNameRole).toString();
    if (newName.isEmpty() || newName == oldName) { rebuild(); return; }
    m_doc->run("rename", opad::json{{"target", id}, {"name", newName.toStdString()}});
  });
  connect(m_tree, &BrowserTree::reparentRequested, this, [this](const std::vector<std::string>& ids, const std::string& parent, int index) {
    for (const auto& id : ids) {
      opad::json op{{"target", id}};
      op["parent"] = parent.empty() ? opad::json(nullptr) : opad::json(parent);
      if (index >= 0) op["index"] = index;
      try { m_doc->run("reparent", op); } catch (const std::exception& e) { emit m_doc->message(QString::fromUtf8(e.what())); }
    }
  });
  connect(doc, &AppDocument::changed, this, &BrowserPanel::rebuild);
  rebuild();
}

void BrowserPanel::focusFilter() {
  m_filter->setFocus();
  m_filter->selectAll();
}

QTreeWidgetItem* BrowserPanel::build(const std::string& id, QTreeWidgetItem* parent, std::set<std::string>& expanded) {
  const opad::Node* n = m_doc->node(id);
  if (!n) return nullptr;
  auto* item = parent ? new QTreeWidgetItem(parent) : new QTreeWidgetItem(m_tree);
  QString name = QString::fromStdString(n->name);
  item->setText(0, name);
  item->setData(0, kIdRole, QString::fromStdString(id));
  m_index[id] = item;
  item->setData(0, kNameRole, name);
  item->setData(0, Qt::UserRole, n->kind == opad::Node::Kind::Body ? "body" : "component");
  item->setFlags(item->flags() | Qt::ItemIsEditable | Qt::ItemIsDragEnabled | (n->kind == opad::Node::Kind::Component ? Qt::ItemIsDropEnabled : Qt::NoItemFlags));
  item->setToolTip(0, QString("%1\n%2").arg(name, QString::fromStdString(id)));
  for (const auto& c : n->children) build(c, item, expanded);
  item->setExpanded(expanded.empty() ? true : expanded.count(id) > 0);
  return item;
}

void BrowserPanel::rebuild() {
  trace::Scope scope("BrowserPanel::rebuild");
  m_updating = true;
  std::set<std::string> expanded;
  std::vector<std::string> selected = selectedIds();
  std::function<void(QTreeWidgetItem*)> collect = [&](QTreeWidgetItem* it) {
    if (it->isExpanded()) expanded.insert(it->data(0, kIdRole).toString().toStdString());
    for (int i = 0; i < it->childCount(); ++i) collect(it->child(i));
  };
  for (int i = 0; i < m_tree->topLevelItemCount(); ++i) collect(m_tree->topLevelItem(i));
  m_tree->clear();
  m_index.clear();
  if (m_doc->hasDocument) {
    auto* root = new QTreeWidgetItem(m_tree);
    QString docName = m_doc->doc.path.empty() ? (m_doc->browse ? tr("Viewer") : tr("Untitled")) : QString::fromStdString(m_doc->doc.path.filename().string());
    root->setText(0, docName);
    root->setData(0, kIdRole, QString());
    root->setData(0, kNameRole, docName);
    root->setData(0, Qt::UserRole, "document");
    root->setFlags((root->flags() | Qt::ItemIsDropEnabled) & ~Qt::ItemIsEditable & ~Qt::ItemIsDragEnabled);
    for (const auto& r : m_doc->scene.roots) build(r, root, expanded);
    root->setExpanded(true);
  }
  bool empty = !m_doc->hasDocument;
  m_tree->setVisible(!empty);
  m_empty->setVisible(empty);
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
    bool self = f.isEmpty() || it->data(0, kNameRole).toString().contains(f, Qt::CaseInsensitive);
    it->setHidden(!(self || child_match));
    if (!f.isEmpty() && child_match) it->setExpanded(true);
    return self || child_match;
  };
  for (int i = 0; i < m_tree->topLevelItemCount(); ++i) visit(m_tree->topLevelItem(i));
}

QTreeWidgetItem* BrowserPanel::itemFor(const std::string& id) const {
  auto it = m_index.find(id);
  return it == m_index.end() ? nullptr : it->second;
}

std::vector<std::string> BrowserPanel::selectedIds() const {
  std::vector<std::string> ids;
  for (QTreeWidgetItem* it : m_tree->selectedItems()) {
    std::string id = it->data(0, kIdRole).toString().toStdString();
    if (!id.empty()) ids.push_back(id);
  }
  return ids;
}

void BrowserPanel::setSelectedIds(const std::vector<std::string>& ids) {
  bool was = m_updating;
  m_updating = true;
  m_tree->clearSelection();
  QItemSelection sel;  // one batched select: per-item setSelected is O(n) each in QTreeWidget
  QTreeWidgetItem* first = nullptr;
  for (const auto& id : ids)
    if (auto* it = itemFor(id)) {
      for (QTreeWidgetItem* p = it->parent(); p && !p->isExpanded(); p = p->parent()) p->setExpanded(true);
      const QModelIndex idx = m_tree->indexFromItem(it);
      sel.select(idx, idx);
      if (!first) first = it;
    }
  if (!sel.isEmpty()) m_tree->selectionModel()->select(sel, QItemSelectionModel::Select | QItemSelectionModel::Rows);
  if (first) m_tree->scrollToItem(first);
  m_updating = was;
  updateBreadcrumb();
}

void BrowserPanel::selectIds(const std::vector<std::string>& ids) {
  setSelectedIds(ids);
  emit selectionChanged(selectedIds());
}

void BrowserPanel::selectParent() {
  std::vector<std::string> parents;
  for (const auto& id : selectedIds()) {
    const opad::Node* n = m_doc->node(id);
    if (!n || n->parent.empty()) continue;  // a root has no parent to go to
    if (std::find(parents.begin(), parents.end(), n->parent) == parents.end()) parents.push_back(n->parent);
  }
  if (!parents.empty()) selectIds(parents);
}

void BrowserPanel::setViewerMode(bool on) {
  m_viewer = on;
  m_tree->setDragDropMode(on ? QAbstractItemView::NoDragDrop : QAbstractItemView::InternalMove);
  m_tree->setEditTriggers(on ? QAbstractItemView::NoEditTriggers : QAbstractItemView::EditKeyPressed);
}

void BrowserPanel::expandAll() { m_tree->expandAll(); }

void BrowserPanel::scrollToSelected() {
  const auto ids = selectedIds();
  if (ids.empty()) return;
  QTreeWidgetItem* it = itemFor(ids.front());
  if (!it) return;
  for (QTreeWidgetItem* p = it->parent(); p; p = p->parent()) p->setExpanded(true);
  m_tree->scrollToItem(it, QAbstractItemView::PositionAtCenter);
}

void BrowserPanel::collapseAll() {
  m_tree->collapseAll();
  if (m_tree->topLevelItemCount() > 0) m_tree->topLevelItem(0)->setExpanded(true);  // keep the roots in view
}

void BrowserPanel::startRename(const std::string& id) {
  if (auto* it = itemFor(id)) m_tree->editItem(it, 0);
}

void BrowserPanel::updateBreadcrumb() {
  const Tokens& t = theme::current();
  auto ids = selectedIds();
  if (ids.empty()) {
    m_breadcrumb->setText(m_doc->hasDocument ? QString("<span style='color:%1'>%2</span>").arg(t.fg2.name(), tr("Document")) : QString());
    return;
  }
  QStringList parts;
  auto path = m_doc->scene.path_to(ids.front());
  for (size_t i = 0; i < path.size(); ++i) {
    bool last = i + 1 == path.size();
    const QString name = m_doc->nodeName(path[i]).toHtmlEscaped();
    if (last) parts << QString("<span style='color:%1'>%2</span>").arg(t.fg.name(), name);
    else parts << QString("<a href='%1' style='color:%2;text-decoration:none'>%3</a>").arg(QString::fromStdString(path[i]).toHtmlEscaped(), t.fg2.name(), name);
  }
  m_breadcrumb->setText(parts.join(QString("<span style='color:%1'> › </span>").arg(t.fg3.name())));
}

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
      row->setForeground(0, t.fg2);
      const QString key = row->text(0);
      row->setForeground(1, key == "key" || key == "source_op" ? t.fg3 : t.fg);
    }
  });
  connect(m_table, &QTreeWidget::itemClicked, this, [this](QTreeWidgetItem* it, int) {
    if (it->data(0, Qt::UserRole).isValid()) emit faceChosen(it->data(0, Qt::UserRole).toInt());
  });
}

void PropertiesPanel::addRow(const QString& key, const opad::json& v) {
  const Tokens& t = theme::current();
  auto* row = new QTreeWidgetItem(m_table);
  row->setText(0, key);
  row->setForeground(0, t.fg2);
  row->setText(1, fmtValue(v));
  row->setToolTip(1, fmtValue(v));
  if (v.is_number() || v.is_array() || (v.is_string() && key == "key")) row->setFont(1, theme::mono(12));
  if (key == "key" || key == "source_op") row->setForeground(1, t.fg3);
}

void PropertiesPanel::showEntity(const QString& title, const QString& subtitle, const QString& id, const opad::json& props) {
  const Tokens& t = theme::current();
  m_title->setText(title);
  m_subtitle->setText(subtitle);
  m_id->setText(id);
  m_table->clear();
  static const char* order[] = {"surface", "curve", "area", "length", "volume", "radius", "diameter", "normal", "axis", "center", "center_of_mass",
                                "start", "end", "origin", "bbox", "faces", "edges", "vertices", "solid", "instances", "opacity", "visible", "locked",
                                "transform", "world", "component", "key", "source_op"};
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
      r->setText(0, QString("%1 %2").arg(prefix).arg(e.get<int>()));
      r->setFont(0, theme::mono(12));
      r->setData(0, Qt::UserRole, e.get<int>());
      r->setToolTip(0, tr("Click to inspect"));
    }
  };
  section(tr("ADJACENT FACES"), props.value("adjacent_faces", opad::json()), "face");
  section(tr("BOUNDING EDGES"), props.value("edges", opad::json()), "edge");
}

void PropertiesPanel::clear() {
  m_title->setText(tr("Nothing selected"));
  m_subtitle->setText(tr("Click a body, or pick faces, edges and vertices with the Select filter (1–4)."));
  m_id->clear();
  m_table->clear();
}

// ---------------------------------------------------------------- AnnotationsPanel
AnnotationsPanel::AnnotationsPanel(AppDocument* doc, QWidget* parent) : QWidget(parent), m_doc(doc) {
  auto* layout = new QVBoxLayout(this);
  layout->setContentsMargins(12, 8, 12, 8);
  layout->setSpacing(8);
  auto* bar = new QHBoxLayout();
  bar->setSpacing(8);
  m_author = new QComboBox(this);
  m_status = new QComboBox(this);
  m_status->addItems({tr("All"), tr("Open"), tr("Unresolved"), tr("Resolved")});
  m_count = new QLabel(this);
  m_count->setObjectName("secondary");
  bar->addWidget(m_author, 1);
  bar->addWidget(m_status);
  bar->addWidget(m_count);
  layout->addLayout(bar);
  auto* scroll = new QScrollArea(this);
  scroll->setWidgetResizable(true);
  scroll->setFrameShape(QFrame::NoFrame);
  m_cards = new QWidget(scroll);
  auto* cl = new QVBoxLayout(m_cards);
  cl->setContentsMargins(0, 0, 0, 0);
  cl->setSpacing(8);
  cl->addStretch();
  scroll->setWidget(m_cards);
  layout->addWidget(scroll, 1);
  auto* add = new QPushButton(tr("Add note   N"), this);
  add->setObjectName("primary");
  layout->addWidget(add);
  connect(add, &QPushButton::clicked, this, &AnnotationsPanel::addRequested);
  connect(m_author, &QComboBox::currentIndexChanged, this, [this](int) { rebuild(); });
  connect(m_status, &QComboBox::currentIndexChanged, this, [this](int) { rebuild(); });
  connect(doc, &AppDocument::changed, this, &AnnotationsPanel::rebuild);
  connect(theme::notifier(), &theme::Notifier::changed, this, &AnnotationsPanel::rebuild);
}

void AnnotationsPanel::rebuild() {
  const Tokens& t = theme::current();
  QString currentAuthor = m_author->currentText();
  std::set<std::string> authors;
  std::set<std::string> deleted(m_doc->scene.deleted_ops.begin(), m_doc->scene.deleted_ops.end());
  std::set<std::string> unresolved;
  for (const auto& a : m_doc->scene.annotations) if (a.unresolved) unresolved.insert(a.id);
  for (const auto& op : m_doc->doc.ops) if (op.type == "annotation") authors.insert(op.data.value("by", ""));
  m_author->blockSignals(true);
  m_author->clear();
  m_author->addItem(tr("All authors"));
  for (const auto& a : authors) m_author->addItem(QString::fromStdString(a));
  int idx = m_author->findText(currentAuthor);
  m_author->setCurrentIndex(idx < 0 ? 0 : idx);
  m_author->blockSignals(false);
  std::string filterAuthor = m_author->currentIndex() > 0 ? m_author->currentText().toStdString() : std::string();
  int status = m_status->currentIndex();

  auto* cl = static_cast<QVBoxLayout*>(m_cards->layout());
  while (cl->count() > 1) {
    QLayoutItem* it = cl->takeAt(0);
    delete it->widget();
    delete it;
  }
  int total = 0, shown = 0;
  for (const auto& op : m_doc->doc.ops) {
    if (op.type != "annotation") continue;
    ++total;
    std::string by = op.data.value("by", "");
    bool resolved = deleted.count(op.id) > 0, unres = unresolved.count(op.id) > 0;
    QString state = resolved ? "resolved" : unres ? "unresolved" : "open";
    if (!filterAuthor.empty() && by != filterAuthor) continue;
    if ((status == 1 && state != "open") || (status == 2 && state != "unresolved") || (status == 3 && state != "resolved")) continue;
    ++shown;
    opad::Ref anchor;
    try { anchor = opad::Ref::from_json(op.data["anchor"]); } catch (...) {}
    auto* card = new QFrame(m_cards);
    card->setObjectName("card");
    card->setProperty("state", state);
    card->setCursor(Qt::PointingHandCursor);
    auto* v = new QVBoxLayout(card);
    v->setContentsMargins(8, 8, 8, 8);
    v->setSpacing(6);
    auto* head = new QHBoxLayout();
    head->setSpacing(6);
    auto* dot = new QLabel(card);
    QPixmap dp(8, 8);
    dp.fill(Qt::transparent);
    { QPainter p(&dp); p.setRenderHint(QPainter::Antialiasing); p.setPen(Qt::NoPen); p.setBrush(unres ? t.red : authorColor(by)); p.drawEllipse(0, 0, 8, 8); }
    dot->setPixmap(dp);
    head->addWidget(dot);
    auto* author = new QLabel(QString::fromStdString(by), card);
    author->setFont(theme::ui(13, QFont::Medium));
    head->addWidget(author);
    auto* time = new QLabel(QString::fromStdString(op.data.value("ts", "")).left(16).replace('T', ' '), card);
    time->setObjectName("secondary");
    head->addWidget(time, 1);
    auto* id = new QLabel(shortId(op.id), card);
    id->setObjectName("tertiary");
    id->setFont(theme::mono(11));
    head->addWidget(id);
    v->addLayout(head);
    auto* text = new QLabel(QString::fromStdString(op.data.value("text", "")), card);
    text->setWordWrap(true);
    v->addWidget(text);
    if (unres) {
      auto* row = new QHBoxLayout();
      auto* w = new QLabel(card);
      w->setPixmap(icons::pixmap("warning", t.red, 14, devicePixelRatioF()));
      row->addWidget(w);
      auto* l = new QLabel(QString::fromUtf8("unresolved · target %1 no longer exists").arg(shortId(anchor.body)), card);
      l->setStyleSheet(QString("color:%1; font-size:11px;").arg(t.red.name()));
      row->addWidget(l, 1);
      v->addLayout(row);
    } else if (resolved) {
      auto* row = new QHBoxLayout();
      auto* c = new QLabel(card);
      c->setPixmap(icons::pixmap("check", t.green, 14, devicePixelRatioF()));
      row->addWidget(c);
      auto* l = new QLabel(tr("resolved"), card);
      l->setStyleSheet(QString("color:%1; font-size:11px;").arg(t.green.name()));
      row->addWidget(l, 1);
      v->addLayout(row);
    }
    auto* foot = new QHBoxLayout();
    QString target = anchor.kind == opad::Ref::Kind::Point ? tr("point") : m_doc->nodeName(anchor.body);
    if (anchor.kind != opad::Ref::Kind::Body && anchor.kind != opad::Ref::Kind::Point) target += QString(" › %1 %2").arg(opad::Ref::kind_name(anchor.kind)).arg(anchor.index);
    auto* tl = new QLabel(target, card);
    tl->setObjectName("tertiary");
    tl->setFont(theme::mono(11));
    foot->addWidget(tl, 1);
    auto* btn = new QPushButton(resolved ? tr("Restore") : tr("Resolve"), card);
    btn->setObjectName("outline");
    foot->addWidget(btn);
    v->addLayout(foot);
    std::string opId = op.id, body = anchor.body;
    connect(btn, &QPushButton::clicked, this, [this, opId, resolved] { if (resolved) emit restoreRequested(opId); else emit resolveRequested(opId); });
    card->installEventFilter(this);
    card->setProperty("opId", QString::fromStdString(opId));
    card->setProperty("body", QString::fromStdString(body));
    cl->insertWidget(cl->count() - 1, card);
  }
  m_count->setText(tr("%1 of %2").arg(shown).arg(total));
  // Click on a card: make it current and select its anchor body.
  for (QObject* o : m_cards->children())
    if (auto* f = qobject_cast<QFrame*>(o)) f->removeEventFilter(this), f->installEventFilter(this);
}

bool AnnotationsPanel::eventFilter(QObject* o, QEvent* e) {
  if (e->type() == QEvent::MouseButtonPress) {
    if (auto* card = qobject_cast<QFrame*>(o)) {
      m_current = card->property("opId").toString().toStdString();
      std::string body = card->property("body").toString().toStdString();
      if (!body.empty()) emit selectNode(body);
    }
  }
  return QWidget::eventFilter(o, e);
}

// ---------------------------------------------------------------- SectionPanel
SectionPanel::SectionPanel(AppDocument* doc, QWidget* parent) : QWidget(parent), m_doc(doc) {
  auto* layout = new QVBoxLayout(this);
  layout->setContentsMargins(12, 8, 12, 8);
  layout->setSpacing(8);
  m_state = new QLabel(this);
  m_state->setObjectName("secondary");
  layout->addWidget(m_state);
  auto* axisLabel = new QLabel(tr("AXIS"), this);
  axisLabel->setObjectName("sectionHeader");
  layout->addWidget(axisLabel);
  auto* seg = new QWidget(this);
  seg->setObjectName("segmented");
  auto* sl = new QHBoxLayout(seg);
  sl->setContentsMargins(1, 1, 1, 1);
  sl->setSpacing(0);
  const char* names[] = {"X", "Y", "Z", "Pick face"};
  for (int i = 0; i < 4; ++i) {
    auto* b = new QToolButton(seg);
    b->setObjectName("segmentPrimary");
    b->setText(names[i]);
    b->setCheckable(true);
    b->setChecked(i == 2);
    b->setFont(i < 3 ? theme::mono(12) : theme::ui(12));
    b->setFixedHeight(26);
    b->setAutoRaise(true);
    sl->addWidget(b, 1);
    m_axisButtons << b;
    connect(b, &QToolButton::clicked, this, [this, i] {
      if (i == 3) { beginPick(); return; }
      for (int k = 0; k < 4; ++k) m_axisButtons[k]->setChecked(k == i);
      m_pick = false;
      m_axis = i;
      emitChange();
    });
  }
  layout->addWidget(seg);
  auto* offLabel = new QLabel(tr("OFFSET"), this);
  offLabel->setObjectName("sectionHeader");
  layout->addWidget(offLabel);
  auto* row = new QHBoxLayout();
  m_slider = new QSlider(Qt::Horizontal, this);
  m_slider->setRange(0, 1000);
  m_slider->setValue(500);
  row->addWidget(m_slider, 1);
  m_value = new QLineEdit(this);
  m_value->setObjectName("mono");
  m_value->setFixedWidth(88);
  m_value->setFont(theme::mono(12));
  m_value->setAlignment(Qt::AlignRight);
  row->addWidget(m_value);
  layout->addLayout(row);
  auto* toggles = new QHBoxLayout();
  m_flipButton = new QToolButton(this);
  m_flipButton->setObjectName("segment");
  m_flipButton->setText(tr("Flip   Shift+X"));
  m_flipButton->setCheckable(true);
  m_flipButton->setFixedHeight(28);
  m_capButton = new QToolButton(this);
  m_capButton->setObjectName("segment");
  m_capButton->setText(tr("Cap faces"));
  m_capButton->setCheckable(true);
  m_capButton->setChecked(true);
  m_capButton->setFixedHeight(28);
  auto styleToggles = [this] {
    for (QToolButton* b : {m_flipButton, m_capButton})
      b->setStyleSheet(QString("QToolButton { border: 1px solid %1; border-radius: 3px; padding: 0 10px; } QToolButton:checked { background: %2; border-color: %3; }")
                           .arg(theme::css(theme::current().line), theme::css(theme::current().selbg), theme::css(theme::current().sel)));
  };
  styleToggles();
  connect(theme::notifier(), &theme::Notifier::changed, this, styleToggles);
  for (QToolButton* b : {m_flipButton, m_capButton}) toggles->addWidget(b);
  toggles->addStretch();
  layout->addLayout(toggles);
  auto* namedLabel = new QLabel(tr("NAMED SECTIONS"), this);
  namedLabel->setObjectName("sectionHeader");
  layout->addWidget(namedLabel);
  m_named = new QListWidget(this);
  layout->addWidget(m_named, 1);
  auto* save = new QPushButton(tr("Save as named section"), this);
  save->setObjectName("primary");
  layout->addWidget(save);

  connect(m_slider, &QSlider::valueChanged, this, [this](int) { emitChange(); });
  connect(m_value, &QLineEdit::editingFinished, this, [this] {
    opad::Vec3 lo, hi;
    if (!opad::scene_bbox(m_doc->doc, m_doc->scene, {}, lo, hi)) return;
    double v = m_value->text().section(' ', 0, 0).toDouble();
    if (m_pick) {
      double dmin, dmax;
      if (pickRange(dmin, dmax)) m_slider->setValue(static_cast<int>(std::clamp((v - dmin) / (dmax - dmin), 0.0, 1.0) * 1000));
      return;
    }
    double range = hi[m_axis] - lo[m_axis];
    if (range > 0) m_slider->setValue(static_cast<int>(std::clamp((v - lo[m_axis]) / range, 0.0, 1.0) * 1000));
  });
  connect(m_flipButton, &QToolButton::toggled, this, [this](bool on) { m_flip = on; emitChange(); });
  connect(m_capButton, &QToolButton::toggled, this, [this](bool) { emitChange(); });
  connect(save, &QPushButton::clicked, this, [this] {
    QString name = QString("Section %1").arg(m_doc->scene.sections.size() + 1);
    emit saveRequested(name, origin(), normal());
  });
  connect(m_named, &QListWidget::itemActivated, this, [this](QListWidgetItem* it) { applyNamed(it->data(Qt::UserRole).toString().toStdString()); });
  connect(doc, &AppDocument::changed, this, &SectionPanel::rebuild);
  rebuild();
  emitChange();
}

bool SectionPanel::pickRange(double& dmin, double& dmax) const {
  opad::Vec3 lo, hi;
  if (!opad::scene_bbox(m_doc->doc, m_doc->scene, {}, lo, hi)) return false;
  dmin = 1e300;
  dmax = -1e300;
  for (int i = 0; i < 8; ++i) {
    const double x = (i & 1) ? hi[0] : lo[0], y = (i & 2) ? hi[1] : lo[1], z = (i & 4) ? hi[2] : lo[2];
    const double d = x * m_pickNormal[0] + y * m_pickNormal[1] + z * m_pickNormal[2];
    dmin = std::min(dmin, d);
    dmax = std::max(dmax, d);
  }
  return dmax > dmin;
}

opad::Vec3 SectionPanel::origin() const {
  if (m_pick) {
    // The slider slides the picked plane along its normal; 0..1000 spans the model in that direction.
    double dmin, dmax;
    if (!pickRange(dmin, dmax)) return m_pickOrigin;
    const double d = dmin + m_slider->value() / 1000.0 * (dmax - dmin);
    const double d0 = m_pickOrigin[0] * m_pickNormal[0] + m_pickOrigin[1] * m_pickNormal[1] + m_pickOrigin[2] * m_pickNormal[2];
    return {m_pickOrigin[0] + m_pickNormal[0] * (d - d0), m_pickOrigin[1] + m_pickNormal[1] * (d - d0), m_pickOrigin[2] + m_pickNormal[2] * (d - d0)};
  }
  opad::Vec3 lo{0, 0, 0}, hi{0, 0, 0};
  if (!opad::scene_bbox(m_doc->doc, m_doc->scene, {}, lo, hi)) return {0, 0, 0};
  opad::Vec3 o{(lo[0] + hi[0]) / 2, (lo[1] + hi[1]) / 2, (lo[2] + hi[2]) / 2};
  o[m_axis] = lo[m_axis] + m_slider->value() / 1000.0 * (hi[m_axis] - lo[m_axis]);
  return o;
}

opad::Vec3 SectionPanel::normal() const {
  if (m_pick) return m_flip ? opad::Vec3{-m_pickNormal[0], -m_pickNormal[1], -m_pickNormal[2]} : m_pickNormal;
  opad::Vec3 n{0, 0, 0};
  n[m_axis] = m_flip ? 1 : -1;
  return n;
}

bool SectionPanel::caps() const { return m_capButton->isChecked(); }

void SectionPanel::emitChange() {
  const char axes[] = {'X', 'Y', 'Z'};
  opad::Vec3 o = origin();
  const double along = m_pick ? o[0] * m_pickNormal[0] + o[1] * m_pickNormal[1] + o[2] * m_pickNormal[2] : o[m_axis];  // pick mode: distance along the face normal
  m_value->setText(QString("%1 mm").arg(along, 0, 'f', 1));
  m_state->setText(!m_enabled ? tr("Section off · press X or use View › Section to enable")
                   : m_pick ? QString::fromUtf8("Section along the picked face = %1 mm · drag the slider, Shift+X flips").arg(along, 0, 'f', 1)
                            : QString::fromUtf8("Section %1 = %2 mm · drag the slider, Shift+X flips").arg(axes[m_axis]).arg(o[m_axis], 0, 'f', 1));
  emit planeChanged();
}

void SectionPanel::setEnabled(bool on) {
  if (m_enabled == on) return;
  m_enabled = on;
  emitChange();
  emit enabledChanged(on);
}

void SectionPanel::flip() { m_flipButton->setChecked(!m_flipButton->isChecked()); }

void SectionPanel::beginPick() {
  m_pick = true;
  for (int k = 0; k < 4; ++k) m_axisButtons[k]->setChecked(k == 3);
  m_state->setText(tr("Pick face · click a planar face in the 3D view"));
  emit pickRequested();
}

void SectionPanel::setFromFace(const opad::Vec3& origin, const opad::Vec3& normal) {
  m_pick = true;
  m_pickOrigin = origin;
  m_pickNormal = normal;
  for (int k = 0; k < 4; ++k) m_axisButtons[k]->setChecked(k == 3);
  double dmin, dmax;
  if (pickRange(dmin, dmax)) {  // start the slider at the face itself, so the plane does not jump
    const double d0 = origin[0] * normal[0] + origin[1] * normal[1] + origin[2] * normal[2];
    QSignalBlocker block(m_slider);
    m_slider->setValue(static_cast<int>(std::clamp((d0 - dmin) / (dmax - dmin), 0.0, 1.0) * 1000));
  }
  emitChange();
}

void SectionPanel::rebuild() {
  m_named->clear();
  const char axes[] = {'X', 'Y', 'Z'};
  for (const auto& s : m_doc->scene.sections) {
    int axis = std::fabs(s.normal[0]) > 0.9 ? 0 : std::fabs(s.normal[1]) > 0.9 ? 1 : 2;
    auto* it = new QListWidgetItem(icons::themed("section", 16), QString("%1        %2 = %3 mm").arg(QString::fromStdString(s.name)).arg(axes[axis]).arg(s.origin[axis], 0, 'f', 1));
    it->setData(Qt::UserRole, QString::fromStdString(s.id));
    it->setToolTip(tr("Double-click to apply"));
    m_named->addItem(it);
  }
}

void SectionPanel::applyNamed(const std::string& id) {
  for (const auto& s : m_doc->scene.sections)
    if (s.id == id) {
      m_enabled = true;
      setFromFace(s.origin, s.normal);
      emit enabledChanged(true);
      return;
    }
}

// ---------------------------------------------------------------- MeasureCard
MeasureCard::MeasureCard(QWidget* parent) : QFrame(parent) {
  setObjectName("card");
  setProperty("state", "measure");
  setFixedWidth(256);
  auto* v = new QVBoxLayout(this);
  v->setContentsMargins(12, 10, 12, 10);
  v->setSpacing(6);
  m_title = new QLabel(this);
  m_title->setFont(theme::ui(13, QFont::Medium));
  v->addWidget(m_title);
  m_value = new QLabel(this);
  m_value->setFont(theme::mono(20));
  v->addWidget(m_value);
  m_deltas = new QLabel(this);
  m_deltas->setFont(theme::mono(12));
  m_deltas->setObjectName("secondary");
  v->addWidget(m_deltas);
  m_targets = new QLabel(this);
  m_targets->setObjectName("secondary");
  m_targets->setWordWrap(true);
  v->addWidget(m_targets);
  auto* row = new QHBoxLayout();
  auto* pin = new QPushButton(tr("Pin to document   P"), this);
  pin->setObjectName("primary");
  auto* clear = new QPushButton(tr("Clear   Esc"), this);
  row->addWidget(pin, 1);
  row->addWidget(clear);
  v->addLayout(row);
  connect(pin, &QPushButton::clicked, this, &MeasureCard::pinRequested);
  connect(clear, &QPushButton::clicked, this, &MeasureCard::clearRequested);
}

void MeasureCard::setResult(const opad::json& r, const QStringList& targets) {
  QString kind = QString::fromStdString(r.value("kind", "measure"));
  m_title->setText(kind.left(1).toUpper() + kind.mid(1));
  QString unit = QString::fromStdString(r.value("unit", ""));
  if (r.contains("value")) m_value->setText(QString("%1 %2").arg(r["value"].get<double>(), 0, 'f', 3).arg(unit));
  else if (r.contains("size")) m_value->setText(QString("%1 × %2 × %3 mm").arg(r["size"][0].get<double>(), 0, 'f', 2).arg(r["size"][1].get<double>(), 0, 'f', 2).arg(r["size"][2].get<double>(), 0, 'f', 2));
  if (r.contains("delta")) {
    const auto& d = r["delta"];
    m_deltas->setText(QString::fromUtf8("ΔX %1   ΔY %2   ΔZ %3").arg(d[0].get<double>(), 0, 'f', 3).arg(d[1].get<double>(), 0, 'f', 3).arg(d[2].get<double>(), 0, 'f', 3));
    m_deltas->show();
  } else if (r.contains("diameter")) {
    m_deltas->setText(QString::fromUtf8("⌀ %1 mm").arg(r["diameter"].get<double>(), 0, 'f', 3));
    m_deltas->show();
  } else if (r.contains("supplement")) {
    m_deltas->setText(QString("supplement %1°").arg(r["supplement"].get<double>(), 0, 'f', 2));
    m_deltas->show();
  } else {
    m_deltas->hide();
  }
  m_targets->setText(targets.join(QString::fromUtf8("  ↔  ")));
  adjustSize();
}

// ---------------------------------------------------------------- ViewportChips
ViewportChips::ViewportChips(QWidget* parent) : QWidget(parent) {
  setAttribute(Qt::WA_TransparentForMouseEvents);
  setObjectName("chipsHost");
  setAutoFillBackground(true);
  auto paintHost = [this] {
    QPalette pal = palette();
    pal.setColor(QPalette::Window, theme::current().vp);
    setPalette(pal);
  };
  paintHost();
  connect(theme::notifier(), &theme::Notifier::changed, this, paintHost);
  auto* l = new QHBoxLayout(this);
  l->setContentsMargins(8, 8, 8, 8);
  l->setSpacing(6);
  m_mode = new QLabel(this);
  m_mode->setObjectName("chip");
  m_proj = new QLabel(this);
  m_proj->setObjectName("chip");
  m_section = new QLabel(this);
  m_section->setObjectName("chipSel");
  m_isolate = new QLabel(this);
  m_isolate->setObjectName("chipSel");
  l->addWidget(m_mode);
  l->addWidget(m_proj);
  l->addWidget(m_section);
  l->addWidget(m_isolate);
  l->addStretch();
}

void ViewportChips::set(const QString& mode, const QString& projection, const QString& section, const QString& isolate) {
  m_mode->setText(mode);
  m_proj->setText(projection);
  m_section->setText(section);
  m_section->setVisible(!section.isEmpty());
  m_isolate->setText(isolate);
  m_isolate->setVisible(!isolate.isEmpty());
  adjustSize();
}

// ---------------------------------------------------------------- TimelineWidget
TimelineWidget::TimelineWidget(AppDocument* doc, QWidget* parent) : QWidget(parent), m_doc(doc) {
  connect(theme::notifier(), &theme::Notifier::changed, this, qOverload<>(&QWidget::update));
  setMouseTracking(true);
  setFixedHeight(48);
  setAttribute(Qt::WA_Hover);
  connect(doc, &AppDocument::changed, this, &TimelineWidget::rebuild);
  rebuild();
}

void TimelineWidget::rebuild() {
  m_deleted = std::set<std::string>(m_doc->scene.deleted_ops.begin(), m_doc->scene.deleted_ops.end());
  m_unresolved.clear();
  for (const auto& u : m_doc->scene.unresolved) m_unresolved.insert(u.op_id);
  if (!m_current.empty() && !m_doc->doc.find_op(m_current)) m_current.clear();
  m_shown.clear();
  for (size_t i = 0; i < m_doc->doc.ops.size(); ++i)
    if (timelineShows(m_doc->doc, m_doc->doc.ops[i])) m_shown.push_back(i);
  update();
}

bool TimelineWidget::isUnresolved(const std::string& opId) const { return m_unresolved.count(opId) > 0; }

QRect TimelineWidget::markerRect(int i) const {
  const int n = static_cast<int>(m_shown.size());
  const int trackStart = 112 + 16, trackEnd = width() - 72;
  double step = 26;
  if (n > 1 && trackStart + (n - 1) * step + 18 > trackEnd) step = std::max(6.0, static_cast<double>(trackEnd - 18 - trackStart) / (n - 1));
  return QRect(static_cast<int>(trackStart + i * step), 15, 18, 18);
}

int TimelineWidget::indexAt(const QPoint& p) const {
  for (int i = static_cast<int>(m_shown.size()) - 1; i >= 0; --i)
    if (markerRect(i).adjusted(-3, -3, 3, 3).contains(p)) return i;
  return -1;
}

void TimelineWidget::setCurrentOp(const std::string& id) {
  m_current = id;
  update();
}

void TimelineWidget::step(int delta) {
  const auto& ops = m_doc->doc.ops;
  if (m_shown.empty()) return;
  int i = -1;
  for (size_t k = 0; k < m_shown.size(); ++k) if (ops[m_shown[k]].id == m_current) i = static_cast<int>(k);
  const int n = static_cast<int>(m_shown.size());
  i = i < 0 ? (delta > 0 ? 0 : n - 1) : std::clamp(i + delta, 0, n - 1);
  m_current = ops[m_shown[static_cast<size_t>(i)]].id;
  update();
  emit opClicked(m_current);
}

QString TimelineWidget::describe(const opad::Op& op) const {
  const opad::json& d = op.data;
  QString target = d.contains("target") && d["target"].is_string() ? m_doc->nodeName(d["target"].get<std::string>()) : QString();
  if (op.type == "import") return tr("Import %1").arg(QString::fromStdString(d.value("source", "")));
  if (op.type == "rename") return tr("Rename → %1").arg(QString::fromStdString(d.value("name", "")));
  if (op.type == "annotation") {
    try { return tr("Note on %1").arg(m_doc->nodeName(opad::Ref::from_json(d["anchor"]).body)); } catch (...) { return tr("Note"); }
  }
  if (op.type == "measurement") return tr("%1 measurement").arg(QString::fromStdString(d.value("kind", "")));
  if (op.type == "section") return tr("Section %1").arg(QString::fromStdString(d.value("name", "")));
  if (op.type == "view") return tr("View %1").arg(QString::fromStdString(d.value("name", "")));
  if (op.type == "delete") {
    const opad::Op* t = m_doc->doc.find_op(d.value("target", ""));
    return tr("Delete %1").arg(t ? QString::fromStdString(t->type) : shortId(d.value("target", "")));
  }
  if (op.type == "appearance") return tr("Appearance %1").arg(target);
  if (op.type == "transform") return tr("Transform %1").arg(target);
  if (op.type == "reparent") return tr("Reparent %1").arg(target);
  return QString::fromStdString(op.type);
}

void TimelineWidget::paintEvent(QPaintEvent*) {
  const Tokens& t = theme::current();
  QPainter p(this);
  p.setRenderHint(QPainter::Antialiasing);
  p.fillRect(rect(), t.bg2);
  p.setPen(QPen(t.line, 1));
  p.drawLine(0, 0, width(), 0);
  const auto& ops = m_doc->doc.ops;
  p.setFont(theme::ui(13, QFont::Medium));
  p.setPen(t.fg2);
  p.drawText(QRect(12, 6, 100, 16), Qt::AlignVCenter | Qt::AlignLeft, tr("Timeline"));
  p.setFont(theme::mono(11));
  p.setPen(t.fg3);
  size_t tomb = 0;
  for (size_t i : m_shown) tomb += m_deleted.count(ops[i].id);
  QString count = tr("%1 ops").arg(m_shown.size());
  if (tomb > 0) count += QString::fromUtf8(" · %1 tomb").arg(tomb);
  p.drawText(QRect(12, 24, 100, 16), Qt::AlignVCenter | Qt::AlignLeft, count);
  p.setPen(QPen(t.line, 1));
  p.drawLine(112, 8, 112, 40);
  p.drawLine(width() - 72, 8, width() - 72, 40);
  if (m_shown.empty() || !m_doc->hasDocument) {
    p.setFont(theme::ui(12));
    p.setPen(t.fg3);
    p.drawText(QRect(128, 0, width() - 200, height()), Qt::AlignVCenter | Qt::AlignLeft, tr("One marker per operation. Import a file to start the log."));
  }
  const qreal dpr = devicePixelRatioF();
  QRect last;
  for (size_t k = 0; k < m_shown.size(); ++k) {
    const size_t i = m_shown[k];
    QRect r = markerRect(static_cast<int>(k));
    last = r;
    const bool deleted = m_deleted.count(ops[i].id) > 0, unresolved = isUnresolved(ops[i].id);
    const bool current = ops[i].id == m_current, hovered = static_cast<int>(k) == m_hover;
    QColor fill = t.bg4, iconColor = t.fg;
    if (ops[i].type == "annotation") { fill = t.amber; iconColor = QColor("#1e1f22"); }
    if (current) { fill = t.sel; iconColor = t.onsel; }
    p.setPen(Qt::NoPen);
    if (deleted) {
      p.setBrush(Qt::NoBrush);
      p.setPen(QPen(t.fg2, 1.5, Qt::DashLine));
      iconColor = t.fg2;
    } else {
      p.setBrush(fill);
    }
    p.drawRoundedRect(r, 2, 2);
    if (unresolved && !deleted) {
      p.setBrush(Qt::NoBrush);
      p.setPen(QPen(t.red, 1.5));
      p.drawRoundedRect(r, 2, 2);
    }
    if (hovered || current) {
      p.setBrush(Qt::NoBrush);
      p.setPen(QPen(current ? t.sel : t.hov, 1.5));
      p.drawRoundedRect(r.adjusted(-2, -2, 2, 2), 3, 3);
    }
    p.drawPixmap(r.left() + 3, r.top() + 3, icons::pixmap(opTypeIcon(ops[i].type), iconColor, 12, dpr));
  }
  if (!m_shown.empty()) {
    int x = last.right() + 9;
    p.setPen(Qt::NoPen);
    p.setBrush(t.sel);
    p.drawRect(x - 1, 8, 2, 32);
    QPainterPath tri;
    tri.moveTo(x - 3, 8);
    tri.lineTo(x + 3, 8);
    tri.lineTo(x, 12);
    tri.closeSubpath();
    p.drawPath(tri);
  }
  m_prevBtn = QRect(width() - 60, 12, 24, 24);
  m_nextBtn = QRect(width() - 32, 12, 24, 24);
  for (const QRect& b : {m_prevBtn, m_nextBtn}) {
    if (b.contains(mapFromGlobal(QCursor::pos()))) { p.setPen(Qt::NoPen); p.setBrush(t.bg3); p.drawRoundedRect(b, 3, 3); }
  }
  p.setFont(theme::ui(14));
  p.setPen(m_shown.empty() ? t.fg3 : t.fg2);
  p.drawText(m_prevBtn, Qt::AlignCenter, QString::fromUtf8("‹"));
  p.drawText(m_nextBtn, Qt::AlignCenter, QString::fromUtf8("›"));
}

void TimelineWidget::mouseMoveEvent(QMouseEvent* e) {
  int i = indexAt(e->pos());
  if (i != m_hover) { m_hover = i; update(); }
  if (i >= 0) {
    const Tokens& t = theme::current();
    const opad::Op& op = m_doc->doc.ops[m_shown[static_cast<size_t>(i)]];
    QColor sw = op.type == "annotation" ? t.amber : m_deleted.count(op.id) ? t.fg2 : t.bg4;
    QString target;
    if (op.data.contains("target") && op.data["target"].is_string()) target = m_doc->nodeName(op.data["target"].get<std::string>());
    QString html = QString("<div style='width:248px'><table cellspacing='0' cellpadding='0'><tr><td style='background:%1;width:14px;height:14px;'>&nbsp;&nbsp;&nbsp;</td><td>&nbsp;<b>%2</b>&nbsp;&nbsp;<span style='color:%3;font-family:%4;font-size:11px'>%5</span></td></tr></table>"
                           "<div style='color:%6'>%7 · %8</div>%9<div style='color:%3;font-size:11px'>%10</div></div>")
                       .arg(sw.name(), describe(op).toHtmlEscaped(), t.fg3.name(), theme::mono().family(), shortId(op.id), t.fg2.name(),
                            QString::fromStdString(op.data.value("by", "")).toHtmlEscaped(), QString::fromStdString(op.data.value("ts", "")).left(16).replace('T', ' '),
                            target.isEmpty() ? QString() : QString("<div>target %1</div>").arg(target.toHtmlEscaped()),
                            m_deleted.count(op.id) ? tr("tombstoned · right-click to restore") : isUnresolved(op.id) ? tr("unresolved · kept, never hidden") : tr("Right-click for actions"));
    QToolTip::showText(e->globalPosition().toPoint() + QPoint(0, 8), html, this);
  } else {
    QToolTip::hideText();
  }
  update();
}

void TimelineWidget::mousePressEvent(QMouseEvent* e) {
  if (m_prevBtn.contains(e->pos())) return step(-1);
  if (m_nextBtn.contains(e->pos())) return step(+1);
  int i = indexAt(e->pos());
  if (i < 0) return;
  const std::string id = m_doc->doc.ops[m_shown[static_cast<size_t>(i)]].id;
  m_current = id;
  update();
  if (e->button() == Qt::RightButton) emit contextRequested(id, e->globalPosition().toPoint());
  else emit opClicked(id);
}

void TimelineWidget::leaveEvent(QEvent*) {
  m_hover = -1;
  update();
}

// ---------------------------------------------------------------- CommandPalette
namespace {
int fuzzyScore(const QString& text, const QString& query, QList<int>* positions) {
  if (query.isEmpty()) return 1;
  int score = 0, qi = 0, last = -2;
  QString lt = text.toLower(), lq = query.toLower();
  for (int i = 0; i < lt.size() && qi < lq.size(); ++i) {
    if (lt[i] == lq[qi]) {
      score += (i == last + 1) ? 3 : 1;
      if (i == 0 || lt[i - 1] == ' ') score += 2;
      if (positions) positions->append(i);
      last = i;
      ++qi;
    }
  }
  return qi == lq.size() ? score : 0;
}

class PaletteDelegate : public QStyledItemDelegate {
 public:
  QString query;
  using QStyledItemDelegate::QStyledItemDelegate;
  void paint(QPainter* p, const QStyleOptionViewItem& opt, const QModelIndex& index) const override {
    const Tokens& t = theme::current();
    auto* a = static_cast<QAction*>(index.data(Qt::UserRole).value<void*>());
    if (!a) return;
    bool sel = opt.state & QStyle::State_Selected;
    p->save();
    p->setRenderHint(QPainter::Antialiasing);
    if (sel) p->fillRect(opt.rect, t.sel);
    else if (opt.state & QStyle::State_MouseOver) p->fillRect(opt.rect, t.bg4);
    QColor fg = sel ? t.onsel : (a->isEnabled() ? t.fg : t.fg3);
    QString iconName = a->data().toString();
    if (!iconName.isEmpty()) p->drawPixmap(opt.rect.left() + 8, opt.rect.top() + 6, icons::pixmap(iconName, sel ? t.onsel : t.fg2, 16, p->device()->devicePixelRatioF()));
    QString text = a->text().remove('&');
    QList<int> pos;
    fuzzyScore(text, query, &pos);
    QFont normal = theme::ui(13), bold = theme::ui(13, QFont::DemiBold);
    int x = opt.rect.left() + 32;
    for (int i = 0; i < text.size(); ++i) {
      bool hit = pos.contains(i);
      p->setFont(hit ? bold : normal);
      p->setPen(fg);
      QString ch = text.mid(i, 1);
      p->drawText(QRect(x, opt.rect.top(), 40, opt.rect.height()), Qt::AlignVCenter | Qt::AlignLeft, ch);
      x += QFontMetrics(hit ? bold : normal).horizontalAdvance(ch);
    }
    p->setFont(theme::ui(11));
    p->setPen(sel ? t.onsel : t.fg3);
    p->drawText(QRect(x + 10, opt.rect.top(), 120, opt.rect.height()), Qt::AlignVCenter | Qt::AlignLeft, opGroup(a));
    QString sc = a->shortcut().toString(QKeySequence::NativeText);
    if (!sc.isEmpty()) {
      QFontMetrics mm(theme::mono(11));
      int w = mm.horizontalAdvance(sc) + 10;
      QRect key(opt.rect.right() - w - 8, opt.rect.top() + 6, w, 16);
      p->setPen(QPen(sel ? t.onsel : t.line, 1));
      p->setBrush(sel ? QColor(255, 255, 255, 40) : t.bg4);
      p->drawRoundedRect(key, 3, 3);
      p->setFont(theme::mono(11));
      p->setPen(sel ? t.onsel : t.fg2);
      p->drawText(key, Qt::AlignCenter, sc);
    }
    p->restore();
  }
  QSize sizeHint(const QStyleOptionViewItem&, const QModelIndex&) const override { return QSize(100, 28); }
};
}  // namespace

CommandPalette::CommandPalette(const QList<QAction*>& actions, QWidget* parent) : QDialog(parent, Qt::Popup | Qt::FramelessWindowHint), m_actions(actions) {
  setObjectName("overlay");
  setAttribute(Qt::WA_StyledBackground);
  setFixedWidth(560);
  auto* layout = new QVBoxLayout(this);
  layout->setContentsMargins(1, 1, 1, 1);
  layout->setSpacing(0);
  m_edit = new QLineEdit(this);
  m_edit->setObjectName("paletteInput");
  m_edit->setPlaceholderText(tr("Search commands…"));
  m_edit->addAction(icons::icon("search", theme::current().fg3), QLineEdit::LeadingPosition);
  m_list = new QListWidget(this);
  m_list->setObjectName("paletteList");
  m_list->setItemDelegate(new PaletteDelegate(m_list));
  m_list->setMouseTracking(true);
  m_list->setFixedHeight(28 * 9);
  layout->addWidget(m_edit);
  layout->addWidget(m_list);
  auto* foot = new QLabel(QString::fromUtf8("↑↓ navigate · Enter run · Esc close"), this);
  foot->setObjectName("tertiary");
  foot->setContentsMargins(12, 6, 12, 6);
  layout->addWidget(foot);
  connect(m_edit, &QLineEdit::textChanged, this, &CommandPalette::refill);
  connect(m_edit, &QLineEdit::returnPressed, this, &CommandPalette::runCurrent);
  connect(m_list, &QListWidget::itemClicked, this, [this](QListWidgetItem*) { runCurrent(); });
  m_edit->installEventFilter(this);
  refill(QString());
  m_edit->setFocus();
}

bool CommandPalette::eventFilter(QObject* o, QEvent* e) {
  if (o == m_edit && e->type() == QEvent::KeyPress) {
    auto* k = static_cast<QKeyEvent*>(e);
    if (k->key() == Qt::Key_Down || k->key() == Qt::Key_Up) {
      int row = m_list->currentRow() + (k->key() == Qt::Key_Down ? 1 : -1);
      if (row >= 0 && row < m_list->count()) m_list->setCurrentRow(row);
      return true;
    }
  }
  return QDialog::eventFilter(o, e);
}

void CommandPalette::refill(const QString& filter) {
  static_cast<PaletteDelegate*>(m_list->itemDelegate())->query = filter;
  m_list->clear();
  QList<QPair<int, QAction*>> scored;
  for (QAction* a : m_actions) {
    if (a->text().isEmpty() || a->isSeparator()) continue;
    int s = fuzzyScore(a->text().remove('&'), filter, nullptr);
    if (s > 0) scored << qMakePair(s, a);
  }
  std::stable_sort(scored.begin(), scored.end(), [](const auto& x, const auto& y) { return x.first > y.first; });
  for (const auto& [s, a] : scored) {
    auto* it = new QListWidgetItem(m_list);
    it->setData(Qt::UserRole, QVariant::fromValue(static_cast<void*>(a)));
    it->setToolTip(a->toolTip());
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
  resize(560, 520);
  auto* layout = new QVBoxLayout(this);
  m_tree = new QTreeWidget(this);
  m_tree->setColumnCount(2);
  m_tree->setHeaderLabels({tr("Command"), tr("Shortcut")});
  m_tree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
  layout->addWidget(m_tree, 1);
  for (QAction* a : actions) {
    if (a->text().isEmpty() || a->isSeparator()) continue;
    auto* it = new QTreeWidgetItem(m_tree);
    it->setText(0, opGroup(a) + " › " + a->text().remove('&'));
    auto* edit = new QKeySequenceEdit(a->shortcut(), m_tree);
    m_tree->setItemWidget(it, 1, edit);
  }
  auto* row = new QHBoxLayout();
  auto* ok = new QPushButton(tr("Apply"), this);
  ok->setObjectName("primary");
  auto* cancel = new QPushButton(tr("Cancel"), this);
  row->addStretch();
  row->addWidget(cancel);
  row->addWidget(ok);
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

// ---------------------------------------------------------------- LoadShade
LoadShade::LoadShade(QWidget* owner) : QWidget(owner, Qt::Tool | Qt::FramelessWindowHint | Qt::WindowDoesNotAcceptFocus) {
  setAttribute(Qt::WA_TranslucentBackground);
  setAttribute(Qt::WA_ShowWithoutActivating);
  setCursor(Qt::BusyCursor);
  m_timer.setInterval(30);
  connect(&m_timer, &QTimer::timeout, this, [this] {
    m_angle = (m_angle + 10) % 360;
    update(QRect(m_spinner - QPoint(40, 40), QSize(80, 80)));
  });
  connect(theme::notifier(), &theme::Notifier::changed, this, qOverload<>(&QWidget::update));
}

void LoadShade::place(const QRect& globalArea, const QPoint& spinnerCentreGlobal) {
  setGeometry(globalArea);
  m_spinner = spinnerCentreGlobal - globalArea.topLeft();
  update();
}

void LoadShade::showEvent(QShowEvent*) { m_timer.start(); }
void LoadShade::hideEvent(QHideEvent*) { m_timer.stop(); }

void LoadShade::paintEvent(QPaintEvent*) {
  const Tokens& t = theme::current();
  QPainter p(this);
  p.setRenderHint(QPainter::Antialiasing);
  p.fillRect(rect(), QColor(0, 0, 0, t.dark ? 120 : 80));  // a real darkening, in both themes
  const QRect card(m_spinner.x() - 36, m_spinner.y() - 36, 72, 72);
  p.setPen(QPen(t.line, 1));
  p.setBrush(t.bg2);
  p.drawRoundedRect(card, 6, 6);
  const QRect ring = card.adjusted(20, 20, -20, -20);
  p.setPen(QPen(t.bg4, 3));
  p.setBrush(Qt::NoBrush);
  p.drawEllipse(ring);
  p.setPen(QPen(t.sel, 3, Qt::SolidLine, Qt::RoundCap));
  p.drawArc(ring, (90 - m_angle) * 16, -100 * 16);
}

// ---------------------------------------------------------------- ProgressStrip
static QProgressBar* makeThinBar(QWidget* parent) {
  auto* bar = new QProgressBar(parent);
  bar->setTextVisible(false);
  bar->setFixedSize(110, 6);
  bar->setRange(0, 100);
  bar->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
  return bar;
}

ProgressStrip::ProgressStrip(QWidget* parent) : QWidget(parent) {
  setObjectName("progressStrip");
  setFixedHeight(20);  // must fit inside the 24 px status bar (1 px border + item margins)
  auto* l = new QHBoxLayout(this);
  l->setContentsMargins(0, 0, 0, 0);
  l->setSpacing(6);
  m_title = new QLabel(this);
  m_title->setObjectName("progressTitle");
  // The title takes every spare pixel (its width comes from the layout, not the text, so the bars never
  // shift as the phase changes); text longer than that is elided in the middle.
  m_title->setMinimumWidth(240);
  m_title->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
  m_phaseBar = makeThinBar(this);
  m_phasePct = new QLabel(this);
  m_phasePct->setObjectName("tertiary");
  m_phasePct->setFont(theme::mono(11));
  m_phasePct->setFixedWidth(30);
  m_overallLabel = new QLabel(tr("Overall"), this);
  m_overallLabel->setObjectName("tertiary");
  m_overallBar = makeThinBar(this);
  m_overallPct = new QLabel(this);
  m_overallPct->setObjectName("tertiary");
  m_overallPct->setFont(theme::mono(11));
  m_overallPct->setFixedWidth(30);
  m_cancel = new QPushButton(tr("Cancel"), this);
  m_cancel->setObjectName("progressCancel");
  m_cancel->setFixedHeight(18);
  m_cancel->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
  m_cancel->setFocusPolicy(Qt::NoFocus);
  m_cancel->setCursor(Qt::PointingHandCursor);
  l->addWidget(m_title, 1);
  l->addWidget(m_phaseBar);
  l->addWidget(m_phasePct);
  l->addSpacing(16);
  l->addWidget(m_overallLabel);
  l->addWidget(m_overallBar);
  l->addWidget(m_overallPct);
  l->addSpacing(16);
  l->addWidget(m_cancel);
  l->addSpacing(8);
  connect(m_cancel, &QPushButton::clicked, this, [this] {
    m_cancel->setEnabled(false);
    m_cancel->setText(tr("Cancelling\u2026"));
    emit cancelRequested();
  });
  hide();
}

void ProgressStrip::setTitle(const QString& text) {
  m_fullTitle = text;
  m_title->setText(m_title->fontMetrics().elidedText(text, Qt::ElideMiddle, m_title->width()));
  m_title->setToolTip(text);
}

void ProgressStrip::resizeEvent(QResizeEvent* e) {
  QWidget::resizeEvent(e);
  if (!m_fullTitle.isEmpty()) setTitle(m_fullTitle);  // re-elide for the new width
}

void ProgressStrip::begin(const QString& title, bool twoBars) {
  setTitle(title);
  m_cancel->setEnabled(true);
  m_cancel->setText(tr("Cancel"));
  m_phaseBar->setRange(0, 100);
  m_phaseBar->setValue(0);
  m_phasePct->clear();
  m_overallLabel->setVisible(twoBars);
  m_overallBar->setVisible(twoBars);
  m_overallPct->setVisible(twoBars);
  m_overallBar->setValue(0);
  m_overallPct->clear();
  show();
}

void ProgressStrip::setPhase(const QString& text, int percent) {
  setTitle(text);
  if (percent < 0) {
    m_phaseBar->setRange(0, 0);  // indeterminate (marching)
    m_phasePct->clear();
  } else {
    m_phaseBar->setRange(0, 100);
    m_phaseBar->setValue(std::clamp(percent, 0, 100));
    m_phasePct->setText(QString::number(std::clamp(percent, 0, 100)) + "%");
  }
}

void ProgressStrip::setOverall(int percent) {
  m_overallBar->setRange(0, 100);
  m_overallBar->setValue(std::clamp(percent, 0, 100));
  m_overallPct->setText(QString::number(std::clamp(percent, 0, 100)) + "%");
}

void ProgressStrip::finish() { hide(); }
