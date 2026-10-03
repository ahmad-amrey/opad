#include "LayersPanel.hpp"

#include <QColorDialog>
#include <QComboBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPainter>
#include <QSignalBlocker>
#include <QToolButton>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <tuple>

#include "AppDocument.hpp"
#include "AreaController.hpp"
#include "Icons.hpp"
#include "PanelFooter.hpp"
#include "Theme.hpp"
#include "Viewport.hpp"

namespace {
QColor qcolor(const drawing2d::Rgb& c) { return QColor::fromRgbF(float(c[0]), float(c[1]), float(c[2])); }

// The colour cell: the layer's colour, the drawing's own (half dark, half light: it follows the background) or several.
QIcon swatch(const drawing2d::Layer& l, qreal dpr) {
  QPixmap pixmap(QSize(14, 14) * dpr);
  pixmap.setDevicePixelRatio(dpr);
  pixmap.fill(Qt::transparent);
  QPainter p(&pixmap);
  p.setRenderHint(QPainter::Antialiasing);
  const QRectF box(1.5, 1.5, 11, 11);
  if (l.mixed) {
    const QColor hues[3] = {QColor("#e2584c"), QColor("#4fbd6f"), QColor("#2f78e0")};
    for (int i = 0; i < 3; ++i) p.fillRect(QRectF(box.left() + i * box.width() / 3, box.top(), box.width() / 3, box.height()), hues[i]);
  } else if (l.colored) {
    p.fillRect(box, qcolor(l.color));
  } else {
    p.fillRect(box, qcolor(drawing2d::kInkOnLight));
    QPolygonF light({box.topRight(), box.bottomRight(), box.bottomLeft()});
    p.setBrush(qcolor(drawing2d::kInkOnDark));
    p.setPen(Qt::NoPen);
    p.drawPolygon(light);
  }
  p.setBrush(Qt::NoBrush);
  p.setPen(QPen(theme::current().fg3, 1));
  p.drawRect(box);
  return QIcon(pixmap);
}

QString weightText(double mm) { return mm < 0 ? LayersPanel::tr("Default") : LayersPanel::tr("%1 mm").arg(mm, 0, 'f', 2); }
}  // namespace

