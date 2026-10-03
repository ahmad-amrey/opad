#pragma once
// Annotating a drawing sheet (TODO 11 UI-79, UI-80, UI-81): the tools that add dimensions (smart: the picks and where the
// pointer is decide horizontal, vertical, aligned, angle, radius or diameter), hole callouts and hole tables, centre marks
// and centre lines, notes with leaders, datum symbols, feature control frames, surface texture symbols and ordinate,
// baseline or chain sets on the sheet canvas, and re-attaching an annotation whose references are gone. A pick is the snap
// under the pointer on a view (SheetCanvas::pickAt: a model edge, vertex, centre or face), made a reference and measured
// on a worker (drawing::plan_dimension, plan_item); the item then follows the pointer, drawn by the core as the sheet will
// draw it (draw_item), and a click places it: one sheet_item step. Esc takes back the last pick, then leaves the tool;
// Enter places at the pointer, or ends a set's features; Tab goes to the options bar. The bar under the sheet holds what a
// tool adds (type, precision and tolerance through the units service, letter, characteristic, process, ...); with one
// annotation selected and no tool, it edits that one (a sheet_edit per change). Nothing measures on the UI thread.
// Parts lists and revision tables (UI-84) are planned as the tool starts and follow the pointer (their corner); a balloon's
// number comes with its plan, and placing one on a row whose number was not settled settles the list in the same step.
// Typed values (the UI-122 contract on the sheet): while an item follows the pointer, a value card beside it lists what
// can be typed (a dimension's offset from what it measures, decimals and tolerance; a set's offset and spacing; a frame's
// tolerance). Digits, '.', '-' and the keypad's go to the focused field, Tab and Shift+Tab move between fields, Enter places
// the item with them, Esc takes back what was typed (the field, then all) before it steps back. While a tool runs, bare
// digits never reach the window's shortcuts, and Tab stays on the canvas while the card shows.
#include <QObject>
#include <QPointer>
#include <QPointF>
#include <QStringList>
#include <QTimer>

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "SheetCanvas.hpp"
#include "opad/json.hpp"

class AppDocument;
class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QWidget;

class SheetAnnotator : public QObject, public SheetInteraction {
  Q_OBJECT
 public:
  enum class Tool {
    None, Dimension, HoleCallout, CentreMark, CentreLine, Note, Datum, Frame, Surface, Ordinate, Baseline, Chain, HoleTable, Reattach,
    PartsList, Balloon, RevisionTable  // UI-84: the tables follow the pointer from the start; a balloon after its part is picked
  };
  SheetAnnotator(AppDocument* doc, SheetCanvas* canvas, QWidget* parent);
  ~SheetAnnotator() override;
  // Commands go through this (the window's path, waiting while the document is read); then gets the result or null.
  using Runner = std::function<void(const std::string& command, const opad::json& args, std::function<void(const opad::json&)> then)>;
  void setRunner(Runner runner) { m_runner = std::move(runner); }
  QWidget* bar() const { return m_bar; }
  void start(Tool tool);
  void reattach(const std::string& item);  // picks its references again, as many as it had
  void cancel();
  Tool tool() const { return m_tool; }
  void itemsSelected(const std::vector<std::string>& items);  // the bar edits a single selected annotation
  QString nextLetter() const;  // the first datum letter the shown sheet does not use

  // State for benches.
  bool busy() const { return m_pending; }
  int pickCount() const { return static_cast<int>(m_picks.size()); }
  const opad::json& plan() const { return m_plan; }
  QString prompt() const { return m_prompt; }
  std::string chosenType() const { return m_type; }  // the dimension reading the pointer chose
  QComboBox* typeBox() const { return m_typeBox; }
  QComboBox* precisionBox() const { return m_precision; }
  QComboBox* toleranceBox() const { return m_tolBox; }
  QLineEdit* plusEdit() const { return m_plus; }
  QLineEdit* minusEdit() const { return m_minus; }
  QLineEdit* fitEdit() const { return m_fit; }
  QLineEdit* textEdit() const { return m_text; }
  QLineEdit* letterEdit() const { return m_letter; }
  QComboBox* characteristicBox() const { return m_characteristic; }
  QLineEdit* valueEdit() const { return m_value; }
  QComboBox* processBox() const { return m_process; }
  QComboBox* axisBox() const { return m_axis; }
  QComboBox* listModeBox() const { return m_listMode; }
  QCheckBox* qtyBox() const { return m_qty; }
  QWidget* card() const;                              // the value card (shown while it has fields)
  std::vector<std::string> inputKeys() const;         // its fields now: offset, decimals, plus, minus, spacing, value
  QString inputText(const std::string& key) const;    // typed, else what the pointer or the bar gives
  bool inputTyped(const std::string& key) const;
  std::string inputFocus() const;

