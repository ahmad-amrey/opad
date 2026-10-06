// OPAD_BENCH_HIGHLIGHT_KEYS: the highlight switches (X-ray highlight, Ctrl+/; Hover highlight, /) and the cheat sheet's
// key (?). On three boxes one behind the other in the right view, through the window's commands: X-ray on (the default)
// draws the selected back box, its glow and its selected faces in Topmost, over the front box (the frame shows it); off,
// in Top, where the front box hides them, and the hover styles go to Top too; a reference tool's override puts the
// selection in Top for a while and lets go of it back to the user's choice, never clobbering it. Hover off: the pointer
// over the front box still detects it (its name, its hover point, a click selects it in the selection colour), but
// nothing is drawn hovered and the hover fade sleeps; on again, the hover comes back where the pointer rests. Then the
// keys as the window's shortcuts take them: / and Ctrl+/ over the view toggle the commands, ? opens the cheat sheet and
// closes it from its empty search field (typed there once something is), and / and ? in a text field of the window, in
// the command search and in a sketch value being typed are text. <prefix>.xray-on.png, .xray-off.png, .hover-on.png,
// .hover-off.png.
#include <QApplication>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QImage>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMouseEvent>
#include <QScopedValueRollback>
#include <QTimer>

#include <AIS_AnimationCamera.hxx>
#include <Bnd_Box.hxx>

#include <algorithm>
#include <cmath>
#include <functional>
#include <memory>

#include "BenchRegistry.hpp"
#include "BodyShape.hpp"
#include "CommandPalette.hpp"
#include "DesignController.hpp"
#include "HelpWindows.hpp"
#include "Jobs.hpp"
#include "KeyText.hpp"
#include "MainWindow.hpp"
#include "SketchEditor.hpp"
#include "Viewport.hpp"
#include "opad/design/sketch.hpp"

namespace {
// Runs `run` once every body of the document is displayed (or after a minute, reported), then quits with its outcome.
void whenShown(QObject* context, Viewport* v, std::function<bool()> run) {
  auto clock = std::make_shared<QElapsedTimer>();
  clock->start();
  auto* timer = new QTimer(context);
  QObject::connect(timer, &QTimer::timeout, context, [v, timer, clock, run] {
    const bool ready = !v->pumpJob() && v->remainingBodies() == 0 && v->displayedCount() > 0;
    if (!ready && clock->elapsed() < 60000) return;
    timer->stop();
    timer->deleteLater();
    if (!ready) trace::log("bench: highlight keys: the model is displayed FAIL");
    QCoreApplication::exit(ready && run() ? 0 : 2);
  });
  timer->start(50);
}

int gap(const QColor& a, const QColor& b) {
  return std::max({std::abs(a.red() - b.red()), std::abs(a.green() - b.green()), std::abs(a.blue() - b.blue())});
}
QString rgb(const QColor& c) { return QString("%1,%2,%3").arg(c.red()).arg(c.green()).arg(c.blue()); }

// A widget point's pixel in a frame grabbed at device resolution.
QColor pixel(const QImage& frame, const QWidget* view, const QPointF& at) {
  const int x = qRound(at.x() * frame.width() / view->width()), y = qRound(at.y() * frame.height() / view->height());
  return frame.rect().contains(x, y) ? frame.pixelColor(x, y) : QColor();
}
}  // namespace