LayersPanel::LayersPanel(AreaServices& services, QWidget* parent) : QWidget(parent), m_services(services) {
  auto* layout = new QVBoxLayout(this);
  layout->setContentsMargins(12, 8, 12, 0);
  layout->setSpacing(8);
  auto* tools = new QHBoxLayout();
  tools->setSpacing(6);
  m_filter = new QLineEdit(this);
  m_filter->setPlaceholderText(tr("Filter layers"));
  m_filter->setClearButtonEnabled(true);
  connect(m_filter, &QLineEdit::textChanged, this, &LayersPanel::filter);
  tools->addWidget(m_filter, 1);
  auto button = [this, tools](const QString& icon, const QString& text, const QString& tip) {
    auto* b = new QToolButton(this);
    b->setObjectName("segment");
    b->setIcon(icons::themed(icon, 16));
    b->setText(text);
    b->setToolButtonStyle(Qt::ToolButtonIconOnly);  // the filter keeps the room; the tooltip says what it does
    b->setAccessibleName(text);
    b->setToolTip(text + " · " + tip);
    b->setFixedHeight(26);
    b->setFocusPolicy(Qt::NoFocus);
    tools->addWidget(b);
    return b;
  };
  connect(button("isolate", tr("Isolate"), tr("Show only the selected layers, the camera kept (Shift+I shows everything again)")), &QToolButton::clicked, this,
          [this] { isolate(selectedLayers()); });
  connect(button("layerWalk", tr("Walk"), tr("Layer walk: show one layer at a time and step through them (Up and Down, or the arrows)")), &QToolButton::clicked,
          this, [this] { walking() ? stopWalk() : startWalk(); });
  connect(button("eye", tr("All on"), tr("Turn every layer on and thaw it")), &QToolButton::clicked, this, [this] { allOn(); });
  layout->addLayout(tools);

  m_walkBar = new QWidget(this);
  auto* walkRow = new QHBoxLayout(m_walkBar);
  walkRow->setContentsMargins(0, 0, 0, 0);
  walkRow->setSpacing(6);
  m_walkLabel = new QLabel(m_walkBar);
  m_walkLabel->setObjectName("secondary");
  walkRow->addWidget(m_walkLabel, 1);
  for (const auto& [icon, tip, step] : {std::tuple{"chevronUp", tr("Previous layer"), -1}, {"chevronDown", tr("Next layer"), 1}}) {
    auto* b = new QToolButton(m_walkBar);
    b->setObjectName("segment");
    b->setIcon(icons::themed(icon, 16));
    b->setToolTip(tip);
    b->setFocusPolicy(Qt::NoFocus);
    connect(b, &QToolButton::clicked, this, [this, step] { walk(step); });
    walkRow->addWidget(b);
  }
  auto* stop = new QToolButton(m_walkBar);
  stop->setObjectName("segment");
  stop->setText(tr("Stop"));
  stop->setToolTip(tr("End the layer walk: every layer as it was"));
  stop->setFocusPolicy(Qt::NoFocus);
  connect(stop, &QToolButton::clicked, this, &LayersPanel::stopWalk);
  walkRow->addWidget(stop);
  m_walkBar->hide();
  layout->addWidget(m_walkBar);

  m_tree = new QTreeWidget(this);
  m_tree->setObjectName("layers");
  m_tree->setColumnCount(Columns);
  m_tree->setSelectionMode(QAbstractItemView::ExtendedSelection);
  m_tree->setUniformRowHeights(true);
  m_tree->setIconSize(QSize(16, 16));
  m_tree->setAllColumnsShowFocus(true);
  m_tree->setLayoutDirection(Qt::LeftToRight);  // fixed columns, as the browser's: an Arabic name still reads right to left
  QStringList headers{tr("Name"), QString(), QString(), QString(), QString(), tr("Linetype"), tr("Weight"), QString()};
  m_tree->setHeaderLabels(headers);
  const QStringList tips{tr("Layer"), tr("On: shown or turned off"), tr("Freeze: frozen layers are hidden and stay so while turned on"),
                         tr("Lock: a locked layer cannot be changed or picked"), tr("Colour"), tr("Linetype"), tr("Lineweight"),
                         tr("Plot: printed and exported, or left out")};
  const char* headerIcons[] = {nullptr, "eye", "freeze", "lock", nullptr, nullptr, nullptr, "plot"};
  for (int c = 0; c < Columns; ++c) {
    m_tree->headerItem()->setToolTip(c, tips[c]);
    if (headerIcons[c]) m_tree->headerItem()->setIcon(c, icons::themed(headerIcons[c], 16));
  }
  m_tree->headerItem()->setText(Colour, QString());
  m_tree->headerItem()->setIcon(Colour, icons::themed("palette", 16));
  QHeaderView* header = m_tree->header();
  header->setStretchLastSection(false);
  header->setSectionResizeMode(Name, QHeaderView::Stretch);
  for (int c : {On, Freeze, Lock, Colour, Plot}) {
    header->setSectionResizeMode(c, QHeaderView::Fixed);
    m_tree->setColumnWidth(c, 28);
  }
  header->setSectionResizeMode(Linetype, QHeaderView::ResizeToContents);
  header->setSectionResizeMode(Lineweight, QHeaderView::ResizeToContents);
  connect(m_tree, &QTreeWidget::itemClicked, this, &LayersPanel::cellClicked);
  connect(m_tree, &QTreeWidget::currentItemChanged, this, [this](QTreeWidgetItem* current) {
    if (!walking() || !current) return;
    const std::string id = current->data(Name, Qt::UserRole).toString().toStdString();
    if (!id.empty() && id != m_walked) {
      m_walked = id;
      showWalk();
    }
  });
  layout->addWidget(m_tree, 1);

  auto* statesLabel = new QLabel(tr("LAYER STATES"), this);
  statesLabel->setObjectName("sectionHeader");
  layout->addWidget(statesLabel);
  auto* statesRow = new QHBoxLayout();
  statesRow->setSpacing(6);
  m_states = new QComboBox(this);
  m_states->setToolTip(tr("Saved layer states: every layer's settings, kept as a named view of the document"));
  statesRow->addWidget(m_states, 1);
  auto small = [this, statesRow](const QString& text, const QString& tip) {
    auto* b = new QToolButton(this);
    b->setObjectName("segment");
    b->setText(text);
    b->setToolTip(tip);
    b->setFocusPolicy(Qt::NoFocus);
    statesRow->addWidget(b);
    return b;
  };
  m_restore = small(tr("Restore"), tr("Bring every layer back to the saved state (one step to undo)"));
  connect(m_restore, &QToolButton::clicked, this, [this] { restoreState(m_states->currentData().toString().toStdString()); });
  connect(small(tr("Save…"), tr("Save every layer's settings as a named state")), &QToolButton::clicked, this, [this] {
    bool ok = false;
    const QString name = QInputDialog::getText(this, tr("Save layer state"), tr("Name:"), QLineEdit::Normal, tr("Layers %1").arg(states().size() + 1), &ok);
    if (ok && !name.trimmed().isEmpty()) saveState(name.trimmed());
  });
  m_delete = small(tr("Delete"), tr("Delete the saved state"));
  connect(m_delete, &QToolButton::clicked, this, [this] {
    const QString id = m_states->currentData().toString();
    if (!id.isEmpty()) m_services.guarded([&] { m_services.document()->run("delete", {{"target", id.toStdString()}}); });
  });
  layout->addLayout(statesRow);

  m_footer = new PanelFooter(this);
  m_footer->setHint(tr("Changes apply at once · Ctrl+Z undoes"));
  m_footer->setCancelVisible(false);
  m_footer->setPrimary(tr("Done"), QString());
  layout->addWidget(m_footer);
}

