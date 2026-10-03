#pragma once
// Title block fields of a sheet's template (UI-78): where the title block's values go on a company template read from a
// DXF or DWG file (whose attributes and {placeholders} already became fields) or anywhere on a built-in one. The paper
// shows the template without those fields' values (a picture painted on a worker), the fields as boxes named by their key:
// a drag on the paper adds one, a drag on one moves it, its bottom-right grip resizes it, Del removes it; beside the paper
// the list of fields and the chosen one's key (a title block field such as title or number, or a name of one's own, which
// the sheet's values or the document properties fill), text height and alignment. A built-in title block's own cells are
// shown faint, not edited here. The result is the template with its new `fields`, for one sheet_edit.
#include <QDialog>
#include <QGraphicsView>

#include <string>
#include <vector>

#include "opad/drawing/display.hpp"
#include "opad/scene.hpp"
#include "opad/json.hpp"

class AppDocument;
class JobRunner;
class QComboBox;
class QDoubleSpinBox;
class QGraphicsPixmapItem;
class QGraphicsRectItem;
class QListWidget;

class TemplateFieldsView : public QGraphicsView {
  Q_OBJECT
 public:
  explicit TemplateFieldsView(QWidget* parent = nullptr);
  double paperH = 297;
  QRectF grip(const QRectF& box) const;  // scene: a box's resize grip, a few pixels whatever the zoom

 signals:
  void pressed(const QPointF& scene, bool onGrip);
  void dragged(const QPointF& scene);
  void released(const QPointF& scene);

 protected:
  void mousePressEvent(QMouseEvent* e) override;
  void mouseMoveEvent(QMouseEvent* e) override;
  void mouseReleaseEvent(QMouseEvent* e) override;
  void resizeEvent(QResizeEvent* e) override;
  void drawBackground(QPainter* p, const QRectF& rect) override;
};

class TemplateFieldsDialog : public QDialog {
  Q_OBJECT
 public:
  TemplateFieldsDialog(AppDocument* doc, JobRunner* jobs, const std::string& sheet, QWidget* parent);
  opad::json change() const;  // sheet_edit `set` ({"template"}), null when nothing changed
  // Benches.
  TemplateFieldsView* view() const { return m_view; }
  QComboBox* keyBox() const { return m_key; }
  bool pictured() const { return m_pictured; }
  int count() const { return static_cast<int>(m_fields.size()); }
  const opad::json& field(int i) const { return m_fields[static_cast<size_t>(i)]; }
  QPoint at(opad::drawing::Vec2 paper) const;  // the viewport's pixel there

 protected:
  void keyPressEvent(QKeyEvent* e) override;

 private:
  QRectF box(const opad::json& f) const;  // scene
  void drawTemplate();                    // the picture behind the fields
  void rebuild();                         // boxes and list from m_fields
  void select(int i);
  void edited();                          // the key, height or alignment of the chosen field
  void press(const QPointF& at, bool onGrip);
  void drag(const QPointF& at);
  void release(const QPointF& at);

  AppDocument* m_doc;
  JobRunner* m_jobs = nullptr;
  std::string m_sheet;
  opad::Sheet m_bare;  // the sheet without these fields
  opad::json m_template, m_fields;
  double m_w = 420, m_h = 297;
  TemplateFieldsView* m_view;
  QGraphicsPixmapItem* m_picture = nullptr;
  QGraphicsRectItem* m_rubber = nullptr;
  std::vector<QGraphicsRectItem*> m_boxes;
  QListWidget* m_list;
  QComboBox *m_key, *m_align;
  QDoubleSpinBox* m_height;
  int m_selected = -1;
  enum class Mode { None, Create, Move, Resize } m_mode = Mode::None;
  QPointF m_start;
  QRectF m_origin;
  bool m_pictured = false, m_filling = false;
};