bool Viewport::benchHighlightSwitches(const QString& prefix, const std::function<void(const QString&)>& trigger) {
  bool all = true;
  auto require = [&](bool ok, const QString& what) {
    trace::log(QString("bench: highlight keys: %1 %2").arg(what, ok ? "PASS" : "FAIL"));
    all = all && ok;
    return ok;
  };
  if (!require(m_initialised && m_items.size() >= 2, "two bodies or more are displayed")) return false;
  // The front body and the back one, looking along -X (the right view): the largest and the smallest X.
  std::string front, back;
  double most = -1e300, least = 1e300;
  for (const auto& [id, item] : m_items) {
    Bnd_Box b;
    item.ais->BoundingBox(b);
    if (b.IsVoid()) continue;
    const double x = (b.CornerMin().X() + b.CornerMax().X()) / 2;
    if (x > most) most = x, front = id;
    if (x < least) least = x, back = id;
  }
  if (!require(!front.empty() && front != back, "a front and a back body along X")) return false;
  const Handle(AIS_Shape) frontAis = m_items.at(front).ais, backAis = m_items.at(back).ais;
  Bnd_Box fb;
  frontAis->BoundingBox(fb);
  const gp_Pnt lo = fb.CornerMin(), hi = fb.CornerMax();
  // The filter at once (setSelectionFilter activates the bodies in a sliced job).
  auto filter = [this](SelFilter f) {
    m_filter = f;
    for (auto& [id, item] : m_items) activateSelection(item.ais);
  };
  myViewAnimation->Stop();
  m_ctx->ClearSelected(Standard_False);
  applySelectionLayers();
  refreshSubHighlight();
  filter(SelFilter::Body);
  standardView("right");
  fitAll();
  m_view->Redraw();
  const QPointF centre = QPointF(widgetPoint({hi.X(), (lo.Y() + hi.Y()) / 2, (lo.Z() + hi.Z()) / 2}));
  const QPointF away(4, height() - 4);
  auto frame = [this] {
    m_view->Redraw();
    m_view->RedrawImmediate();
    return grabImage();
  };
  // The pointer as the mouse moves it: the handler, then the frame the controller draws (where the hover is decided).
  auto pointer = [this](const QPointF& at) {
    QMouseEvent move(QEvent::MouseMove, at, mapToGlobal(at), Qt::NoButton, Qt::NoButton, Qt::NoModifier);
    QCoreApplication::sendEvent(this, &move);
    paintEvent(nullptr);
  };
  auto layerOf = [this](Prs3d_TypeOfHighlight kind) { return m_ctx->HighlightStyle(kind)->ZLayer(); };
  auto styles = [&](Graphic3d_ZLayerId layer) {
    return layerOf(Prs3d_TypeOfHighlight_Selected) == layer && layerOf(Prs3d_TypeOfHighlight_LocalSelected) == layer &&
           layerOf(Prs3d_TypeOfHighlight_Dynamic) == layer && layerOf(Prs3d_TypeOfHighlight_LocalDynamic) == layer;
  };
  auto subLayer = [this] { return m_subHl.IsNull() ? Graphic3d_ZLayerId_UNKNOWN : m_subHl->ZLayer(); };
  auto glowLayer = [this](const Handle(AIS_Shape)& ais) {
    const auto glow = m_bodyGlows.find(ais.get());
    return glow == m_bodyGlows.end() ? Graphic3d_ZLayerId_UNKNOWN : glow->second->ZLayer();
  };
  pointer(away);
  const QColor plain = pixel(frame(), this, centre);

  // (1) The defaults: both on, every highlight style in Topmost.
  require(xrayHighlight() && hoverHighlight() && selectionXray() && styles(Graphic3d_ZLayerId_Topmost), "by default X-ray and hover highlight are on, the styles in Topmost");

  // (2) The back box selected shows through the front one; X-ray off, the front one hides it.
  opad::Ref backRef;
  backRef.body = back;
  selectRefs({backRef});
  require(m_ctx->IsSelected(backAis) && backAis->ZLayer() == Graphic3d_ZLayerId_Topmost && glowLayer(backAis) == Graphic3d_ZLayerId_Topmost,
          "the selected back box and its glow are in Topmost");
  QImage shot = frame();
  shot.save(prefix + ".xray-on.png");
  const QColor through = pixel(shot, this, centre);
  require(gap(through, plain) > 20, QString("it shows through the front box: %1 over %2").arg(rgb(through), rgb(plain)));
  trigger("view.xrayHighlight");
  require(!xrayHighlight() && !selectionXray() && QSettings().value("view/xrayHighlight", true).toBool() == false, "the command turns X-ray off (saved)");
  require(backAis->ZLayer() == Graphic3d_ZLayerId_Top && glowLayer(backAis) == Graphic3d_ZLayerId_Top && styles(Graphic3d_ZLayerId_Top),
          "X-ray off: the selected body, its glow and the highlight styles (hover too) in Top, depth-tested");
  shot = frame();
  shot.save(prefix + ".xray-off.png");
  const QColor hidden = pixel(shot, this, centre);
  require(gap(hidden, plain) <= 8, QString("the front box hides the selection: %1, plain %2").arg(rgb(hidden), rgb(plain)));

  // (3) Faces of the back box selected with X-ray off: their one object in Top, hidden; on again: Topmost, seen.
  m_ctx->ClearSelected(Standard_False);
  applySelectionLayers();
  filter(SelFilter::Face);
  std::vector<opad::Ref> faces;
  for (int i = 1; i <= 6; ++i) {
    opad::Ref f;
    f.body = back;
    f.kind = opad::Ref::Kind::Face;
    f.index = i;
    faces.push_back(f);
  }
  selectRefs(faces);
  require(subLayer() == Graphic3d_ZLayerId_Top && gap(pixel(frame(), this, centre), plain) <= 8,
          "X-ray off: the selected faces are drawn in Top, hidden by the front box");
  trigger("view.xrayHighlight");
  const QColor faceThrough = pixel(frame(), this, centre);
  require(xrayHighlight() && subLayer() == Graphic3d_ZLayerId_Topmost && styles(Graphic3d_ZLayerId_Topmost) && gap(faceThrough, plain) > 20,
          QString("X-ray on again: the selected faces move to Topmost and show through (%1), the styles back in Topmost").arg(rgb(faceThrough)));

  // (4) A reference tool's override (SketchReference) composes with the setting: for its time the selection is in Top,
  // then the user's choice is back, whichever it is.
  suppressSelectionXray(true);
  selectRefs(faces);
  require(!selectionXray() && xrayHighlight() && subLayer() == Graphic3d_ZLayerId_Top, "the override puts the selection in Top, the setting stays on");
  suppressSelectionXray(false);
  selectRefs(faces);
  require(selectionXray() && subLayer() == Graphic3d_ZLayerId_Topmost, "let go, the selection is X-ray again");
  trigger("view.xrayHighlight");
  suppressSelectionXray(true);
  suppressSelectionXray(false);
  selectRefs(faces);
  require(!xrayHighlight() && !selectionXray() && subLayer() == Graphic3d_ZLayerId_Top, "with X-ray off by the user, the override's end leaves it off");
  trigger("view.xrayHighlight");
  require(xrayHighlight(), "X-ray on again");
  m_ctx->ClearSelected(Standard_False);
  applySelectionLayers();
  refreshSubHighlight();
  filter(SelFilter::Body);

  // (5) Hover on: the front box under the pointer is drawn hovered, X-ray on and off (on its own face, depth-tested).
  int points = 0;
  bool onGeometry = false;
  QString label;
  const auto pointConnection = connect(this, &Viewport::hoverPoint, this, [&](bool valid, const opad::Vec3&) { ++points; onGeometry = valid; });
  const auto labelConnection = connect(this, &Viewport::hoverChanged, this, [&](const QString& text) { label = text; });
  pointer(away);
  pointer(centre);
  shot = frame();
  shot.save(prefix + ".hover-on.png");
  const QColor hovered = pixel(shot, this, centre);
  require(m_ctx->HasDetected() && m_ctx->DetectedInteractive() == frontAis && hoverDrawn() && gap(hovered, plain) > 15,
          QString("hover on: the front box lights up under the pointer (%1 over %2)").arg(rgb(hovered), rgb(plain)));
  trigger("view.xrayHighlight");
  pointer(away);
  pointer(centre);
  shot = frame();
  shot.save(prefix + ".hover-depth.png");
  const QColor hoveredDepth = pixel(shot, this, centre);
  require(!xrayHighlight() && m_ctx->DetectedInteractive() == frontAis && gap(hoveredDepth, plain) > 15,
          QString("X-ray off, the hover on the front face still shows (depth-tested on its own face): %1").arg(rgb(hoveredDepth)));
  trigger("view.xrayHighlight");

  // (6) Hover off by the command: detected, labelled, a hover point, clickable, but nothing drawn and no fade running.
  trigger("view.hoverHighlight");
  require(!hoverHighlight() && QSettings().value("view/hoverHighlight", true).toBool() == false, "the command turns the hover highlight off (saved)");
  pointer(away);
  points = 0;
  label.clear();
  pointer(centre);
  shot = frame();
  shot.save(prefix + ".hover-off.png");
  const QColor quiet = pixel(shot, this, centre);
  require(m_ctx->HasDetected() && m_ctx->DetectedInteractive() == frontAis && !hoverDrawn() && gap(quiet, plain) <= 8,
          QString("hover off: the front box is detected under the pointer but not drawn hovered (%1, plain %2)").arg(rgb(quiet), rgb(plain)));
  require(points > 0 && onGeometry && !label.isEmpty(), "the hover is still tracked: its hover point and its name (" + label + ")");
  require(!m_hoverFadeTimer.isActive(), "the hover fade does not run while the hover highlight is off");
  pointer(QPointF(centre.x() + 2, centre.y() + 1));  // moving on the same body: still nothing drawn
  require(!hoverDrawn() && gap(pixel(frame(), this, centre), plain) <= 8, "moving over the same body draws nothing either");
  benchClickAt(centre);
  const QColor picked = pixel(frame(), this, centre);
  require(m_ctx->IsSelected(frontAis) && m_bodyGlows.count(frontAis.get()) && gap(picked, plain) > 20,
          QString("a click still selects it, drawn in the selection colour (%1)").arg(rgb(picked)));
  m_ctx->ClearSelected(Standard_False);
  applySelectionLayers();
  pointer(away);
  pointer(centre);

  // (7) On again: the hover is drawn where the pointer rests, without moving it.
  trigger("view.hoverHighlight");
  paintEvent(nullptr);
  const QColor back2 = pixel(frame(), this, centre);
  require(hoverHighlight() && hoverDrawn() && gap(back2, plain) > 15, QString("hover on again: drawn where the pointer rests (%1)").arg(rgb(back2)));
  disconnect(pointConnection);
  disconnect(labelConnection);
  pointer(away);
  return all;
}

