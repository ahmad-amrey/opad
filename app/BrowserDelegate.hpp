#pragma once
#include <QStyledItemDelegate>

#include "AppDocument.hpp"

// The browser's rows: item data (BrowserPanel fills it) and the fixed columns BrowserDelegate paints and
// BrowserTree hit-tests (eye, colour swatch, type icon, name), x in px from the row's left edge.
namespace browser {
constexpr int kIdRole = Qt::UserRole + 1;
constexpr int kNameRole = Qt::UserRole + 2;
constexpr int kEyeX = 2, kSwatchX = 24, kTypeX = 42, kNameX = 66;
}  // namespace browser

class BrowserDelegate : public QStyledItemDelegate {
  Q_OBJECT
 public:
  explicit BrowserDelegate(AppDocument* doc, QObject* parent = nullptr) : QStyledItemDelegate(parent), m_doc(doc) {}
  void paint(QPainter* p, const QStyleOptionViewItem& opt, const QModelIndex& index) const override;
  QSize sizeHint(const QStyleOptionViewItem&, const QModelIndex&) const override { return QSize(100, 28); }
  QWidget* createEditor(QWidget* parent, const QStyleOptionViewItem& opt, const QModelIndex& index) const override;
  void updateEditorGeometry(QWidget* editor, const QStyleOptionViewItem& opt, const QModelIndex& index) const override;
 private:
  AppDocument* m_doc;
};
