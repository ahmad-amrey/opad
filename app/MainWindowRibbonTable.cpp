// The ribbon as one table (UI-102/104, TODO 11 Appendix A §2-3): the window's workspaces, their tabs and titled groups,
// with the commands of the window and of the feature areas by id. An id this build does not have (an area that is off)
// is left out, a split item whose own command is missing is led by its first variant, and a group or tab left empty is
// not shown. The areas add only what they own as a whole: a workspace of their own (Drawings) and the contextual tabs
// they show and hide (Canvas, Explode). The same action objects sit on every tab that shows them (View and Inspect are
// in Review and Design alike), so a button is enabled, checked and keyed the same everywhere.
//   Review (Ctrl+1)   View · Inspect · Markup · Compare · Share       look, measure, mark up, compare, share
//   Design (Ctrl+2)   [Sketch] Solid · Assemble · Construct · Inspect · Insert · View
//   Drafting (Ctrl+4) Home · Annotate · View · Output                 2D files: layers, measure, plot, to sketch
// Sketch is the contextual tab a sketch opens first in Design, Finish sketch its primary button at the end.
#include "MainWindow.hpp"

#include <functional>
#include <vector>

namespace {
using Size = RibbonLayout::Size;
constexpr Size S = Size::Small;
constexpr Size L = Size::Large;
constexpr Size I = Size::Icon;
struct RibbonTool {
  const char* id;
  Size size = Size::Large;
  std::vector<const char*> variants = {};  // a split button: the arrow drops these down
  bool primary = false;                    // the tab's main verb, in the accent colour (Finish sketch)
};

// A titled group "<tab>.<name>" of those tools; ids the build does not have are left out.
void addGroup(RibbonLayout& layout, const std::function<QAction*(const QString&)>& action, const QString& tab, const char* name, const QString& title,
              const std::vector<RibbonTool>& tools) {
  const QString id = tab + "." + name;
  if (!layout.addGroup(tab, id, title)) return;
  for (const RibbonTool& t : tools) {
    QList<QAction*> variants;
    for (const char* v : t.variants)
      if (QAction* a = action(v)) variants << a;
    QAction* a = action(t.id);
    if (!a && !variants.isEmpty()) a = variants.takeFirst();
    if (a) layout.addAction(id, a, t.size, variants, t.primary);
  }
}
}  // namespace

