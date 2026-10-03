// Tool panels and the keyboard (TODO 11 UI-05), typed values outside the sketch (UI-122). Cases in
// tools/bench_cases/sketch.py; keys and clicks are Qt events sent where the keyboard is, as a user's would arrive.
#ifdef _WIN32  // first: OCCT's headers take it with the window functions left out
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
#include <QApplication>
#include <QCursor>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QKeyEvent>
#include <QLineEdit>
#include <QListWidget>
#include <QMouseEvent>
#include <QPushButton>
#include <QSlider>
#include <QTimer>
#include <QToolButton>
#include <QWindow>

#include <cmath>
#include <memory>
#include <optional>

#include "BenchRegistry.hpp"
#include "DimensionHandle.hpp"
#include "DrawingPlacer.hpp"
#include "MainWindow.hpp"
#include "ToolValues.hpp"
#include "opad/design/sketch_geom.hpp"
#include "opad/geometry.hpp"
#include "opad/inspect.hpp"

#include <TopExp.hxx>
#include <TopTools_IndexedMapOfShape.hxx>

namespace {
struct Checks {
  QString bench;
  bool all = true;
  void operator()(bool ok, const QString& what) {
    trace::log(QString("bench: %1: %2 %3").arg(bench, what, ok ? "PASS" : "FAIL"));
    all = all && ok;
  }
};

// Polls `done` every 20 ms until it holds or `ms` have passed, then calls `then` with the outcome.
void waitFor(QObject* context, std::function<bool()> done, int ms, std::function<void(bool)> then) {
  auto* timer = new QTimer(context);
  auto clock = std::make_shared<QElapsedTimer>();
  clock->start();
  QObject::connect(timer, &QTimer::timeout, context, [timer, clock, done, ms, then] {
    const bool ok = done();
    if (!ok && clock->elapsed() < ms) return;
    timer->stop();
    timer->deleteLater();
    then(ok);
  });
  timer->start(20);
}

// A left click in the middle of `target`, press and release, through the application (its filters see it); the press gives
// it the keyboard by its focus policy, as Qt does for a real (spontaneous) one.
void click(QWidget* target) {
  const QPointF local = QRectF(target->rect()).center(), global = target->mapToGlobal(local);
  if (target->focusPolicy() & Qt::ClickFocus) target->setFocus(Qt::MouseFocusReason);
  QMouseEvent press(QEvent::MouseButtonPress, local, global, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
  QApplication::sendEvent(target, &press);
  QMouseEvent release(QEvent::MouseButtonRelease, local, global, Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
  QApplication::sendEvent(target, &release);
}

// A key pressed where the keyboard is (not spontaneous: Qt offers it to the shortcuts first, as it does a typed key).
void press(int key, Qt::KeyboardModifiers modifiers = Qt::NoModifier, const QString& text = {}) {
  QWidget* to = QApplication::focusWidget();
  if (!to) to = QApplication::activeWindow();
  if (!to) return;
  QKeyEvent event(QEvent::KeyPress, key, modifiers, text);
  QApplication::sendEvent(to, &event);
}

// A panel has the keyboard, as after a click into one of its text fields: the active window, `focus` focused in it; the
// main window's own focus is elsewhere (on `away`), so giving it back is seen.
void panelHasKeyboard(QWidget* panel, QWidget* focus, QWidget* away) {
  QApplication::setActiveWindow(panel);
  focus->setFocus(Qt::MouseFocusReason);
  away->setFocus(Qt::OtherFocusReason);
}

// Steps run one after another: a step calls next() when it is done (after waiting for a job, say), or ends the bench.
struct Steps {
  std::vector<std::function<void()>> list;
  void next() {
    if (list.empty()) return;
    auto step = list.front();
    list.erase(list.begin());
    step();
  }
};

// The face of `body` facing +Z, by ordinal (-1: none).
int topFace(const AppDocument* doc, const std::string& body) {
  TopTools_IndexedMapOfShape faces;
  TopExp::MapShapes(opad::node_world_shape(doc->doc, doc->scene, body), TopAbs_FACE, faces);
  for (int i = 1; i <= faces.Extent(); ++i) {
    const opad::json d = opad::describe_entity(faces(i));
    if (d.contains("normal") && d["normal"][2].get<double>() > 0.99) return i - 1;
  }
  return -1;
}
}  // namespace

// OPAD_BENCH_PANEL_FOCUS=1 on a document with a body (UI-05): the Section panel has the keyboard (a text field of it was
// typed into). A click on its Y button gives the window and the keyboard back to the view, and so does one on its slider;
// a click into its offset field keeps the keyboard there. A click activates a panel only on a text field or a list (not on a
// button, the slider or the header), on Windows asked by WM_MOUSEACTIVATE as the system asks (a button click raises the
// panel all the same). Esc typed while a button of the panel has the keyboard is the panel's: it closes (it was ambiguous
// with the window's Esc, and nothing happened).
OPAD_BENCH(OPAD_BENCH_PANEL_FOCUS, panelFocus) {
  auto check = std::make_shared<Checks>(Checks{"panel focus"});
  auto finish = [check] { QCoreApplication::exit(check->all ? 0 : 2); };
  ToolPanel* panel = w.m_sectionPanel;
  QWidget* home = w.m_viewport;
  QWidget* away = w.m_timeline;
  w.action("inspect.section")->setChecked(true);
  QToolButton* y = nullptr;
  for (auto* b : w.m_section->findChildren<QToolButton*>())
    if (b->text() == "Y") y = b;
  auto* slider = w.m_section->findChild<QSlider*>();
  auto* field = w.m_section->findChild<QLineEdit*>();
  auto* list = w.m_section->findChild<QListWidget*>();
  (*check)(panel->isVisible() && y && slider && field && list, "the Section panel is open with its axis buttons, slider, offset field and list");
  if (!y || !slider || !field || !list) {
    finish();
    return true;
  }
  auto at = [](QWidget* widget) { return widget->mapToGlobal(widget->rect().center()); };
  (*check)(panel->takesKeyboardAt(at(field)) && panel->takesKeyboardAt(at(list)) && !panel->takesKeyboardAt(at(y)) && !panel->takesKeyboardAt(at(slider)) &&
               !panel->takesKeyboardAt(panel->mapToGlobal(QPoint(ToolPanel::kMargin + 60, ToolPanel::kMargin + 16))),
           "a click activates the panel on its text field and its list only (not a button, the slider or the header)");
#ifdef Q_OS_WIN
  {
    // What Windows asks before a click activates a window, sent as the system sends it: the panel says MA_NOACTIVATE but on
    // its text field or its list, and comes to the front over another panel all the same. A bench panel is never on the
    // screen (Qt does not move its native window either): that is moved for the question, the pointer over the widget.
    const HWND hwnd = reinterpret_cast<HWND>(panel->winId()), main = reinterpret_cast<HWND>(w.winId());
    const HWND other = reinterpret_cast<HWND>(w.m_annotationsPanel->winId());
    auto asked = [panel, hwnd, main](QWidget* target) {
      RECT before{};
      GetWindowRect(hwnd, &before);
      const QPoint inside = target->mapTo(panel, target->rect().center()) * panel->windowHandle()->devicePixelRatio();  // device pixels
      LRESULT answer = -1;
      for (int attempt = 0; attempt < 5 && answer == -1; ++attempt) {
        POINT pointer{}, after{};
        GetCursorPos(&pointer);
        SetWindowPos(hwnd, nullptr, pointer.x - inside.x(), pointer.y - inside.y(), 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        const LRESULT r = SendMessageW(hwnd, WM_MOUSEACTIVATE, WPARAM(main), MAKELPARAM(HTCLIENT, WM_LBUTTONDOWN));
        GetCursorPos(&after);
        if (after.x == pointer.x && after.y == pointer.y) answer = r;  // the pointer stayed where the panel was put
      }
      SetWindowPos(hwnd, nullptr, before.left, before.top, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
      return answer;
    };
    auto over = [](HWND top, HWND under) {
      for (HWND h = GetWindow(top, GW_HWNDNEXT); h; h = GetWindow(h, GW_HWNDNEXT))
        if (h == under) return true;
      return false;
    };
    SetWindowPos(other, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    const bool behind = over(other, hwnd);
    const LRESULT button = asked(y);
    const bool front = over(hwnd, other);
    const LRESULT text = asked(field), items = asked(list);
    (*check)(button == MA_NOACTIVATE && text == MA_ACTIVATE && items == MA_ACTIVATE,
             QString("WM_MOUSEACTIVATE: no activation on a button, activation on the text field and the list (%1 %2 %3)").arg(button).arg(text).arg(items));
    (*check)(behind && front, "a click on a button of a panel behind another brings it to the front");
  }
#endif

  panelHasKeyboard(panel, field, away);
  (*check)(QApplication::activeWindow() == panel && QApplication::focusWidget() == field && w.focusWidget() == away, "the panel has the keyboard, in its offset field");
  click(y);
  waitFor(&w, [&w, home] { return w.focusWidget() == home; }, 1000, [&w, check, finish, panel, home, away, y, slider, field](bool back) {
    (*check)(back && std::abs(w.m_section->normal()[1]) > 0.9,
             QString("a click on the Y button sets the axis and gives the keyboard back to the view (active: %1)").arg(QApplication::activeWindow() == &w ? "the window" : "the panel, not activated in a hidden run"));
    panelHasKeyboard(panel, y, away);
    click(field);
    QTimer::singleShot(100, &w, [&w, check, finish, panel, home, away, y, slider, field] {
      (*check)(w.focusWidget() == away && QApplication::focusWidget() == field,
               QString("a click into the offset field keeps the keyboard in the panel (window focus %1, focus %2, active %3)")
                   .arg(w.focusWidget() ? w.focusWidget()->metaObject()->className() : "none", QApplication::focusWidget() ? QApplication::focusWidget()->metaObject()->className() : "none",
                        QApplication::activeWindow() ? QApplication::activeWindow()->metaObject()->className() : "none"));
      panelHasKeyboard(panel, field, away);
      click(slider);
      waitFor(&w, [&w, home] { return w.focusWidget() == home; }, 1000, [&w, check, finish, panel, away, y](bool back) {
        (*check)(back, "a click on the slider gives the keyboard back to the view");
        panelHasKeyboard(panel, y, away);
        press(Qt::Key_Escape);
        (*check)(!panel->isVisible(), "Esc typed while a button of the panel has the keyboard closes the panel");
        QApplication::setActiveWindow(&w);
        w.action("inspect.section")->setChecked(false);
        finish();
      });
    });
  });
  return true;
}

// OPAD_BENCH_FEATURE_KEYS=<prefix> on a document with a body (UI-122). The control: with no tool running, 3 and 1 typed over
// the view switch the filter. Fillet with an edge picked: a keypad 3 typed over the view starts the radius box beside the
// pointer (the radius is 3, the filter and the picked edge stay), Esc puts the radius back, the next Esc leaves the fillet;
// 5 typed while a button of the feature panel has the keyboard is the radius too (no display style, no filter). Extrude on
// the top face: 25 typed over the view goes into the box by the arrow, Tab moves on to the taper, 3 there is 3 deg, Enter
// makes it. Fillet: 4 and Enter make a 4 mm fillet, the filter unchanged. <prefix>.fillet-box.png, .extrude-handle.png.
OPAD_BENCH(OPAD_BENCH_FEATURE_KEYS, featureKeys) {
  auto check = std::make_shared<Checks>(Checks{"feature keys"});
  auto steps = std::make_shared<Steps>();
  auto next = [steps] { QTimer::singleShot(0, qApp, [steps] { steps->next(); }); };
  auto finish = [check] { QCoreApplication::exit(check->all ? 0 : 2); };
  const QString prefix = value;
  DesignController* design = w.m_design;
  FeaturePanel* form = design->featurePanel();
  ToolValues* values = design->values();
  DimensionHandle* handle = design->distanceHandle();
  Viewport* view = w.m_viewport;
  AppDocument* doc = w.m_doc;
  MainWindow* window = &w;
  const std::string body = doc->scene.all_bodies().empty() ? std::string() : doc->scene.all_bodies().front();
  QAction* style = nullptr;
  for (const char* id : {"view.shaded", "view.edges", "view.wire"})
    if (w.action(id)->isChecked()) style = w.action(id);
  QAction* edgesFilter = w.action("select.edges");
  QAction* facesFilter = w.action("select.faces");
  auto keyboard = [window, view] {
    QApplication::setActiveWindow(window);
    view->setFocus();
  };
  auto pick = [design, view, body](opad::Ref::Kind kind, int index) {
    opad::Ref r;
    r.body = body;
    r.kind = kind;
    r.index = index;
    view->selectRefs({r});
    design->viewportSelectionChanged();
  };
  auto picked = [form](const QString& input) {
    const opad::json p = form->picks(input);
    return p.is_array() ? int(p.size()) : p.is_null() ? 0 : 1;
  };
  auto box = [values](int i) { return values->input()->box(i) ? values->input()->box(i)->text() : QString("<none>"); };
  auto unchanged = [style, view, edgesFilter] { return style->isChecked() && view->selectionFilter() == Viewport::SelFilter::Edge && edgesFilter->isChecked(); };
  auto input = [form](const char* name) { return QString::fromStdString(form->inputs().value(name, std::string())); };
  // A feature started and its pick input's filter applied, then `then`.
  auto start = [window, design, check](const QString& kind, QAction* filter, std::function<void()> then) {
    design->startFeature(kind);
    waitFor(window, [filter] { return filter->isChecked(); }, 10000, [check, design, kind, then](bool ok) {
      (*check)(ok && design->featureActive(), kind + " waits for its picks, the filter's button following");
      then();
    });
  };
  auto committed = [window, design, doc](size_t before, std::function<void(const opad::Feature*)> then) {
    waitFor(window, [design, doc, before] { return !design->featureActive() && doc->scene.features.size() > before && !doc->designBusy; }, 30000,
            [doc, then](bool ok) { then(ok ? &doc->scene.features.back() : nullptr); });
  };
  (*check)(!body.empty() && !doc->browse && style && edgesFilter && facesFilter, "an editable document with a body");
  if (body.empty() || !style) {
    finish();
    return true;
  }
  w.setWorkspace("design");

  steps->list.push_back([=] {  // the control: no tool runs, digits are the filters' keys
    keyboard();
    press(Qt::Key_3, Qt::NoModifier, "3");
    const bool edges = view->selectionFilter() == Viewport::SelFilter::Edge;
    press(Qt::Key_1, Qt::NoModifier, "1");
    (*check)(edges && view->selectionFilter() == Viewport::SelFilter::Body, "control: with no tool running, 3 and 1 typed over the view switch the filter");
    start("fillet", edgesFilter, [=] {
      pick(opad::Ref::Kind::Edge, 0);
      (*check)(picked("edges") == 1, "an edge is picked");
      next();
    });
  });
  steps->list.push_back([=] {  // a keypad digit over the view, Esc, Esc
    keyboard();
    press(Qt::Key_3, Qt::KeypadModifier, "3");
    QLineEdit* radius = values->input()->box(0);
    (*check)(values->input()->isVisible() && values->input()->count() == 1 && values->input()->key(0) == "radius" && radius && QApplication::focusWidget() == radius && box(0) == "3",
             "a keypad 3 typed over the view starts the radius box beside the pointer, which takes the keyboard");
    (*check)(form->valueText("radius") == "3" && unchanged() && picked("edges") == 1, "the radius is 3, the filter and the display style stay, the edge stays picked");
    if (!prefix.isEmpty()) values->input()->shot().save(prefix + ".fillet-box.png");
    press(Qt::Key_Escape);
    (*check)(form->valueText("radius") == "2 mm" && box(0).isEmpty() && design->featureActive(), "Esc puts the radius back (2 mm)");
    press(Qt::Key_Escape);
    (*check)(!design->featureActive() && !values->input()->isVisible(), "the next Esc leaves the fillet");
    start("fillet", edgesFilter, [=] {
      pick(opad::Ref::Kind::Edge, 0);
      next();
    });
  });
  steps->list.push_back([=] {  // typed while the feature panel has the keyboard
    QPushButton* button = nullptr;
    for (auto* b : form->findChildren<QPushButton*>())
      if (b->isVisible() && b->focusPolicy() != Qt::NoFocus) button = b;
    (*check)(button != nullptr, "the feature panel has a button that takes the keyboard");
    if (button) panelHasKeyboard(window->m_featurePanel, button, window->m_timeline);
    press(Qt::Key_5, Qt::NoModifier, "5");
    (*check)(button && form->valueText("radius") == "5" && unchanged() && picked("edges") == 1,
             "5 typed while a button of the feature panel has the keyboard is the radius: no display style, no filter");
    QTimer::singleShot(100, window, [=] {
      for (int i = 0; i < 3 && design->featureActive(); ++i) press(Qt::Key_Escape);
      (*check)(!design->featureActive(), "Esc leaves it");
      keyboard();
      const int top = topFace(doc, body);
      start("extrude", facesFilter, [=] {
        pick(opad::Ref::Kind::Face, top);
        (*check)(top >= 0 && picked("profiles") == 1, "Extrude: the top face is picked");
        waitFor(window, [handle] { return handle->isVisible(); }, 20000, [=](bool shown) {
          (*check)(shown, "the extrusion's arrow and its box show");
          next();
        });
      });
    });
  });
  steps->list.push_back([=] {  // the boxes by the arrow: distance, Tab, taper, Enter
    keyboard();
    press(Qt::Key_2, Qt::NoModifier, "2");
    press(Qt::Key_5, Qt::NoModifier, "5");
    DynamicInput* boxes = handle->input();
    (*check)(boxes->box(0)->text() == "25" && input("distance") == "25 mm" && !values->input()->isVisible(), "25 typed over the view goes into the box by the arrow: 25 mm");
    (*check)(boxes->count() == 2 && boxes->key(1) == "taper", "the arrow's boxes: the distance and the taper");
    press(Qt::Key_Tab);
    (*check)(boxes->box(1) && QApplication::focusWidget() == boxes->box(1), "Tab moves from the distance to the taper");
    press(Qt::Key_3, Qt::NoModifier, "3");
    (*check)(input("taper") == "3 deg" && input("distance") == "25 mm", "3 typed there is a 3 deg taper, the distance stays 25 mm");
    if (!prefix.isEmpty()) handle->grab().save(prefix + ".extrude-handle.png");
    const size_t before = doc->scene.features.size();
    press(Qt::Key_Return);
    committed(before, [=](const opad::Feature* f) {
      (*check)(f && f->kind == "extrude" && f->inputs.value("distance", "") == "25 mm" && f->inputs.value("taper", "") == "3 deg" && f->error.empty(),
               "Enter makes the extrusion: 25 mm, tapered 3 deg");
      start("fillet", edgesFilter, [=] {
        pick(opad::Ref::Kind::Edge, 0);
        next();
      });
    });
  });
  steps->list.push_back([=] {  // 4 and Enter
    keyboard();
    press(Qt::Key_4, Qt::NoModifier, "4");
    (*check)(box(0) == "4" && form->valueText("radius") == "4" && unchanged() && picked("edges") == 1, "Fillet: 4 typed over the view is the radius, the filter stays Edges");
    const size_t before = doc->scene.features.size();
    press(Qt::Key_Return);
    committed(before, [=](const opad::Feature* f) {
      (*check)(f && f->kind == "fillet" && f->inputs.value("radius", "") == "4 mm" && f->error.empty(), "Enter makes a 4 mm fillet");
      finish();
    });
  });
  next();
  return true;
}

// OPAD_BENCH_TOOL_KEYS=<prefix> on a document with a body (UI-122): the Section panel open, 4.537 typed over the view puts
// the plane exactly there (the slider has 1000 steps), the filter stays; Enter keeps it, Esc closes the panel, and then 2
// is the Faces filter's key again. A drawing being placed: 12, Tab, -3 typed over the view move it by (12, -3) as they are
// typed, the filter stays; Enter places it there. <prefix>.section-box.png, <prefix>.placer-boxes.png.
OPAD_BENCH(OPAD_BENCH_TOOL_KEYS, toolKeys) {
  auto check = std::make_shared<Checks>(Checks{"tool keys"});
  auto finish = [check] { QCoreApplication::exit(check->all ? 0 : 2); };
  const QString prefix = value;
  MainWindow* window = &w;
  Viewport* view = w.m_viewport;
  SectionPanel* section = w.m_section;
  DrawingPlacer* placer = w.m_drawingPlacer;
  QAction* bodies = w.action("select.bodies");
  auto keyboard = [window, view] {
    QApplication::setActiveWindow(window);
    view->setFocus();
  };
  auto unchanged = [view, bodies] { return view->selectionFilter() == Viewport::SelFilter::Body && bodies->isChecked(); };
  keyboard();
  w.action("inspect.section")->setChecked(true);
  DynamicInput* offset = section->values()->input();
  (*check)(w.m_sectionPanel->isVisible() && section->enabled() && unchanged(), "the Section panel is open, the filter on bodies");
  press(Qt::Key_4, Qt::NoModifier, "4");
  (*check)(offset->isVisible() && offset->count() == 1 && offset->key(0) == "offset" && offset->box(0)->text() == "4" && QApplication::focusWidget() == offset->box(0) &&
               std::abs(section->along() - 4) < 1e-9 && unchanged(),
           "4 typed over the view starts the section's Offset box: the plane at 4 mm, the filter stays");
  for (const QChar c : QString(".537")) press(c == '.' ? Qt::Key_Period : Qt::Key_0 + c.digitValue(), Qt::NoModifier, QString(c));
  (*check)(std::abs(section->along() - 4.537) < 1e-9 && std::abs(section->origin()[2] - 4.537) < 1e-9, "4.537 puts the plane exactly there, not on the slider's step");
  if (!prefix.isEmpty()) offset->shot().save(prefix + ".section-box.png");
  press(Qt::Key_Return);
  (*check)(!offset->isVisible() && std::abs(section->along() - 4.537) < 1e-9 && QApplication::focusWidget() == view, "Enter keeps it and gives the keyboard back to the view");
  press(Qt::Key_Escape);
  (*check)(!w.m_sectionPanel->isVisible() && section->enabled(), "Esc closes the panel, the section stays");
  press(Qt::Key_2, Qt::NoModifier, "2");
  (*check)(view->selectionFilter() == Viewport::SelFilter::Face && !offset->isVisible(), "with the panel closed, 2 is the Faces filter's key again");
  press(Qt::Key_1, Qt::NoModifier, "1");
  w.action("inspect.section")->setChecked(false);

  // A drawing being placed on XY.
  const QString drawing = QDir::temp().filePath(QString("opad-tool-keys-%1.svg").arg(QCoreApplication::applicationPid()));
  {
    QFile file(drawing);
    file.open(QIODevice::WriteOnly);
    file.write("<svg width=\"40mm\" viewBox=\"0 0 40 40\"><rect width=\"20\" height=\"10\"/></svg>");
  }
  auto placed = std::make_shared<std::optional<opad::Vec3>>();
  auto ready = std::make_shared<bool>(false);
  placer->placed = [placed](const opad::Mat4& m) { *placed = m.apply({0, 0, 0}); };
  QObject::connect(placer, &DrawingPlacer::ready, window, [ready] { *ready = true; }, Qt::SingleShotConnection);
  placer->start(drawing, opad::design::base_frame("xy"), [window](ToolPanel* panel) { window->openPanel(panel); });
  waitFor(window, [ready] { return *ready; }, 20000, [=](bool shown) {
    (*check)(shown && placer->active(), "the drawing is on the plane, to be placed");
    keyboard();
    DynamicInput* boxes = placer->values()->input();
    press(Qt::Key_1, Qt::NoModifier, "1");
    press(Qt::Key_2, Qt::NoModifier, "2");
    const opad::Vec3 at = placer->placement().apply({0, 0, 0});
    (*check)(boxes->isVisible() && boxes->count() == 2 && boxes->box(0)->text() == "12" && std::abs(at[0] - 12) < 1e-9 && std::abs(at[1]) < 1e-9 && unchanged(),
             "12 typed over the view moves the drawing 12 along X as it is typed, the filter stays");
    press(Qt::Key_Tab);
    press(Qt::Key_Minus, Qt::NoModifier, "-");
    press(Qt::Key_3, Qt::NoModifier, "3");
    const opad::Vec3 to = placer->placement().apply({0, 0, 0});
    (*check)(QApplication::focusWidget() == boxes->box(1) && std::abs(to[0] - 12) < 1e-9 && std::abs(to[1] + 3) < 1e-9, "Tab, then -3: the Y offset");
    if (!prefix.isEmpty()) boxes->shot().save(prefix + ".placer-boxes.png");
    press(Qt::Key_Return);
    (*check)(placed->has_value() && std::abs((**placed)[0] - 12) < 1e-9 && std::abs((**placed)[1] + 3) < 1e-9 && !placer->active(), "Enter places the drawing at (12, -3)");
    QFile::remove(drawing);
    finish();
  });
  return true;
}
