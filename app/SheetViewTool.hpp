#pragma once
// Section, detail and auxiliary views, crops and breaks with the mouse on the sheet canvas (TODO 11 UI-82). A tool works
// on one view (the selected one): Section takes the cutting line's points on it (snapped to what the sheet draws, kept
// level or upright within 6 degrees; a segment inclined to the first makes it an aligned section), Enter ends the line,
// then the new view follows the pointer on the side it goes (its size measured on a worker for both sides) and a click
// places it lined up with its parent: one sheet_view step.
// Detail takes a centre, a radius, then where the detail goes (twice the parent's scale, the next standard one).
// Auxiliary takes a straight edge of the view (it looks square to it), then the side. Crop takes a box (two clicks or a
// drag), Break two points (the band between them, along the longer way): one sheet_edit each. Esc steps back (the last
// point, the last stage), then leaves; points are kept in the view's own coordinates, so the sheet redrawing meanwhile
// changes nothing. Uncut, on a section view: a click on a body leaves it whole (ISO 128-50: shafts, fasteners), a click on
// it again cuts it, one sheet_edit each, until Esc. Nothing is measured on the UI thread.
#include <QObject>
#include <QPointer>
#include <QPointF>
#include <QRectF>

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "SheetCanvas.hpp"

class AppDocument;

class SheetViewTool : public QObject, public SheetInteraction {
  Q_OBJECT
 public:
  enum class Tool { None, Section, Detail, Auxiliary, Crop, Break, Uncut };  // Uncut: bodies a section draws whole
  SheetViewTool(AppDocument* doc, SheetCanvas* canvas, QObject* parent);
  ~SheetViewTool() override;
  void setRunner(SheetCanvas::Runner runner) { m_runner = std::move(runner); }
  // On `view` (a view of the shown sheet that can be drawn); false, with a message, when it cannot take the tool.
  bool start(Tool tool, const std::string& view);
  void cancel();
  Tool tool() const { return m_tool; }
  const std::string& view() const { return m_view; }
  QString prompt() const { return m_prompt; }
  int points() const { return static_cast<int>(m_points.size()); }
  bool placing() const { return m_stage == Stage::Place; }
  bool measuring() const { return m_measuring; }
  QRectF ghost() const { return m_ghost; }  // scene: the new view's frame while it follows the pointer
  static QString title(Tool tool);

  bool mousePress(QMouseEvent* e, const QPointF& scene) override;
  bool mouseMove(QMouseEvent* e, const QPointF& scene) override;
  bool mouseRelease(QMouseEvent* e, const QPointF& scene) override;
  bool keyPress(QKeyEvent* e) override;
  bool wantsKey(QKeyEvent* e) override;
  bool active() const override { return m_tool != Tool::None; }
  // As the mouse and keys do it (benches): scene points.
  void clickAt(const QPointF& scene);
  void moveTo(const QPointF& scene);
  void finish();  // Enter
  void back();    // Esc

 signals:
  void toolChanged();
  void added(const std::string& id);  // the new view, or the view cropped or broken
  void message(const QString& text);

 private:
  enum class Stage { Pick, Size, Place };
  struct Extent {
    double w = 0, h = 0;                // model mm
    opad::drawing::Vec2 centre{0, 0};   // its middle in its own view coordinates
  };
  const opad::drawing::ViewFrame* frame() const;  // the view's, as the canvas last laid it out
  opad::drawing::Vec2 toView(const QPointF& scene) const;
  QPointF toScene(opad::drawing::Vec2 view) const;
  QPointF snapped(const QPointF& scene) const;  // onto what the sheet draws
  opad::json probe(int side) const;             // the new view's record for a side (+1 left of the line / along the normal, -1 the other)
  void measure();                               // both sides' extents on a worker, then Place
  void place(const QPointF& scene);             // the ghost for the pointer
  void commit();
  void setPrompt(const QString& text);
  void promptForStage();
  void updatePreview();
  double detailScale() const;
  QString scaleLabel() const;

  AppDocument* m_doc;
  QPointer<SheetCanvas> m_canvas;
  SheetCanvas::Runner m_runner;
  Tool m_tool = Tool::None;
  Stage m_stage = Stage::Pick;
  std::string m_view;
  std::vector<opad::drawing::Vec2> m_points;  // view coordinates (model mm)
  opad::drawing::Vec2 m_edge{1, 0};           // auxiliary: the picked edge's direction
  double m_radius = 0;                        // detail: model mm
  std::map<int, Extent> m_extents;
  int m_side = 1;
  double m_gap = 20;
  bool m_measuring = false, m_pressed = false;
  QPointF m_mouse, m_press;
  QRectF m_ghost;
  QString m_prompt;
  std::shared_ptr<int> m_generation = std::make_shared<int>(0);  // a worker's answer for an earlier stage is dropped
};