QTreeWidgetItem* LayersPanel::item(const std::string& id) const {
  for (QTreeWidgetItemIterator it(m_tree); *it; ++it)
    if ((*it)->data(Name, Qt::UserRole).toString().toStdString() == id) return *it;
  return nullptr;
}

std::vector<std::string> LayersPanel::selectedLayers() const {
  std::vector<std::string> out;
  for (QTreeWidgetItemIterator it(m_tree); *it; ++it)
    if ((*it)->isSelected() && !(*it)->data(Name, Qt::UserRole).toString().isEmpty()) out.push_back((*it)->data(Name, Qt::UserRole).toString().toStdString());
  return out;
}

void LayersPanel::selectLayer(const std::string& id) {
  if (QTreeWidgetItem* row = item(id)) {
    m_tree->setCurrentItem(row);
    m_tree->scrollToItem(row);
  }
}

void LayersPanel::rebuild() {
  const std::vector<std::string> selected = selectedLayers();
  const std::string current = m_tree->currentItem() ? m_tree->currentItem()->data(Name, Qt::UserRole).toString().toStdString() : std::string();
  m_layers = drawing2d::layers(m_services.document()->scene);
  if (walking() && !layer(m_walked)) stopWalk();
  const QSignalBlocker block(m_tree);
  m_tree->clear();
  std::set<std::string> drawings;
  for (const auto& l : m_layers) drawings.insert(l.drawing);
  const bool grouped = drawings.size() > 1;  // several drawings: their layers under each
  m_tree->setRootIsDecorated(grouped);
  const Tokens& t = theme::current();
  const qreal dpr = devicePixelRatioF();
  std::map<std::string, QTreeWidgetItem*> groups;
  for (const auto& l : m_layers) {
    QTreeWidgetItem* parent = nullptr;
    if (grouped) {
      auto& group = groups[l.drawing];
      if (!group) {
        group = new QTreeWidgetItem(m_tree, {QString::fromStdString(l.drawing)});
        group->setIcon(Name, icons::themed("drawing", 16));
        group->setFirstColumnSpanned(true);
        group->setFlags(Qt::ItemIsEnabled);
        group->setExpanded(true);
      }
      parent = group;
    }
    auto* row = parent ? new QTreeWidgetItem(parent) : new QTreeWidgetItem(m_tree);
    row->setData(Name, Qt::UserRole, QString::fromStdString(l.id));
    row->setText(Name, QString::fromStdString(l.name));
    row->setToolTip(Name, QString::fromStdString(l.drawing + " › " + l.name));
    if (!l.on || l.frozen) row->setForeground(Name, t.fg3);
    row->setIcon(On, icons::icon(l.on ? "eye" : "hide", l.on ? t.fg : t.fg3));
    row->setToolTip(On, l.on ? tr("On: click to turn it off") : tr("Off: click to turn it on"));
    row->setIcon(Freeze, icons::icon(l.frozen ? "freeze" : "thaw", l.frozen ? t.sel : t.fg3));
    row->setToolTip(Freeze, l.frozen ? tr("Frozen: click to thaw") : tr("Thawed: click to freeze"));
    row->setIcon(Lock, icons::icon(l.locked ? "lock" : "unlock", l.locked ? t.locked : t.fg3));
    row->setToolTip(Lock, l.locked ? tr("Locked: click to unlock") : tr("Unlocked: click to lock"));
    row->setIcon(Colour, swatch(l, dpr));
    QString colourTip = l.mixed ? tr("Several colours: click to give the layer one")
                        : l.colored ? tr("%1: click to change").arg(qcolor(l.color).name())
                                    : tr("Drawing colour (dark on a light background, light on a dark one): click to change");
    if (l.own > 0) colourTip += "\n" + tr("Objects drawn in a colour of their own keep it");
    row->setToolTip(Colour, colourTip);
    row->setText(Linetype, l.linetype.empty() ? tr("Continuous") : QString::fromStdString(l.linetype));
    row->setText(Lineweight, weightText(l.lineweight));
    row->setIcon(Plot, icons::icon(l.plot ? "plot" : "noPlot", l.plot ? t.fg : t.fg3));
    row->setToolTip(Plot, l.plot ? tr("Plotted: click to leave it out of prints") : tr("Not plotted: click to print it"));
    if (std::find(selected.begin(), selected.end(), l.id) != selected.end()) row->setSelected(true);
    if (l.id == current) m_tree->setCurrentItem(row, Name, QItemSelectionModel::NoUpdate);
  }
  filter();
  // Layer states: the views that carry them.
  const QString chosen = m_states->currentData().toString();
  m_states->clear();
  for (const auto& [id, name] : states()) m_states->addItem(name, QString::fromStdString(id));
  if (const int i = m_states->findData(chosen); i >= 0) m_states->setCurrentIndex(i);
  m_restore->setEnabled(m_states->count() > 0);
  m_delete->setEnabled(m_states->count() > 0 && !m_services.document()->browse);
  if (walking()) showWalk();
}

