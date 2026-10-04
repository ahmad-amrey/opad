#pragma once
#include <QList>
#include <QStyledItemDelegate>
#include <functional>
#include <string>
#include <vector>

#include "AppDocument.hpp"
#include "Theme.hpp"

class QMenu;

// The browser's rows: item data (BrowserPanel fills it) and the fixed columns BrowserDelegate paints and
// BrowserTree hit-tests (eye, colour swatch, type icon, name), x in px from the row's left edge.
namespace browser {
constexpr int kIdRole = Qt::UserRole + 1;
constexpr int kNameRole = Qt::UserRole + 2;
constexpr int kFolderRole = Qt::UserRole + 5;  // a folder row's id ("sketches", or a provided folder's), and a provided row's folder
constexpr int kIconRole = Qt::UserRole + 6;    // a provided folder's or row's icon
constexpr int kErrorRole = Qt::UserRole + 7;   // a provided row that is not drawn (Item::error)
constexpr int kEyeX = 2, kSwatchX = 24, kTypeX = 42, kNameX = 66;

// ---- row decorations: what a feature area adds to rows it knows something about (an asset's sync state, the active
// component, a link icon), without a column of its own. BrowserPanel::addDecorator.
struct Row {  // a row as decorators see it
  std::string id;                    // node, sketch or provided row id; empty for the document row and folders
  QString kind;                      // "document", "component", "body", "folder", "sketch" or "provided" (a Folder's row)
  QString folder;                    // folder rows and provided rows: the folder's id ("sketches", "drawings", ...)
  const opad::Node* node = nullptr;  // components and bodies
};

// A trailing badge: an icon, a short text or both, in a pill when it has a fill. Badges go right to left after the
// built-in ones (instance count, missing body), and the name is cut short before them.
struct Badge {
  QString text, icon;
  QColor Tokens::* color = &Tokens::fg2;  // text and icon
  QColor Tokens::* fill = &Tokens::bg4;   // the pill; nullptr: none
  QString tooltip;
  std::function<void()> clicked;  // a button then (pointing hand); its click neither selects nor drags the row
};

struct Decoration {
  QList<Badge> badges;
  // In the colour swatch's place, left of the type icon (a component's activation radio): its icon (14 px, its color; no
  // text, no fill), tooltip and click as above. None while lead.icon is empty; without a click there, the swatch's click.
  Badge lead;
  QString typeIcon;  // in place of the row's type icon
  QString tooltip;   // a line under the row's own tooltip
  bool italic = false, bold = false, dim = false;  // the name's style; dim greys the name and the icons (fg3)
};

// Asked for every row each time it is painted, hovered or clicked: O(1) per row, from what the area already knows (never
// geometry). Adds to `out`; decorators run in the order they were added. BrowserPanel::refreshDecorations repaints.
using Decorator = std::function<void(const Row& row, Decoration& out)>;

// ---- provided folders: top-level folders an area fills (Drawings, Assets, ...), after Sketches. BrowserPanel::addFolder.
struct Item {  // a row of a provided folder
  std::string id;  // unique among all browser ids (nodes, sketches, other folders): prefix it, e.g. "drawing:<op id>"
  QString name, icon, tooltip;
  std::vector<Item> children;
  bool editable = false;  // F2 (edit.rename) renames it in place through Folder::rename
  bool error = false;     // not drawn (a kind of a newer build, a missing parent): its icon red, its name grey, the tooltip says why
};

struct Folder {
  QString id, title, icon;  // id: "drawings"; it also keeps the folder open or closed across rebuilds
  std::function<std::vector<Item>()> items;  // asked at every rebuild (the document changed, BrowserPanel::rebuild); none: no folder
  std::function<void(const std::string& id, QMenu& menu)> contextMenu;  // right-click on a row (its id) or the folder (empty id)
  std::function<void(const std::string& id)> activated;                 // double-click on a row
  std::function<void(const std::string& id, const QString& name)> rename;  // an editable row renamed in place; throws to refuse
  std::function<bool(const std::vector<std::string>& ids)> remove;  // Del (edit.delete) on its selected rows: true when it took them
};
// Its rows are selected like nodes: BrowserPanel::selectionChanged (and the areas' selectionChanged) carry their ids.
// They have no eye, colour or drag; F2 and Del reach them through the folder's rename and remove; the view, the other
// edit commands and the tools never see them (the window keeps them out of the node selection), and Properties shows
// only the areas' sections for one (subject: a ref to its id). A row that is new since the last rebuild starts open.
}  // namespace browser

class BrowserDelegate : public QStyledItemDelegate {
  Q_OBJECT
 public:
  explicit BrowserDelegate(AppDocument* doc, QObject* parent = nullptr) : QStyledItemDelegate(parent), m_doc(doc) {}
  void paint(QPainter* p, const QStyleOptionViewItem& opt, const QModelIndex& index) const override;
  QSize sizeHint(const QStyleOptionViewItem&, const QModelIndex&) const override { return QSize(100, theme::px(28)); }  // taller at a larger text size
  QWidget* createEditor(QWidget* parent, const QStyleOptionViewItem& opt, const QModelIndex& index) const override;
  void updateEditorGeometry(QWidget* editor, const QStyleOptionViewItem& opt, const QModelIndex& index) const override;
  bool helpEvent(QHelpEvent* e, QAbstractItemView* view, const QStyleOptionViewItem& opt, const QModelIndex& index) override;
  void addDecorator(browser::Decorator decorator) { m_decorators.push_back(std::move(decorator)); }
  browser::Decoration decoration(const QModelIndex& index) const;  // what the decorators say about a row
  // A decorator's badge on the row (visual rect `row`) at `pos`, its rect in `rect`: null if there is none there.
  const browser::Badge* badgeAt(const browser::Decoration& d, const QModelIndex& index, const QRect& row, const QPoint& pos, QRect* rect = nullptr) const;
 private:
  static QRect leadRect(const QRect& row) { return QRect(row.left() + browser::kSwatchX - 3, row.center().y() - 7, 16, 16); }
  void paintLead(QPainter* p, const browser::Decoration& d, const QRect& row) const;
  int builtinBadges(QPainter* p, const QRect& r, const opad::Node* n, const QColor& text) const;  // paints them (p set); returns the x left of them
  std::vector<QRect> badgeRects(const browser::Decoration& d, const QRect& r, int right) const;  // right to left from `right`
  void paintBadges(QPainter* p, const browser::Decoration& d, const std::vector<QRect>& rects) const;
  AppDocument* m_doc;
  std::vector<browser::Decorator> m_decorators;
};