// The viewport's part, then the keys through the window's shortcut path.
OPAD_BENCH(OPAD_BENCH_HIGHLIGHT_KEYS, highlightKeys) {
  Viewport* v = w.m_viewport;
  whenShown(&w, v, [&w, v, value] {
    bool ok = v->benchHighlightSwitches(value, [&w](const QString& id) { w.action(id)->trigger(); });
    auto require = [&ok](bool pass, const QString& what) {
      trace::log(QString("bench: highlight keys: %1 %2").arg(what, pass ? "PASS" : "FAIL"));
      ok = ok && pass;
      return pass;
    };
    QAction* hover = w.action("view.hoverHighlight");
    QAction* xray = w.action("view.xrayHighlight");
    QAction* sheetCommand = w.action("help.shortcuts");
    // A key press as a window delivers it (QApplication sends the shortcut override first, then the shortcut map).
    auto press = [](QWidget* to, int key, Qt::KeyboardModifiers modifiers, const QString& text) {
      QKeyEvent down(QEvent::KeyPress, key, modifiers, text);
      QApplication::sendEvent(to, &down);
      QKeyEvent up(QEvent::KeyRelease, key, modifiers, text);
      QApplication::sendEvent(to, &up);
    };
    require(hover->shortcut() == QKeySequence("/") && xray->shortcut() == QKeySequence("Ctrl+/") && sheetCommand->shortcut() == QKeySequence("?"),
            "the default keys: / Hover highlight, Ctrl+/ X-ray highlight, ? the cheat sheet");
    require(keys::text("help.shortcuts").contains("?") && keys::text("view.hoverHighlight").contains("/") && hover->toolTip().contains("(/)") &&
                xray->toolTip().contains("Ctrl+/"),
            "keys and tooltips show them (" + hover->toolTip() + " | " + xray->toolTip() + ")");
    QApplication::setActiveWindow(&w);
    v->setFocus();
    const bool hoverOn = hover->isChecked(), xrayOn = xray->isChecked();
    press(v, Qt::Key_Slash, Qt::NoModifier, "/");
    require(hover->isChecked() != hoverOn && v->hoverHighlight() == hover->isChecked(), "/ over the view toggles the hover highlight");
    press(v, Qt::Key_Slash, Qt::NoModifier, "/");
    require(hover->isChecked() == hoverOn && v->hoverHighlight() == hoverOn, "/ again turns it back");
    press(v, Qt::Key_Slash, Qt::ControlModifier, QString());
    require(xray->isChecked() != xrayOn && v->xrayHighlight() == xray->isChecked() && hover->isChecked() == hoverOn, "Ctrl+/ toggles X-ray highlight (and not the hover)");
    press(v, Qt::Key_Slash, Qt::ControlModifier, QString());
    require(xray->isChecked() == xrayOn && v->xrayHighlight() == xrayOn, "Ctrl+/ again turns it back");
    // '?' as Qt's key map gives it for Shift+/ (the shortcut map takes a real Shift+/ for "?"; a synthesised event has no
    // scan code to map, so the bench sends the symbol itself).
    press(v, Qt::Key_Question, Qt::NoModifier, "?");
    auto* sheet = w.findChild<ShortcutSheet*>();
    require(sheet && sheet->isVisible() && sheet->closeKey() == QKeySequence("?"), "? opens the shortcuts cheat sheet, which closes on ?");
    if (sheet) {
      QApplication::setActiveWindow(sheet);
      auto* search = sheet->findChild<QLineEdit*>("paletteInput");
      search->setFocus();
      press(search, Qt::Key_Question, Qt::NoModifier, "?");
      require(!sheet->isVisible() && search->text().isEmpty(), "? in its empty search field closes the sheet");
      sheetCommand->trigger();
      QApplication::setActiveWindow(sheet);
      search->setFocus();
      search->setText("fit");
      press(search, Qt::Key_Question, Qt::NoModifier, "?");
      require(sheet->isVisible() && search->text() == "fit?", "with something typed, ? goes into the search");
      search->clear();
      sheet->close();
    }
    // A text field of the window: / and ? are text, the commands stay as they were.
    QApplication::setActiveWindow(&w);
    auto* field = new QLineEdit(&w);
    field->setObjectName("benchField");
    field->show();
    field->setFocus();
    press(field, Qt::Key_Slash, Qt::NoModifier, "/");
    press(field, Qt::Key_Question, Qt::NoModifier, "?");
    require(field->text() == "/?" && hover->isChecked() == hoverOn && !(sheet && sheet->isVisible()), "in a text field / and ? are typed, not the shortcuts (" + field->text() + ")");
    delete field;
    // The command search.
    auto* palette = new CommandPalette(w.m_actions, &w);
    palette->setAttribute(Qt::WA_DeleteOnClose);
    palette->show();
    QApplication::setActiveWindow(palette);
    auto* input = palette->findChild<QLineEdit*>("paletteInput");
    input->setFocus();
    press(input, Qt::Key_Slash, Qt::NoModifier, "/");
    require(input->text() == "/" && hover->isChecked() == hoverOn, "in the command search / is typed");
    palette->close();
    // A sketch: / while a value is typed over the view goes on in the value (an expression); with nothing typed it is the
    // window's Hover highlight, as in 3D.
    QApplication::setActiveWindow(&w);
    v->setFocus();
    SketchEditor* sketch = w.m_design->sketch();
    sketch->begin({}, "Highlight keys", {{"base", "xy"}}, {}, opad::design::Sketch().to_json());
    sketch->setTool("line");
    QWidget* typing = QApplication::focusWidget() ? QApplication::focusWidget() : v;
    press(typing, Qt::Key_1, Qt::NoModifier, "1");
    press(QApplication::focusWidget() ? QApplication::focusWidget() : v, Qt::Key_0, Qt::NoModifier, "0");
    QWidget* box = QApplication::focusWidget() ? QApplication::focusWidget() : v;
    press(box, Qt::Key_Slash, Qt::NoModifier, "/");
    auto* edit = qobject_cast<QLineEdit*>(QApplication::focusWidget());
    require(hover->isChecked() == hoverOn && (!edit || edit->text().contains("10/")),
            "in a sketch a value being typed takes / (" + (edit ? edit->text() : QString("typed over the view")) + ")");
    sketch->setTool("select");
    v->setFocus();
    press(v, Qt::Key_Slash, Qt::NoModifier, "/");
    require(hover->isChecked() != hoverOn, "in a sketch with nothing typed / toggles the hover highlight");
    press(v, Qt::Key_Slash, Qt::NoModifier, "/");
    sketch->end();
    return ok;
  });
  return true;
}
