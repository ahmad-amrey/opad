#include "BrowserPanel.hpp"

#include <QAction>
#include <QColorDialog>
#include <QDropEvent>
#include <QHBoxLayout>
#include <QItemSelection>
#include <QItemSelectionModel>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QSettings>
#include <QVBoxLayout>

#include <algorithm>
#include <functional>

#include "Icons.hpp"
#include "Jobs.hpp"
#include "Theme.hpp"

using browser::kIdRole;
using browser::kNameRole;
using browser::kEyeX;
using browser::kSwatchX;
using browser::kTypeX;
using browser::kNameX;

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
    if (const auto click = badgeClick(e->pos())) { click(); return; }  // after this the row may be gone (a rebuild)
    QRect r = visualRect(idx);
    int x = e->pos().x() - r.left();
    std::string id = idx.data(kIdRole).toString().toStdString();
    const QString folder = idx.data(browser::kFolderRole).toString();  // provided folders and rows have no eye or colour
    if (folder.isEmpty() || folder == "sketches") {
      if (x >= kEyeX && x < kEyeX + 18) { emit eyeClicked(id); return; }
      if (x >= kSwatchX - 2 && x < kSwatchX + 14) { emit swatchClicked(id); return; }
    }
  }
  QTreeWidget::mousePressEvent(e);
}

void BrowserTree::mouseMoveEvent(QMouseEvent* e) {
  if (!(e->buttons() & Qt::LeftButton)) {
    if (badgeClick(e->pos())) viewport()->setCursor(Qt::PointingHandCursor);
    else viewport()->unsetCursor();
  }
  QTreeWidget::mouseMoveEvent(e);
}

void BrowserTree::mouseDoubleClickEvent(QMouseEvent* e) {
  if (e->button() == Qt::LeftButton && badgeClick(e->pos())) return;  // the press clicked it already; no fit or edit
  QTreeWidget::mouseDoubleClickEvent(e);
}

void BrowserTree::startDrag(Qt::DropActions actions) {
  if (auto* delegate = qobject_cast<BrowserDelegate*>(itemDelegate()))
    for (const QModelIndex& index : selectedIndexes())
      if (delegate->decoration(index).readOnly) return;
  QTreeWidget::startDrag(actions);
}

std::function<void()> BrowserTree::badgeClick(const QPoint& pos) const {
  const QModelIndex idx = indexAt(pos);
  auto* delegate = qobject_cast<BrowserDelegate*>(itemDelegate());
  if (!idx.isValid() || !delegate) return {};
  const browser::Decoration d = delegate->decoration(idx);
  const browser::Badge* badge = delegate->badgeAt(d, idx, visualRect(idx), pos);
  return badge ? badge->clicked : std::function<void()>();
}

void BrowserTree::drawBranches(QPainter* painter, const QRect& rect, const QModelIndex& index) const {
  if (!model()->hasChildren(index)) return;
  const Tokens& t = theme::current();
  QRect r(rect.right() - 16, rect.top() + 6, 16, 16);
  painter->drawPixmap(r.topLeft(), icons::pixmap(isExpanded(index) ? "chevronDown" : "chevronRight", t.fg3, 16, devicePixelRatioF()));
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
  hl->setContentsMargins(8, 6, 8, 6);
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
  auto* pin=button("pin",tr("Keep browser expanded"));pin->setCheckable(true);
  pin->setChecked(!QSettings().value("ui/browserAutoHide",true).toBool());
  pin->setStyleSheet("QToolButton:checked { background: #865bce; border: 1px solid #cab0ff; border-radius: 3px; }");
  connect(pin,&QToolButton::toggled,this,[this](bool on){ emit autoHideChanged(!on); });
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
  m_tree->setLayoutDirection(Qt::LeftToRight);  // the delegate paints fixed left-to-right columns; names are model data
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
  connect(m_tree, &QTreeWidget::customContextMenuRequested, this, [this](const QPoint& p) {
    QTreeWidgetItem* it = m_tree->itemAt(p);
    if (const browser::Folder* folder = providedFolder(it)) {  // its own menu, not the objects' one
      if (!folder->contextMenu) return;
      QMenu menu(this);
      folder->contextMenu(it->data(0, Qt::UserRole).toString() == "provided" ? it->data(0, kIdRole).toString().toStdString() : std::string(), menu);
      if (!menu.isEmpty()) menu.exec(m_tree->viewport()->mapToGlobal(p));
      return;
    }
    emit contextMenuRequested(m_tree->viewport()->mapToGlobal(p), selectedIds());
  });
  connect(m_tree, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem* it, int) {
    const std::string id = it->data(0, kIdRole).toString().toStdString();
    if (const browser::Folder* folder = providedFolder(it)) {
      if (folder->activated && !id.empty()) folder->activated(id);
      return;
    }
    if (it->data(0, Qt::UserRole).toString() == "sketch") emit sketchActivated(id);
    else if (!id.empty()) emit fitRequested({id});
  });
  connect(m_tree, &BrowserTree::eyeClicked, this, [this](const std::string& id) {
    if(!m_editedSketch.empty() && id==m_editedSketch){emit editedSketchVisibilityRequested();return;}
    if (id.empty()) {  // document row: toggle every root
      bool anyVisible = false;
      for (const auto& r : m_doc->scene.roots) anyVisible = anyVisible || m_doc->node(r)->visible;
      for (const auto& r : m_doc->scene.roots) m_doc->run("appearance", opad::json{{"target", r}, {"visible", !anyVisible}});
      return;
    }
    const opad::Node* n = m_doc->node(id);
    if (n) m_doc->run("appearance", opad::json{{"target", id}, {"visible", !n->visible}});
    else if (const opad::SketchItem* s = m_doc->scene.sketch(id)) m_doc->run("appearance", opad::json{{"target", id}, {"visible", !s->visible}});
  });
  connect(m_tree, &BrowserTree::swatchClicked, this, [this](const std::string& id) {  // a view setting in viewer mode too
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
    if (ids.empty()) return;
    opad::json op{{"targets", ids}};  // one step, kept in the dragged order
    op["parent"] = parent.empty() ? opad::json(nullptr) : opad::json(parent);
    if (index >= 0) op["index"] = index;
    try { m_doc->run("reparent", op); } catch (const std::exception& e) { emit m_doc->message(QString::fromUtf8(e.what())); }
  });
  connect(doc, &AppDocument::changed, this, &BrowserPanel::rebuild);
  rebuild();
}

