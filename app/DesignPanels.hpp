#pragma once
// Design UI pieces: the feature form (one panel serves every feature kind; it is built from the core's spec
// table, so a new kind needs no UI code), expression fields, and the parameters dialog.
#include <QComboBox>
#include <QCheckBox>
#include <QDialog>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QWidget>
#include <functional>
#include <map>

#include "AppDocument.hpp"
#include "PanelFooter.hpp"
#include "opad/design/feature.hpp"

class ToolGuide;

// A line edit for an expression ("width / 2 + 3 mm") with its value, or what is wrong with it, underneath.
class ExprEdit : public QWidget {
  Q_OBJECT
 public:
  ExprEdit(AppDocument* doc, opad::design::Dim dim, QWidget* parent = nullptr);
  void setText(const QString& text);
  QString text() const;
  bool valid() const { return m_valid; }
  QString problem() const { return m_valid ? QString() : m_value->text(); }  // why it does not evaluate
  QLineEdit* lineEdit() const { return m_edit; }
 signals:
  void changed();
  void returnPressed();
 private:
  void evaluate();
  AppDocument* m_doc;
  opad::design::Dim m_dim;
  QLineEdit* m_edit;
  QLabel* m_value;
  bool m_valid = false;
};

// One pick input of the form: shows what is picked; clicking it makes it the input the viewport picks for.
class PickBox : public QPushButton {
  Q_OBJECT
 public:
  explicit PickBox(QWidget* parent = nullptr);
  void set(int count, const QString& what, bool active, bool satisfied);
  void setNote(const QString& note);  // what the box says instead, as if waiting (a primitive's plane: "Click in the view")
 signals:
  void cleared();
 protected:
  void paintEvent(QPaintEvent*) override;
  void mousePressEvent(QMouseEvent* e) override;
 private:
  int m_count = 0;
  QString m_what, m_note;
  bool m_active = false, m_satisfied = true;
};

