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
#include "opad/design/feature.hpp"

// A line edit for an expression ("width / 2 + 3 mm") with its value, or what is wrong with it, underneath.
class ExprEdit : public QWidget {
  Q_OBJECT
 public:
  ExprEdit(AppDocument* doc, opad::design::Dim dim, QWidget* parent = nullptr);
  void setText(const QString& text);
  QString text() const;
  bool valid() const { return m_valid; }
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
 signals:
  void cleared();
 protected:
  void paintEvent(QPaintEvent*) override;
  void mousePressEvent(QMouseEvent* e) override;
 private:
  int m_count = 0;
  QString m_what;
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
  void setEditHidden(bool hidden);
  void setValue(const QString& input, const opad::json& value);  // expression, choice or flag, as the user would type it
  void activate(const QString& input);  // empty: none
  void activateNextPick();              // the first shown pick input that still needs picks
 signals:
  void inputsChanged();                 // anything that changes the result
  void activeInputChanged(const QString& input);
  void accepted();
  void cancelled();
 protected:
  void keyPressEvent(QKeyEvent* e) override;
 private:
  void refreshVisibility();
  static bool isPick(const std::string& type);
  AppDocument* m_doc;
  const opad::design::FeatureSpec* m_spec = nullptr;
  QLineEdit* m_name;
  QLabel* m_hint;
  QLabel* m_status;
  QLabel* m_hiddenWarning;
  QVBoxLayout* m_rows;
  QPushButton* m_ok;
  struct Row {
    QWidget* row = nullptr;
    ExprEdit* expr = nullptr;
    QComboBox* combo = nullptr;
    QCheckBox* check = nullptr;
    PickBox* pick = nullptr;
  };
  std::map<QString, Row> m_widgets;
  opad::json m_values = opad::json::object();  // picks and the values of hidden inputs
  QString m_active;
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
  AppDocument* m_doc;
  std::function<void(std::vector<opad::json>, QString)> m_apply;
  QTreeWidget* m_table;
  QLabel* m_status;
  bool m_filling = false;
};
