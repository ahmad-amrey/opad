// Select other (UI-128) in the view's context menu: a right click lists what lies under it, as Alt+click does (the way to
// it without a modifier, and where it is found).
#include <QAction>
#include <QMenu>

#include "AreaController.hpp"
#include "Viewport.hpp"

class SelectOther : public AreaController {
 public:
  using AreaController::AreaController;

  void contextMenu(const SelectionContext& selection, QMenu& menu) override {
    Viewport* v = services().viewport();
    QPointF at;
    if (selection.sketching || !v || !v->contextMenuPoint(at) || v->pickCandidates(at).size() < 2) return;
    menu.addSeparator();
    QAction* a = menu.addAction(tr("Select other…"));
    a->setObjectName("select.other");
    a->setToolTip(tr("Everything under the pointer, nearest first (Alt+click; Tab hovers the next)"));
    const QPoint global = v->mapToGlobal(at.toPoint());
    connect(a, &QAction::triggered, v, [v, at, global] {
      if (QMenu* list = v->selectOtherMenu(at)) list->popup(global);
    });
  }
};
OPAD_AREA(SelectOther)
