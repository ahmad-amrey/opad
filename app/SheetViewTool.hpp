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
// it again cuts it, one sheet_edit each, until Esc. Breakout, on a base, projected or auxiliary view: points round what
// to open up (a smooth closed curve through them), Enter, then the depth: a click in a view square to it (the cut goes
// through that point) or Enter (the part's middle): one sheet_edit. Nothing is measured on the UI thread.
// The keyboard (UI-122): a value card beside the pointer takes what the stage measures, digits typed into the focused field
// and Tab to the next (bare digits never reach the window's shortcuts while a tool runs): a section's or auxiliary view's
// gap, a detail's radius and scale (5, 5:1 or 1:2), a crop's width and height from its first corner, a break's length from
// its first point, a broken-out section's depth below the part's front. Typed values win over the pointer's; Enter takes
// them, Esc clears them first.
#include <QObject>
#include <QPointer>
#include <QPointF>
#include <QRectF>

#include <array>
#include <map>
#include <optional>
#include <memory>
#include <string>
#include <vector>

#include "SheetCanvas.hpp"

class AppDocument;

class SheetViewTool : public QObject, public SheetInteraction {
  Q_OBJECT
 public:
  enum class Tool { None, Section, Detail, Auxiliary, Crop, Break, Uncut, Breakout };  // Uncut: bodies a section draws whole
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
  bool askingDepth() const { return m_stage == Stage::Depth; }  // breakout: the outline closed, the depth next
  bool measuring() const { return m_measuring; }
  QRectF ghost() const { return m_ghost; }  // scene: the new view's frame while it follows the pointer
  std::vector<std::string> inputs() const;   // the value card's fields now (benches)
  QString inputText(const std::string& key) const;  // a field's text: typed, else what the pointer gives
  bool cardShown() const;
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
  enum class Stage { Pick, Size, Place, Depth };
  struct Input {
    std::string key;  // gap | radius | scale | width | height | length | depth
    QString label, typed;
  };
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
  std::optional<double> depthAt(const QPointF& scene) const;  // breakout: from a point in a view square to this one
  void measureDepth();                                         // breakout: the bodies' depths on a worker (Enter: their middle)
  std::vector<opad::drawing::Vec2> outline() const;            // breakout: the curve through the points (and the pointer)
  QString scaleLabel() const;
  double scaleNow() const;                           // detail: the typed scale, else detailScale()
  std::optional<double> typed(const std::string& key) const;  // a typed value (model mm; a scale's ratio)
  std::vector<std::string> inputKeys() const;        // what the stage takes by keyboard
  opad::drawing::Vec2 sized(const QPointF& scene) const;  // crop, break: the second point, typed sizes winning
  void syncInputs();                                 // the card's fields for the stage, beside the pointer
  bool inputKey(QKeyEvent* e);                       // a typed character, Backspace, Tab, Esc: true when taken
  void clearInputs();

  AppDocument* m_doc;
  QPointer<SheetCanvas> m_canvas;
  SheetCanvas::Runner m_runner;
  Tool m_tool = Tool::None;
  Stage m_stage = Stage::Pick;
  std::string m_view;
  std::vector<opad::drawing::Vec2> m_points;  // view coordinates (model mm)
  opad::drawing::Vec2 m_edge{1, 0};           // auxiliary: the picked edge's direction
  double m_radius = 0;                        // detail: model mm
  std::array<double, 2> m_depths{0, 0};       // breakout: the bodies' least and most depth in the view
  bool m_haveDepths = false;
  double m_depth = 0;                         // breakout: the one picked
  std::map<int, Extent> m_extents;
  int m_side = 1;
  double m_gap = 20;
  bool m_measuring = false, m_pressed = false;
  QPointF m_mouse, m_press;
  QRectF m_ghost;
  QString m_prompt;
  std::vector<Input> m_inputs;
  size_t m_focus = 0;
  class SheetValueCard* m_card = nullptr;
  std::shared_ptr<int> m_generation = std::make_shared<int>(0);  // a worker's answer for an earlier stage is dropped
};