void LayersPanel::filter() {
  const QString text = m_filter->text().trimmed();
  for (QTreeWidgetItemIterator it(m_tree); *it; ++it)
    if (!(*it)->data(Name, Qt::UserRole).toString().isEmpty()) (*it)->setHidden(!text.isEmpty() && !(*it)->text(Name).contains(text, Qt::CaseInsensitive));
}

void LayersPanel::apply(std::vector<std::pair<std::string, opad::json>> commands, const QString& label) {
  if (!commands.empty()) m_services.guarded([&] { m_services.document()->runAll(commands, label); });
}

void LayersPanel::cellClicked(QTreeWidgetItem* row, int column) {
  const std::string id = row ? row->data(Name, Qt::UserRole).toString().toStdString() : std::string();
  const drawing2d::Layer* l = layer(id);
  if (!l) return;
  const QPoint below = m_tree->viewport()->mapToGlobal(m_tree->visualItemRect(row).bottomLeft() + QPoint(m_tree->header()->sectionPosition(column), 0));
  if (column == Colour) {
    QMenu menu(this);
    menu.addAction(tr("Choose colour…"), this, [this, id, l] {
      const QColor c = QColorDialog::getColor(l->colored ? qcolor(l->color) : QColor(Qt::white), this, tr("Layer colour"));
      if (c.isValid()) setColor(id, {c.redF(), c.greenF(), c.blueF()});
    });
    menu.addAction(tr("Drawing colour"), this, [this, id] { setDrawingColor(id); })->setEnabled(l->colored || l->mixed);
    menu.exec(below);
  } else if (column == Linetype) {
    QMenu menu(this);
    std::vector<std::string> names = drawing2d::linetypes();
    for (const auto& other : m_layers)
      if (!other.linetype.empty() && std::none_of(names.begin(), names.end(), [&](const std::string& n) { return QString::fromStdString(n).compare(QString::fromStdString(other.linetype), Qt::CaseInsensitive) == 0; }))
        names.push_back(other.linetype);
    for (const auto& name : names) {
      QAction* a = menu.addAction(QString::fromStdString(name), this, [this, id, name] { setLinetype(id, name); });
      a->setCheckable(true);
      a->setChecked(QString::fromStdString(name).compare(QString::fromStdString(l->linetype.empty() ? "Continuous" : l->linetype), Qt::CaseInsensitive) == 0);
    }
    menu.exec(below);
  } else if (column == Lineweight) {
    QMenu menu(this);
    std::vector<double> weights{-1};
    weights.insert(weights.end(), drawing2d::lineweights().begin(), drawing2d::lineweights().end());
    for (double mm : weights) {
      QAction* a = menu.addAction(weightText(mm), this, [this, id, mm] { setLineweight(id, mm); });
      a->setCheckable(true);
      a->setChecked(std::abs(mm - l->lineweight) < 1e-6);
    }
    menu.exec(below);
  } else if (column == On || column == Freeze || column == Lock || column == Plot) {
    toggle(id, column);
  }
}