class FeaturePanel : public QWidget {
  Q_OBJECT
 public:
  explicit FeaturePanel(AppDocument* doc, QWidget* parent = nullptr);
  // `inputs`: the spec's defaults for a new feature, the stored inputs when editing.
  void begin(const opad::design::FeatureSpec& spec, const opad::json& inputs, const QString& name, bool editing);
  const opad::design::FeatureSpec* spec() const { return m_spec; }
  opad::json inputs() const;      // what is shown and used; hidden (show_if) inputs keep their last value
  QString name() const { return m_name->text().trimmed(); }
  bool complete(QString* missing = nullptr) const;  // every shown pick has enough, every expression evaluates
  QString activeInput() const { return m_active; }
  const opad::design::InputSpec* input(const QString& name) const;
  void setPicks(const QString& input, const opad::json& picks);
  opad::json picks(const QString& input) const;
  void setStatus(const QString& text, bool error);
  QString statusText() const;
  void setEditHidden(bool hidden);
  void setValue(const QString& input, const opad::json& value);  // expression, choice or flag, as the user would type it
  void setValues(const std::vector<std::pair<QString, opad::json>>& values);  // several at once: one inputsChanged
  // TODO 11 P1, a primitive placed in the view: what a pick box says instead of its picks (empty: its picks), and the step
  // the guide waits at (count 0: the picks say).
  void setPickNote(const QString& input, const QString& note);
  void setGuideStep(int step, int count);
  // The values shown (lengths, angles, numbers, counts), in the form's order: what the keyboard types into (UI-122).
  QStringList valueInputs() const;
  QString valueText(const QString& input) const;  // as its field shows it
  QString problem(const QString& input) const;    // why it does not evaluate; empty: it does
  void activate(const QString& input);  // empty: none
  void activateNextPick();              // the first shown pick input that still needs picks
  // TODO 10 B14: the name, colour and component of the bodies a new feature makes, shown while its operation is
  // "new". Left alone they keep the defaults (the feature's name, no colour, `component` = the one selected in the
  // browser; copies follow the picked bodies). bodyStyle() is the argument of design::style_new_bodies.
  void setBodyDefaults(const std::string& component);
  opad::json bodyStyle() const;
  void setBodyName(const QString& name);  // benches
  void setBodyColour(const QColor& colour);
  // As tall as the rows this feature shows (a fillet's two no longer sat in an extrude-sized panel), and wide enough
  // for a pick box beside "By rule…" to say "3 selected" rather than "3 selec…".
  QSize preferredSize(int width) const;
  ToolGuide* guide() const { return m_guide; }  // UI-107: a new feature's animated guide, at the pick it waits for
  PanelFooter* footer() const { return m_footer; }
 signals:
  void inputsChanged();                 // anything that changes the result
  void activeInputChanged(const QString& input);
  // "By rule…" on a face or edge input holding one picked entity (TODO 10 B7): the controller offers rules.
  void ruleRequested(const QString& input, QWidget* anchor);
  void accepted();
  void cancelled();       // Cancel
  void escapePressed();   // Esc in the panel: one step back, as in the view
  void contentResized();  // rows were added, shown or hidden: the panel fits itself again
 protected:
  void keyPressEvent(QKeyEvent* e) override;
 private:
  void refreshVisibility();
  void refreshNewBody();
  bool makesCopies() const;
 public:
  static bool isPick(const std::string& type);  // an input picked in the view (bodies, faces, edges, profiles, ...)
 private:
  AppDocument* m_doc;
  const opad::design::FeatureSpec* m_spec = nullptr;
  bool m_editingFeature = false;
  QWidget* m_newBody = nullptr;
  QCheckBox* m_newBodyToggle = nullptr;
  QWidget* m_newBodyRows = nullptr;
  QLineEdit* m_bodyName = nullptr;
  QPushButton* m_bodyColour = nullptr;
  QPushButton* m_bodyColourReset = nullptr;
  QComboBox* m_bodyParent = nullptr;
  QColor m_colour;  // invalid: automatic
  class QScrollArea* m_scroll;
  QWidget* m_form;  // everything above the footer, in m_scroll
  QLineEdit* m_name;
  QLabel* m_hint;
  ToolGuide* m_guide;
  QLabel* m_status;
  QLabel* m_hiddenWarning;
  QVBoxLayout* m_rows;
  PanelFooter* m_footer;
  struct Row {
    QWidget* row = nullptr;
    ExprEdit* expr = nullptr;
    QComboBox* combo = nullptr;
    QCheckBox* check = nullptr;
    PickBox* pick = nullptr;
    QPushButton* rule = nullptr;  // "By rule…" (faces and edges)
  };
  std::map<QString, Row> m_widgets;
  opad::json m_values = opad::json::object();  // picks and the values of hidden inputs
  QString m_active;
  std::map<QString, QString> m_notes;  // setPickNote
  int m_guideStep = 0, m_guideCount = 0;  // setGuideStep
};

// Modeless "Change parameters" dialog: name, expression, value, comment; rows are edited in place.
class ParametersDialog : public QWidget {
  Q_OBJECT
 public:
  // `apply(ops, label)` hands design ops to the controller (planned on a worker, committed, reported back
  // through AppDocument::changed or `failed`).
  ParametersDialog(AppDocument* doc, std::function<void(std::vector<opad::json>, QString)> apply, QWidget* parent = nullptr);
  void failed(const QString& error);  // the last change was refused
 public slots:
  void rebuild();
 signals:
  void closeRequested();
 private:
  void addParameter();
  void removeCurrent();
  void itemEdited(QTreeWidgetItem* item, int column);
  bool eventFilter(QObject* watched, QEvent* event) override;  // in a narrow panel, name, expression and value share the width
  AppDocument* m_doc;
  std::function<void(std::vector<opad::json>, QString)> m_apply;
  QTreeWidget* m_table;
  QLabel* m_status;
  bool m_filling = false;
};
