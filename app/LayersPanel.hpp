#pragma once
// The Layers manager of 2D drawings (UI-89, area drawing2d): one row per layer of the drawings in the document, with On,
// Freeze, Lock, Colour, Linetype, Lineweight and Plot (a click on a cell changes it, one step to undo; viewer mode too),
// isolate, a layer walk and layer states (saved in view ops). The layer model is Drawing2D.hpp.
#include <QStringList>
#include <QWidget>

#include <string>
#include <utility>
#include <vector>

#include "Drawing2D.hpp"

class AreaServices;
class PanelFooter;
class QComboBox;
class QLabel;
class QLineEdit;
class QToolButton;
class QTreeWidget;
class QTreeWidgetItem;

class LayersPanel : public QWidget {
  Q_OBJECT
 public:
  enum Column { Name, On, Freeze, Lock, Colour, Linetype, Lineweight, Plot, Columns };
  explicit LayersPanel(AreaServices& services, QWidget* parent = nullptr);
  void rebuild();  // from the document; keeps the selected rows and the walk
  const std::vector<drawing2d::Layer>& layers() const { return m_layers; }
  QTreeWidget* tree() const { return m_tree; }
  QTreeWidgetItem* item(const std::string& layer) const;
  void selectLayer(const std::string& layer);
  std::vector<std::string> selectedLayers() const;
  PanelFooter* footer() const { return m_footer; }
  // A lineweight is a pen's width on paper: in millimetres whatever the document's unit ("Default" below 0).
  static QString weightText(double mm);

  // Changes, each one step to undo; a cell's click and its menus do these.
  void toggle(const std::string& layer, int column);  // On, Freeze, Lock, Plot
  void setColor(const std::string& layer, const drawing2d::Rgb& color);
  void setDrawingColor(const std::string& layer);  // back to the drawing's own colours (colour 7: the background's ink)
  void setLinetype(const std::string& layer, const std::string& linetype);
  void setLineweight(const std::string& layer, double mm);
  void allOn();  // every layer on and thawed
  void isolate(const std::vector<std::string>& layers);  // only these shown (view only: Shift+I or Show all ends it)

  // Layer walk (LAYWALK): one layer shown at a time, the camera kept; steps through the rows as listed. Stopping (or any
  // other end of the isolation) shows every layer again.
  void startWalk();
  void walk(int step);
  void stopWalk();
  bool walking() const { return !m_walked.empty(); }
  const std::string& walked() const { return m_walked; }
  QString walkText() const;

  // Layer states: every layer's settings in a view op (display.layers); restoring is one step.
  void saveState(const QString& name);  // asks to save a viewed file first (a view op edits the document)
  void restoreState(const std::string& viewId);
  std::vector<std::pair<std::string, QString>> states() const;  // view ids and names, in document order

 signals:
  void walkChanged(const QString& text);  // empty when the walk stops

 private:
  void apply(std::vector<std::pair<std::string, opad::json>> commands, const QString& label);
  void cellClicked(QTreeWidgetItem* item, int column);
  void filter();
  void showWalk();
  void retheme();  // the header's and the buttons' icons in the theme's colours (the rows are rebuilt with the document)
  const drawing2d::Layer* layer(const std::string& id) const { return drawing2d::find(m_layers, id); }
  AreaServices& m_services;
  std::vector<drawing2d::Layer> m_layers;
  std::string m_walked;
  QLineEdit* m_filter;
  QTreeWidget* m_tree;
  QWidget* m_walkBar;
  QLabel* m_walkLabel;
  QComboBox* m_states;
  QToolButton *m_restore, *m_delete;
  PanelFooter* m_footer;
  std::vector<std::pair<QToolButton*, QString>> m_icons;  // the tool buttons and their icon names
};