void LayersPanel::toggle(const std::string& id, int column) {
  const drawing2d::Layer* l = layer(id);
  if (!l) return;
  if (column == On) apply({{"appearance", drawing2d::setOn(*l, !l->on)}}, l->on ? tr("turn layer off") : tr("turn layer on"));
  else if (column == Freeze) apply({{"appearance", drawing2d::setFrozen(*l, !l->frozen)}}, l->frozen ? tr("thaw layer") : tr("freeze layer"));
  else if (column == Lock) apply({{"appearance", drawing2d::setLocked(*l, !l->locked)}}, l->locked ? tr("unlock layer") : tr("lock layer"));
  else if (column == Plot) apply({{"appearance", drawing2d::setPlot(*l, !l->plot)}}, tr("layer plot"));
}

void LayersPanel::setColor(const std::string& id, const drawing2d::Rgb& color) {
  if (const drawing2d::Layer* l = layer(id)) apply({{"appearance", drawing2d::setColor(*l, color)}}, tr("layer colour"));
}

void LayersPanel::setDrawingColor(const std::string& id) {
  if (const drawing2d::Layer* l = layer(id)) apply({{"appearance", drawing2d::setDefaultColor(*l)}}, tr("layer colour"));
}

void LayersPanel::setLinetype(const std::string& id, const std::string& linetype) {
  if (const drawing2d::Layer* l = layer(id)) apply({{"appearance", drawing2d::setLinetype(*l, linetype)}}, tr("layer linetype"));
}

void LayersPanel::setLineweight(const std::string& id, double mm) {
  if (const drawing2d::Layer* l = layer(id)) apply({{"appearance", drawing2d::setLineweight(*l, mm)}}, tr("layer lineweight"));
}

void LayersPanel::allOn() {
  std::vector<std::pair<std::string, opad::json>> commands;
  for (const auto& l : m_layers)
    if (!l.on || l.frozen) {
      drawing2d::Layer on = l;
      on.frozen = false;
      commands.push_back({"appearance", drawing2d::setOn(on, true)});
    }
  apply(commands, tr("turn layers on"));
}

void LayersPanel::isolate(const std::vector<std::string>& ids) {
  if (ids.empty()) return m_services.showMessage(tr("Select the layers to isolate first."));
  if (walking()) stopWalk();
  m_services.viewport()->isolate(ids, false);
}