void MainWindow::ribbonTable(RibbonLayout& layout) {
  const auto find = [this](const QString& id) { return action(id); };
  auto group = [&](const QString& tab, const char* name, const QString& title, const std::vector<RibbonTool>& tools) { addGroup(layout, find, tab, name, title, tools); };
  layout.addWorkspace("review", {tr("Review"), "eye", "Ctrl+1", tr("Look, measure, mark up, compare and share. Nothing here changes geometry or structure."),
                                 tr("ops: annotation · measurement · section · view")});
  layout.addWorkspace("design", {tr("Design"), "component", "Ctrl+2", tr("Model parts: sketches, features, parameters; arrange the assembly."),
                                 tr("ops: param · sketch · feature · edit · regen · import · reparent · appearance")});

  // The tabs both modelling and reviewing show, with the same commands.
  auto viewTab = [&](const QString& tab) {
    group(tab, "navigate", tr("Navigate"),
          {{"view.fit", L, {"view.fitall", "view.zoomWindow", "view.previous", "view.next"}},
           {"view.home", L, {"view.setHome", "view.resetHome"}},
           {"view.iso", S, {"view.top", "view.front", "view.right", "view.bottom", "view.back", "view.left"}},
           {"view.rollleft", S, {"view.rollright", "view.twist", "view.untwist"}},
           {"view.alignPlane", S}});
    group(tab, "display", tr("Display"),
          {{"view.shaded", S}, {"view.edges", S}, {"view.wire", S}, {"view.hidden", S, {"view.hiddenEdges"}}, {"view.ortho", S}, {"view.2d", S},
           {"view.grid", S, {"view.gridSettings"}}});
    group(tab, "visibility", tr("Visibility"),
          {{"view.isolate"}, {"view.unisolate", S}, {"view.hideothers", S}, {"edit.hide", S}, {"edit.showall", S}, {"view.hideSmallParts", S, {"view.smallPartSize"}},
           {"assembly.explode"}});
    group(tab, "layers", tr("Layers"), {{"drawing2d.layers"}, {"drawing2d.layerWalk", S}, {"drawing2d.isolateLayer", S}});
  };
  auto inspectTab = [&](const QString& tab) {
    group(tab, "measure", tr("Measure"),
          {{"inspect.distance"}, {"inspect.angle"}, {"inspect.radius"}, {"inspect.bbox", S}, {"inspect.length", S}, {"inspect.area", S}, {"inspect.pin"}});
    group(tab, "section", tr("Section"), {{"inspect.section"}, {"inspect.flip", S}, {"panel.section", S}});
    group(tab, "check", tr("Check"), {{"inspect.interference"}, {"inspect.printcheck"}, {"kicad.clearance", S}});
    group(tab, "properties", tr("Properties"), {{"inspect.properties"}, {"inspect.partProperties", S}, {"inspect.material", S}, {"select.similar", S}});
  };

  // ---------------------------------------------------------------- Review
  layout.addTab("review", "review.view", tr("View"));
  viewTab("review.view");
  layout.addTab("review", "review.inspect", tr("Inspect"));
  inspectTab("review.inspect");
  layout.addTab("review", "review.markup", tr("Markup"));
  group("review.markup", "notes", tr("Notes"), {{"annotate.add"}, {"annotate.draw"}, {"annotate.resolve", S}, {"annotate.show", S}, {"panel.annotations", S}});
  group("review.markup", "views", tr("Saved views"), {{"view.saveview"}, {"assembly.explodeSave", S}});
  layout.addTab("review", "review.compare", tr("Compare"));
  group("review.compare", "compare", tr("Compare"),
        {{"vcs.compare", L, {"vcs.unsavedChanges", "vcs.compareFile"}}, {"vcs.previousChange", S}, {"vcs.nextChange", S}});
  group("review.compare", "versions", tr("Versions"),
        {{"vcs.panel"}, {"vcs.history", S}, {"vcs.commit", S}, {"vcs.branches", S}, {"vcs.newBranch", S}, {"file.recover", S}});
  group("review.compare", "remote", tr("Remote"), {{"vcs.pull", S}, {"vcs.push", S}, {"vcs.fetch", S}});
  layout.addTab("review", "review.share", tr("Share"));
  group("review.share", "export", tr("Export"), {{"file.export"}, {"file.exportBom", S}, {"file.screenshot"}, {"drawing2d.plot"}});
  group("review.share", "file", tr("File"), {{"file.importdoc"}, {"file.reveal", S}, {"file.copyPath", S}, {"file.documentProperties", S}});

  // ---------------------------------------------------------------- Design
  layout.addTab("design", "design.solid", tr("Solid"));
  group("design.solid", "sketch", tr("Sketch"), {{"design.sketch"}});
  group("design.solid", "create", tr("Create"),
        {{"design.extrude"}, {"design.revolve"}, {"design.hole"}, {"design.sweep", S}, {"design.loft", S}, {"design.pipe", S}, {"design.coil", S},
         {"design.thicken", S}, {"design.box", S, {"design.cylinder", "design.sphere", "design.cone", "design.torus"}}});
  group("design.solid", "modify", tr("Modify"),
        {{"design.offset_face", L, {"design.remove_faces"}}, {"design.fillet"}, {"design.chamfer", S}, {"design.shell", S}, {"design.draft", S}, {"design.scale", S},
         {"design.move", S}});
  group("design.solid", "combine", tr("Combine"), {{"design.combine", S}, {"design.split", S}, {"design.remove", S}});
  group("design.solid", "pattern", tr("Pattern"), {{"design.mirror", S}, {"design.pattern_rect", S}, {"design.pattern_circ", S}});
  group("design.solid", "history", tr("History"), {{"design.parameters", S}, {"design.edit", S}, {"design.regenerate", S}});
  layout.addTab("design", "design.assemble", tr("Assemble"));
  group("design.assemble", "components", tr("Components"),
        {{"design.newcomponent", L, {"design.componentFromSelection"}}, {"assembly.activate"}, {"assembly.activateRoot", S}, {"assembly.activeVisibility", S},
         {"design.reparent", S}});
  group("design.assemble", "explode", tr("Explode"), {{"assembly.explode"}, {"assembly.explodePlay", S}, {"assembly.explodeOff", S}});
  group("design.assemble", "appearance", tr("Appearance"),
        {{"design.colour"}, {"inspect.material", S}, {"design.opacity", S}, {"design.lock", S}, {"edit.hide", S}, {"view.isolate", S}});
  group("design.assemble", "organise", tr("Organise"),
        {{"edit.rename", S}, {"edit.delete", S}, {"edit.restore", S}, {"edit.copy", S}, {"edit.paste", S}, {"edit.pastelinked", S}});
  layout.addTab("design", "design.construct", tr("Construct"));
  group("design.construct", "planes", tr("Planes and axes"), {{"design.plane"}, {"design.axis"}});
  layout.addTab("design", "design.inspect", tr("Inspect"));
  inspectTab("design.inspect");
  layout.addTab("design", "design.insert", tr("Insert"));
  group("design.insert", "files", tr("Insert"), {{"file.import"}, {"assets.link"}, {"kicad.insert"}, {"canvas.insert"}});
  group("design.insert", "linked", tr("Linked files"), {{"assets.syncAll"}, {"assets.autoSync", S}, {"assets.pack", S}, {"assets.settings", S}});
  layout.addTab("design", "design.view", tr("View"));
  viewTab("design.view");

  // ---------------------------------------------------------------- Sketch (contextual, first in Design while sketching)
  layout.addContextualTab("design", "design.sketch", tr("Sketch"));
  group("design.sketch", "create", tr("Create"),
        {{"sketch.line"},
         {"sketch.rect", L, {"sketch.crect", "sketch.rect3"}},
         {"sketch.circle", L, {"sketch.circle2", "sketch.circle3", "sketch.tangent_circle"}},
         {"sketch.arc3", S, {"sketch.arcc", "sketch.tangent_arc"}},
         {"sketch.polygon", S, {"sketch.polygon_outer"}},
         {"sketch.slot", S, {"sketch.cslot", "sketch.arcslot"}},
         {"sketch.spline", S, {"sketch.control_spline"}},
         {"sketch.ellipse", S, {"sketch.conic"}},
         {"sketch.point", S},
         {"sketch.text", S}});
  group("design.sketch", "modify", tr("Modify"),
        {{"sketch.trim", L, {"sketch.extend", "sketch.split", "sketch.break"}},
         {"sketch.offset", S},
         {"sketch.fillet", S, {"sketch.chamfer"}},
         {"sketch.mirror", S},
         {"sketch.move", S, {"sketch.rotate", "sketch.scale", "sketch.copy"}},
         {"sketch.rect_pattern", S, {"sketch.polar_pattern"}},
         {"sketch.union", S, {"sketch.subtract", "sketch.intersect"}},
         {"sketch.heal", S, {"sketch.explode", "sketch.simplify"}},
         {"sketch.construction", S}});
  group("design.sketch", "constrain", tr("Constrain"),
        {{"sketch.dimension"},
         {"sketch.c.horizontal", I},
         {"sketch.c.vertical", I},
         {"sketch.c.coincident", I, {"sketch.c.collinear", "sketch.c.concentric", "sketch.c.midpoint"}},
         {"sketch.c.parallel", I},
         {"sketch.c.perpendicular", I},
         {"sketch.c.tangent", I, {"sketch.c.smooth", "sketch.c.curvature"}},
         {"sketch.c.equal", I, {"sketch.c.symmetric"}},
         {"sketch.c.fix", I},
         {"sketch.constraints", I}});
  group("design.sketch", "reference", tr("Reference"),
        {{"sketch.project", L, {"sketch.intersect_body", "sketch.silhouette", "sketch.include3d", "sketch.break_link"}}, {"kicad.project", S}});
  group("design.sketch", "insert", tr("Insert"), {{"sketch.moreFiles"}});
  group("design.sketch", "options", tr("Options"),
        {{"sketch.snaps", S},
         {"view.grid", S, {"view.gridSettings"}},
         {"view.2d", S, {"view.alignPlane"}},
         {"sketch.showConstraints", S},
         {"sketch.openEnds", S},
         {"sketch.panel", S}});
  group("design.sketch", "finish", tr("Finish"), {{"sketch.cancel", S}, {"sketch.finish", L, {}, true}});
}