  // SheetInteraction.
  bool mousePress(QMouseEvent* e, const QPointF& scene) override;
  bool mouseMove(QMouseEvent* e, const QPointF& scene) override;
  bool mouseRelease(QMouseEvent* e, const QPointF& scene) override;
  bool keyPress(QKeyEvent* e) override;
  bool wantsKey(QKeyEvent* e) override;
  bool active() const override { return m_tool != Tool::None; }
  // A click at a point of the sheet (scene), as the mouse gives it (benches).
  void clickAt(const QPointF& scene);
  void moveTo(const QPointF& scene);
  void finish();  // Enter: place at the pointer, or end a set's features

 signals:
  void toolChanged();
  void added(const std::string& id);  // an annotation was added (or re-attached)
  void message(const QString& text);  // a pick refused, a value not understood

 private:
  void buildBar();
  void showFields();     // the bar's fields for the tool, or for the selected annotation
  void setPrompt(const QString& text);
  void promptForStep();  // what the tool asks for now
  bool wantsPick() const;
  bool placing() const;  // a plan is there and the item follows the pointer
  void replan();         // the picks and options measured again on a worker
  void planned(const opad::json& plan);
  void updatePreview();
  // The item as the pointer would place it: its record and its measure (null when nothing is placed yet).
  std::pair<opad::json, opad::json> current();
  void commit();
  void finishReattach();
  opad::json args() const;          // what the tool adds, without picks
  opad::json tolerance() const;     // the bar's tolerance, in the sheet's units; null: none
  void fieldChanged(const char* key);  // an option changed: plan again, or edit the selected annotation
  std::string sheetId() const;
  // The value card.
  struct Input {
    std::string key;
    QString label, typed;
  };
  void syncInputs();               // its fields for the stage, beside the pointer
  bool inputKey(QKeyEvent* e);     // a typed character, Backspace, Tab, Esc: true when taken
  void applyInput(Input& in);      // a typed decimals, tolerance or frame value into the bar
  void clearInputs();
  double typedNumber(const std::string& key, double fallback) const;
  opad::json frameValue() const;   // the bar's tolerance of a feature control frame

  AppDocument* m_doc;
  QPointer<SheetCanvas> m_canvas;
  Runner m_runner;
  Tool m_tool = Tool::None;
  std::string m_view, m_item, m_type;  // the picks' view; the annotation being re-attached or edited; the reading shown
  std::vector<SheetPick> m_picks;
  opad::json m_plan;  // the worker's last answer for the picks shown
  bool m_pending = false, m_again = false, m_ending = false, m_filling = false;
  int m_needed = 0;  // re-attach: how many references
  QPointF m_mouse;
  QString m_prompt;
  QTimer m_debounce;
  QWidget* m_bar = nullptr;
  QLabel* m_title = nullptr;
  QComboBox *m_typeBox = nullptr, *m_precision = nullptr, *m_tolBox = nullptr, *m_characteristic = nullptr, *m_material = nullptr, *m_process = nullptr,
            *m_axis = nullptr, *m_listMode = nullptr;
  QLineEdit *m_plus = nullptr, *m_minus = nullptr, *m_fit = nullptr, *m_text = nullptr, *m_letter = nullptr, *m_value = nullptr, *m_datums[3] = {nullptr, nullptr, nullptr};
  QCheckBox *m_zone = nullptr, *m_qty = nullptr;
  QPushButton* m_done = nullptr;
  std::vector<std::pair<QWidget*, std::vector<Tool>>> m_fields;  // a field and the tools it belongs to
  std::vector<std::pair<QWidget*, std::vector<std::string>>> m_itemFields;  // a field and the annotation kinds it edits
  std::vector<Input> m_inputs;
  size_t m_focus = 0;
  QStringList m_barBefore;  // precision, tolerance type, plus, minus, frame value when typing began (Esc puts them back)
  double m_liveOffset = 0;  // the offset the pointer gives
  class SheetValueCard* m_card = nullptr;
  std::shared_ptr<bool> m_alive = std::make_shared<bool>(true);
};