// ---------------------------------------------------------------- layer walk
void LayersPanel::startWalk() {
  std::vector<std::string> shown;
  for (QTreeWidgetItemIterator it(m_tree); *it; ++it)
    if (!(*it)->isHidden() && !(*it)->data(Name, Qt::UserRole).toString().isEmpty()) shown.push_back((*it)->data(Name, Qt::UserRole).toString().toStdString());
  if (shown.empty()) return;
  const std::string current = m_tree->currentItem() ? m_tree->currentItem()->data(Name, Qt::UserRole).toString().toStdString() : std::string();
  m_walked = std::find(shown.begin(), shown.end(), current) != shown.end() ? current : shown.front();
  showWalk();
}

void LayersPanel::walk(int step) {
  if (!walking()) return startWalk();
  std::vector<std::string> shown;
  for (QTreeWidgetItemIterator it(m_tree); *it; ++it)
    if (!(*it)->isHidden() && !(*it)->data(Name, Qt::UserRole).toString().isEmpty()) shown.push_back((*it)->data(Name, Qt::UserRole).toString().toStdString());
  auto at = std::find(shown.begin(), shown.end(), m_walked);
  if (shown.empty() || at == shown.end()) return;
  const int n = int(shown.size()), i = int(at - shown.begin());
  m_walked = shown[size_t(((i + step) % n + n) % n)];
  showWalk();
}

void LayersPanel::showWalk() {
  m_services.viewport()->isolate({m_walked}, false);  // whatever its own state: a walk shows off and frozen layers too
  if (QTreeWidgetItem* row = item(m_walked); row && m_tree->currentItem() != row) {
    const QSignalBlocker block(m_tree);
    m_tree->setCurrentItem(row);
    m_tree->scrollToItem(row);
  }
  m_walkLabel->setText(walkText());
  m_walkBar->show();
  emit walkChanged(walkText());
}

QString LayersPanel::walkText() const {
  if (!walking()) return {};
  int index = 0, count = 0;
  for (QTreeWidgetItemIterator it(m_tree); *it; ++it)
    if (!(*it)->isHidden() && !(*it)->data(Name, Qt::UserRole).toString().isEmpty()) {
      ++count;
      if ((*it)->data(Name, Qt::UserRole).toString().toStdString() == m_walked) index = count;
    }
  const drawing2d::Layer* l = layer(m_walked);
  return tr("Layer walk · %1 (%2 of %3)").arg(l ? QString::fromStdString(l->name) : QString()).arg(index).arg(count);
}

void LayersPanel::stopWalk() {
  if (!walking()) return;
  m_walked.clear();
  m_walkBar->hide();
  if (m_services.viewport()->isIsolated()) m_services.viewport()->isolate({}, false);
  emit walkChanged({});
}

// ---------------------------------------------------------------- layer states
std::vector<std::pair<std::string, QString>> LayersPanel::states() const {
  std::vector<std::pair<std::string, QString>> out;
  for (const auto& v : m_services.document()->scene.views)
    if (v.display.is_object() && v.display.contains("layers")) out.push_back({v.id, QString::fromStdString(v.name)});
  return out;
}

void LayersPanel::saveState(const QString& name) {
  if (!m_services.requireEditable([this, name] { saveState(name); })) return;
  m_services.guarded([&] {
    m_services.document()->run("view", {{"name", name.toStdString()}, {"camera", m_services.viewport()->cameraJson()},
                                        {"display", drawing2d::captureState(m_services.document()->scene)}});
  });
  if (const auto saved = states(); !saved.empty()) m_states->setCurrentIndex(m_states->findData(QString::fromStdString(saved.back().first)));
}

void LayersPanel::restoreState(const std::string& viewId) {
  for (const auto& v : m_services.document()->scene.views)
    if (v.id == viewId) {
      std::vector<std::pair<std::string, opad::json>> commands;
      for (auto& args : drawing2d::restoreState(m_services.document()->scene, v.display)) commands.push_back({"appearance", std::move(args)});
      if (commands.empty()) return m_services.showMessage(tr("The layers are as “%1” saved them").arg(QString::fromStdString(v.name)));
      apply(commands, tr("restore layer state"));
      return;
    }
}
