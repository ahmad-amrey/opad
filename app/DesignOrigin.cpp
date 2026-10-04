// The origin of an empty design document (UI-51): with nothing modelled yet, the Design workspace shows the origin's axes,
// its XY, XZ and YZ planes (pickable: a plane picked first is where New sketch draws) and the grid; they go once there is a
// body or a sketch, while a sketch or a feature is edited, and in the other workspaces.
#include <QAction>
#include <QKeySequence>

#include "AreaController.hpp"
#include "AppDocument.hpp"
#include "DesignController.hpp"
#include "Viewport.hpp"

class DesignOrigin : public AreaController {
 public:
  using AreaController::AreaController;

  void ready() override {
    connect(services().design(), &DesignController::stateChanged, this, [this] { update(); });
    connect(services().viewport(), &Viewport::selectionChanged, this, [this] { planePicked(); });
    update();
  }
  void documentChanged(bool) override { update(); }
  void workspaceChanged(const QString&) override { update(); }

 private:
  void update() {
    const AppDocument* doc = services().document();
    const DesignController* design = services().design();
    if (!doc || !design || !services().viewport()) return;
    const bool empty = doc->hasDocument && !doc->browse && doc->scene.all_bodies().empty() && doc->scene.sketches.empty();
    services().viewport()->setOriginGuide(empty && services().workspace() == "design" && !design->sketchActive() && !design->featureActive());
  }
  // A plane of the origin picked by itself: said where it leads.
  void planePicked() {
    Viewport* v = services().viewport();
    const auto picked = v->selectedCandidates();
    if (!v->originGuide() || picked.size() != 1 || services().design()->pickingPlane()) return;
    const opad::json id = opad::json::parse(picked.front(), nullptr, false);
    if (!id.is_object() || !id.contains("base")) return;
    services().showMessage(tr("%1 plane selected: New sketch draws on it").arg(QString::fromStdString(id["base"].get<std::string>()).toUpper()), 6000);
  }
};
OPAD_AREA(DesignOrigin)