// Drafting (UI-104, Appendix A "Draft 2D", phase 1 with the tools there are): after the areas' workspaces in the switcher.
// A drawing file viewed (DXF, DWG, SVG) comes here by itself (MainWindow::followDrawing).
void MainWindow::draftingTable(RibbonLayout& layout) {
  const auto find = [this](const QString& id) { return action(id); };
  auto group = [&](const QString& tab, const char* name, const QString& title, const std::vector<RibbonTool>& tools) { addGroup(layout, find, tab, name, title, tools); };
  layout.addWorkspace("drafting", {tr("Drafting"), "drawing", "Ctrl+4",
                                   tr("2D drawings (DXF, DWG, SVG): layers, measuring, plotting, and a drawing turned into a sketch to edit. Viewing a drawing comes here."),
                                   tr("ops: annotation · measurement · view")});
  layout.addTab("drafting", "drafting.home", tr("Home"));
  group("drafting.home", "layers", tr("Layers"), {{"drawing2d.layers"}, {"drawing2d.layerWalk", S}, {"drawing2d.isolateLayer", S}});
  group("drafting.home", "measure", tr("Measure"),
        {{"inspect.distance"}, {"inspect.area"}, {"inspect.angle", S}, {"inspect.radius", S}, {"inspect.length", S}, {"inspect.bbox", S}, {"inspect.pin"}});
  group("drafting.home", "snap", tr("Snap"), {{"drawing2d.objectSnap", S}, {"view.gridSnap", S}, {"view.orthoSnap", S}, {"view.polarSnap", S}});
  group("drafting.home", "edit", tr("Edit"), {{"design.convertDrawing"}});
  layout.addTab("drafting", "drafting.annotate", tr("Annotate"));
  group("drafting.annotate", "notes", tr("Notes"), {{"annotate.add"}, {"annotate.draw"}, {"annotate.resolve", S}, {"annotate.show", S}, {"panel.annotations", S}});
  group("drafting.annotate", "views", tr("Saved views"), {{"view.saveview"}});
  layout.addTab("drafting", "drafting.view", tr("View"));
  group("drafting.view", "navigate", tr("Navigate"),
        {{"view.fit", L, {"view.fitall", "view.zoomWindow", "view.previous", "view.next"}}, {"view.home", L, {"view.setHome", "view.resetHome"}}, {"view.rollleft", S, {"view.rollright"}}});
  group("drafting.view", "display", tr("Display"), {{"view.2d"}, {"view.grid", S, {"view.gridSettings"}}});
  group("drafting.view", "visibility", tr("Visibility"), {{"view.isolate"}, {"view.unisolate", S}, {"view.hideothers", S}, {"edit.hide", S}, {"edit.showall", S}});
  layout.addTab("drafting", "drafting.output", tr("Output"));
  group("drafting.output", "output", tr("Output"), {{"drawing2d.plot"}, {"file.export"}, {"file.screenshot"}});
  group("drafting.output", "file", tr("File"), {{"file.importdoc"}, {"file.reveal", S}, {"file.copyPath", S}});
}
