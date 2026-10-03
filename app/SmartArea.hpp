#pragma once
// Smart selection (TODO 11 UI-97), a feature area (AreaController.hpp). Select similar selects the faces or edges like
// the picked one by rule (holes of its size, fillets of its radius, faces facing its way ...), or a body's edges and
// faces by rule (top perimeter, edges along X ...), found on a worker (opad::Recognizer); asked again on what it
// selected, the next rule. It replaces the modal Select by geometry: the built-in Inspect menu and Select ▾ name it in
// that one's place, the area adds the rest (Edit menu, Inspect > Results, the context menu). Remove faces (a feature kind,
// design.remove_faces) is placed under Offset face's arrow and in the context menu of picked faces.
#include <string>
#include <vector>

#include "AreaController.hpp"
#include "opad/recognize.hpp"

class SmartArea : public AreaController {
 public:
  using AreaController::AreaController;
  struct Similar {
    std::vector<opad::Recognized> rules;
    std::string body;
    size_t current = 0;
    std::vector<opad::Ref> selected;
    unsigned long long revision = 0, generation = 0;
    unsigned token = 0;  // the last apply: one waiting for its selection filter is dropped by the next
  };
  const Similar& similar() const { return m_similar; }  // the rules found last and the one shown (benches)

  void buildActions() override;
  void menus(QMenuBar* bar, const QMap<QString, QMenu*>& menus) override;
  void ribbon(RibbonLayout& layout) override;
  void contextMenu(const SelectionContext& selection, QMenu& menu) override;

 private:
  void selectSimilar();
  void apply(size_t rule);
  QAction* m_similarAction = nullptr;
  Similar m_similar;
};