void BrowserPanel::focusFilter() {
  m_filter->setFocus();
  m_filter->selectAll();
}
void BrowserPanel::setEditedSketch(const std::string& id,const QString& name,bool visible) {
  if(m_editedSketch==id && m_editedName==name && m_editedVisible==visible)return;
  m_editedSketch=id;m_editedName=name;m_editedVisible=visible;rebuild();
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
  QString category=QString::fromStdString(n->representation);
  if(n->kind!=opad::Node::Kind::Body) {
    category=item->childCount()?item->child(0)->data(0,Qt::UserRole+4).toString():QString();
    for(int i=1;i<item->childCount();++i) if(item->child(i)->data(0,Qt::UserRole+4).toString()!=category) {category.clear();break;}
  }
  item->setData(0,Qt::UserRole+4,category);
  item->setExpanded(expanded.empty() ? true : expanded.count(id) > 0);
  return item;
}

void BrowserPanel::rebuild() {
  trace::Scope scope("BrowserPanel::rebuild");
  m_updating = true;
  std::set<std::string> expanded;
  std::vector<std::string> selected = selectedIds();
  std::function<void(QTreeWidgetItem*)> collect = [&](QTreeWidgetItem* it) {
    if (it->isExpanded()) expanded.insert(it->data(0, Qt::UserRole).toString() == "folder" ? "folder:" + it->data(0, browser::kFolderRole).toString().toStdString() : it->data(0, kIdRole).toString().toStdString());
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
    if (!m_doc->scene.sketches.empty() || !m_editedSketch.empty()) {
      auto* folder = new QTreeWidgetItem(root);
      folder->setText(0, tr("Sketches"));
      folder->setData(0, kIdRole, QString());
      folder->setData(0, kNameRole, tr("Sketches"));
      folder->setData(0, Qt::UserRole, "folder");
      folder->setData(0, browser::kFolderRole, "sketches");
      folder->setFlags(folder->flags() & ~Qt::ItemIsEditable & ~Qt::ItemIsDragEnabled & ~Qt::ItemIsDropEnabled & ~Qt::ItemIsSelectable);
      for (const auto& s : m_doc->scene.sketches) {
        auto* item = new QTreeWidgetItem(folder);
        const QString name = QString::fromStdString(s.name);
        item->setText(0, name);
        item->setData(0, kIdRole, QString::fromStdString(s.id));
        item->setData(0, kNameRole, name);
        item->setData(0, Qt::UserRole, "sketch");
        item->setFlags(item->flags() & ~Qt::ItemIsEditable & ~Qt::ItemIsDragEnabled & ~Qt::ItemIsDropEnabled);
        item->setToolTip(0, s.error.empty() ? tr("%1\nDouble-click to edit").arg(name) : QString::fromStdString(s.error));
        m_index[s.id] = item;
      }
      if(!m_editedSketch.empty()) {
        auto* item=itemFor(m_editedSketch);
        if(!item){item=new QTreeWidgetItem(folder);item->setData(0,kIdRole,QString::fromStdString(m_editedSketch));item->setData(0,Qt::UserRole,"sketch");m_index[m_editedSketch]=item;}
        const auto label=tr("%1 (editing)").arg(m_editedName);
        item->setText(0,label);item->setData(0,kNameRole,label);item->setData(0,Qt::UserRole+8,true);item->setData(0,Qt::UserRole+9,m_editedVisible);
        item->setFlags(item->flags() & ~Qt::ItemIsEditable & ~Qt::ItemIsDragEnabled & ~Qt::ItemIsDropEnabled);
      }
      folder->setExpanded(!m_editedSketch.empty() || expanded.empty() || expanded.count("folder:sketches") > 0);
    }
    for (const auto& r : m_doc->scene.roots) build(r, root, expanded);
    QString category;
    if(root->childCount()) category=root->child(0)->data(0,Qt::UserRole+4).toString();
    for(int i=1;i<root->childCount();++i) if(root->child(i)->data(0,Qt::UserRole+4).toString()!=category) {category.clear();break;}
    root->setData(0,Qt::UserRole+4,category);
    // The areas' folders (addFolder), after Sketches: rows of their own, not nodes.
    int at = root->childCount() && root->child(0)->data(0, Qt::UserRole).toString() == "folder" ? 1 : 0;
    for (const browser::Folder& f : m_folders) {
      const std::vector<browser::Item> items = f.items ? f.items() : std::vector<browser::Item>();
      if (items.empty()) continue;
      auto* folder = new QTreeWidgetItem();
      root->insertChild(at++, folder);
      folder->setText(0, f.title);
      folder->setData(0, kIdRole, QString());
      folder->setData(0, kNameRole, f.title);
      folder->setData(0, Qt::UserRole, "folder");
      folder->setData(0, browser::kFolderRole, f.id);
      folder->setData(0, browser::kIconRole, f.icon);
      folder->setFlags(folder->flags() & ~Qt::ItemIsEditable & ~Qt::ItemIsDragEnabled & ~Qt::ItemIsDropEnabled & ~Qt::ItemIsSelectable);
      std::function<void(QTreeWidgetItem*, const browser::Item&)> add = [&](QTreeWidgetItem* parent, const browser::Item& item) {
        auto* it = new QTreeWidgetItem(parent);
        const QString id = QString::fromStdString(item.id);
        it->setText(0, item.name);
        it->setData(0, kIdRole, id);
        it->setData(0, kNameRole, item.name);
        it->setData(0, Qt::UserRole, "provided");
        it->setData(0, browser::kFolderRole, f.id);
        it->setData(0, browser::kIconRole, item.icon);
        it->setToolTip(0, item.tooltip);
        it->setFlags(it->flags() & ~Qt::ItemIsEditable & ~Qt::ItemIsDragEnabled & ~Qt::ItemIsDropEnabled);
        m_index[item.id] = it;
        for (const browser::Item& child : item.children) add(it, child);
        it->setExpanded(expanded.empty() || expanded.count(item.id) > 0);
      };
      for (const browser::Item& item : items) add(folder, item);
      folder->setExpanded(expanded.empty() || expanded.count("folder:" + f.id.toStdString()) > 0);
    }
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

void BrowserPanel::addDecorator(browser::Decorator decorator) {
  if (auto* delegate = qobject_cast<BrowserDelegate*>(m_tree->itemDelegate())) delegate->addDecorator(std::move(decorator));
  refreshDecorations();
}

void BrowserPanel::addFolder(browser::Folder folder) {
  m_folders.push_back(std::move(folder));
  rebuild();
}

void BrowserPanel::refreshDecorations() { m_tree->viewport()->update(); }

const browser::Folder* BrowserPanel::providedFolder(const QTreeWidgetItem* item) const {
  const QString id = item ? item->data(0, browser::kFolderRole).toString() : QString();
  for (const browser::Folder& f : m_folders)
    if (!id.isEmpty() && f.id == id) return &f;
  return nullptr;
}

QTreeWidgetItem* BrowserPanel::itemFor(const std::string& id) const {
  auto it = m_index.find(id);
  return it == m_index.end() ? nullptr : it->second;
}

bool BrowserPanel::isProvided(const std::string& id) const {
  const QTreeWidgetItem* item = itemFor(id);
  return item && item->data(0, Qt::UserRole).toString() == "provided";
}

QString BrowserPanel::rowName(const std::string& id) const {
  const QTreeWidgetItem* item = itemFor(id);
  return item ? item->text(0) : QString();
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
  QTreeWidgetItem* it = path.empty() ? itemFor(ids.front()) : nullptr;
  if (it && it->data(0, Qt::UserRole).toString() == "provided")  // a provided folder's row: the folder, then the rows above it
    for (bool last = true; it && it->data(0, Qt::UserRole).toString() != "document"; it = it->parent(), last = false) {
      const QString name = it->data(0, kNameRole).toString().toHtmlEscaped(), id = it->data(0, kIdRole).toString().toHtmlEscaped();
      parts.prepend(last ? QString("<span style='color:%1'>%2</span>").arg(t.fg.name(), name)
                    : id.isEmpty() ? QString("<span style='color:%1'>%2</span>").arg(t.fg2.name(), name)
                                   : QString("<a href='%1' style='color:%2;text-decoration:none'>%3</a>").arg(id, t.fg2.name(), name));
    }
  m_breadcrumb->setText(parts.join(QString("<span style='color:%1'> › </span>").arg(t.fg3.name())));
}
