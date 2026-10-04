// Select other (UI-128): what lies under the pointer, listed (Alt+click) or hovered in turn (Tab), when the nearest thing
// is not the one meant: the face behind, an edge against a face, coincident bodies.
#include "Viewport.hpp"

#include <QCoreApplication>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QMenu>
#include <QTimer>
#include <QToolTip>

#include <StdSelect_BRepOwner.hxx>

#include <algorithm>

#include "I18n.hpp"
#include "Jobs.hpp"
#include "opad/geometry.hpp"

namespace {
constexpr size_t kMostCandidates = 24;
}

// The owners picking finds in the current filter, nearest first; never an occluder (a face standing in front of edges and
// vertices, UI-31) or an arc's centre finder, and only bodies, sketches and feature candidates.
bool Viewport::selectOtherOwner(const Handle(SelectMgr_EntityOwner)& owner, opad::Ref& ref, std::string& candidate) const {
  if (owner.IsNull() || !Handle(OccluderOwner)::DownCast(owner).IsNull() || !Handle(CircleOwner)::DownCast(owner).IsNull()) return false;
  const auto object = Handle(AIS_InteractiveObject)::DownCast(owner->Selectable());
  ref = opad::Ref();
  candidate.clear();
  if (const auto node = m_nodeOf.find(object.get()); node != m_nodeOf.end()) {
    ref.body = node->second;
    if (m_filter == SelFilter::Body) return true;
    if (const auto mine = Handle(SubShapeOwner)::DownCast(owner); !mine.IsNull()) {
      ref.kind = mine->kind();
      ref.index = mine->index();
      return true;
    }
    const auto stock = Handle(StdSelect_BRepOwner)::DownCast(owner);
    const auto shape = Handle(AIS_Shape)::DownCast(object);
    if (stock.IsNull() || !stock->HasShape() || shape.IsNull()) return true;
    const TopAbs_ShapeEnum type = stock->Shape().ShapeType();
    ref.kind = type == TopAbs_FACE ? opad::Ref::Kind::Face : type == TopAbs_EDGE ? opad::Ref::Kind::Edge : type == TopAbs_VERTEX ? opad::Ref::Kind::Vertex : opad::Ref::Kind::Body;
    if (ref.kind != opad::Ref::Kind::Body) ref.index = opad::subshape_index(shape->Shape(), stock->Shape());  // a body drawn the stock way
    return true;
  }
  for (const auto& [id, ais] : m_candidates)
    if (ais.get() == object.get()) {
      candidate = id;
      return true;
    }
  return false;
}

std::vector<Viewport::PickCandidate> Viewport::pickCandidates(const QPointF& at) {
  std::vector<PickCandidate> out;
  m_pickOwners.clear();
  if (!m_initialised || m_sketchInput) return out;
  m_pickAt = devicePos(at);
  ResetPreviousMoveTo();  // the controller's next move picks again
  m_ctx->MoveTo(m_pickAt.x(), m_pickAt.y(), m_view, Standard_False);
  const auto& selector = m_ctx->MainSelector();
  const auto camera = m_view->Camera();
  std::vector<std::pair<double, Handle(SelectMgr_EntityOwner)>> found;
  for (int i = 1; i <= selector->NbPicked(); ++i) {
    const auto owner = selector->Picked(i);
    opad::Ref ref;
    std::string candidate;
    if (!selectOtherOwner(owner, ref, candidate)) continue;
    if (std::any_of(found.begin(), found.end(), [&](const auto& f) { return f.second == owner; })) continue;
    found.push_back({gp_Vec(camera->Eye(), selector->PickedPoint(i)).Dot(gp_Vec(camera->Direction())), owner});
  }
  std::stable_sort(found.begin(), found.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
  for (const auto& [depth, owner] : found) {
    if (out.size() == kMostCandidates) break;
    PickCandidate c;
    selectOtherOwner(owner, c.ref, c.candidate);
    c.depth = depth;
    if (!c.candidate.empty()) {
      const opad::json id = opad::json::parse(c.candidate, nullptr, false);
      const std::string base = id.is_object() ? id.value("base", "") : "";
      const bool axis = base == "x" || base == "y" || base == "z";
      c.label = base.empty() ? tr("Construction geometry") : (axis ? tr("%1 axis") : tr("%1 plane")).arg(QString::fromStdString(base).toUpper());
    } else {
      c.label = hoverName(c.ref.body);
      if (c.ref.kind != opad::Ref::Kind::Body) c.label += QString::fromUtf8(" › %1 %2").arg(i18n::t(opad::Ref::kind_name(c.ref.kind))).arg(c.ref.index);
    }
    out.push_back(c);
    m_pickOwners.push_back(owner);
  }
  return out;
}

// The context's detected owner made `owner`, as if the pointer had found it first (picked again at the list's point when
// the pick has moved on); it is hovered there and a click takes it.
bool Viewport::detectOwner(const Handle(SelectMgr_EntityOwner)& owner) {
  bool present = false;
  for (m_ctx->InitDetected(); m_ctx->MoreDetected() && !present; m_ctx->NextDetected()) present = m_ctx->DetectedCurrentOwner() == owner;
  if (!present) m_ctx->MoveTo(m_pickAt.x(), m_pickAt.y(), m_view, Standard_False);
  for (int i = 0; i < 256 && m_ctx->HasDetected() && m_ctx->DetectedOwner() != owner; ++i) m_ctx->HilightNextDetected(m_view, Standard_False);
  const bool done = m_ctx->HasDetected() && m_ctx->DetectedOwner() == owner;
  if (done) {
    m_hoverCycled = true;  // dropOccluded leaves it: the face behind is the point
    m_cycledAt = m_pickAt;
  }
  m_view->InvalidateImmediate();
  requestRedraw();
  return done;
}

void Viewport::previewPickCandidate(int index) {
  if (!m_initialised) return;
  if (index >= 0 && index < static_cast<int>(m_pickOwners.size())) {
    detectOwner(m_pickOwners[index]);
    return;
  }
  m_hoverCycled = false;
  ResetPreviousMoveTo();
  m_ctx->ClearDetected(Standard_False);
  m_view->InvalidateImmediate();
  requestRedraw();
}

bool Viewport::choosePickCandidate(int index) {
  if (!m_initialised || index < 0 || index >= static_cast<int>(m_pickOwners.size()) || !detectOwner(m_pickOwners[index])) return false;
  m_ctx->SelectDetected(m_pickAccumulate ? AIS_SelectionScheme_XOR : AIS_SelectionScheme_Replace);
  m_cubeClick = false;
  OnSelectionChanged(m_ctx, m_view);  // as a click: listeners, the pick's point (a guided tool's marker), highlights
  return true;
}

QMenu* Viewport::selectOtherMenu(const QPointF& at, size_t fewest) {
  if (auto* old = findChild<QMenu*>("selectOther")) old->close();
  const auto candidates = pickCandidates(at);
  if (candidates.size() < std::max<size_t>(fewest, 1)) {
    if (fewest <= 1) QToolTip::showText(mapToGlobal(at.toPoint()) + QPoint(14, 18), tr("Nothing to select here"), this, QRect(), 1500);
    return nullptr;
  }
  auto* menu = new QMenu(this);
  menu->setObjectName("selectOther");
  menu->setAttribute(Qt::WA_DeleteOnClose);
  menu->setToolTipsVisible(true);
  menu->setTitle(tr("Select other"));
  menu->addAction(tr("Under the pointer, nearest first"))->setEnabled(false);
  menu->addSeparator();
  auto chosen = std::make_shared<bool>(false);
  for (size_t i = 0; i < candidates.size(); ++i) {
    QAction* a = menu->addAction(candidates[i].label);
    a->setData(static_cast<int>(i));
    a->setToolTip(i == 0 ? tr("Nearest under the pointer") : tr("Behind the one above"));
    connect(a, &QAction::triggered, this, [this, i, chosen] { *chosen = choosePickCandidate(static_cast<int>(i)); });
  }
  connect(menu, &QMenu::hovered, this, [this](QAction* a) {
    if (a->data().isValid()) previewPickCandidate(a->data().toInt());
  });
  // Closed without a choice: what was previewed is hovered no longer (the menu's triggered comes after it hides).
  connect(menu, &QMenu::aboutToHide, this, [this, menu, chosen] {
    menu->setObjectName(QString());  // closed (deleted later): findChild finds the open one only
    QTimer::singleShot(0, this, [this, chosen] { if (!*chosen) previewPickCandidate(-1); });
  });
  if (trace::enabled()) trace::log(QStringLiteral("select other: %1 under the pointer").arg(candidates.size()));
  return menu;
}

// A plain left press held still: the list as Alt+click opens it, when more than one thing is under the pointer (a slow click
// on one thing stays a click). The controller forgets the press, so its release neither clicks nor ends a rubber band.
void Viewport::pressHeld() {
  if (!m_initialised || m_blocked || m_sketchInput || m_selectOtherPress || m_measureAnchorPress || m_zoomDrag) return;
  QMenu* menu = selectOtherMenu(m_holdAt, 2);
  if (!menu) return;
  ResetViewInput();
  myUI.Selection.Points.Clear();  // a band begun within the click tolerance comes down with the next frame
  m_holdPress = true;
  if (trace::enabled()) trace::log(QStringLiteral("select other: press held"));
  menu->popup(mapToGlobal(m_holdAt.toPoint()));
  requestRedraw();
}

// Tab / Shift+Tab with the pointer resting on the view: the next or previous thing under it is hovered in place.
bool Viewport::cycleHover(bool forward) {
  if (!m_initialised || m_sketchInput || m_blocked || !m_ctx->HasDetected()) return false;
  std::vector<Handle(SelectMgr_EntityOwner)> owners;
  for (m_ctx->InitDetected(); m_ctx->MoreDetected(); m_ctx->NextDetected()) {
    const auto owner = m_ctx->DetectedCurrentOwner();
    opad::Ref ref;
    std::string candidate;
    if (selectOtherOwner(owner, ref, candidate) && std::find(owners.begin(), owners.end(), owner) == owners.end()) owners.push_back(owner);
  }
  if (owners.size() < 2) return false;
  const auto now = std::find(owners.begin(), owners.end(), m_ctx->DetectedOwner());
  const int n = static_cast<int>(owners.size()), at = now == owners.end() ? (forward ? -1 : 0) : static_cast<int>(now - owners.begin());
  const auto target = owners[((forward ? at + 1 : at - 1) % n + n) % n];
  for (int i = 0; i < 256 && m_ctx->DetectedOwner() != target; ++i)
    if (forward) m_ctx->HilightNextDetected(m_view, Standard_False);
    else m_ctx->HilightPreviousDetected(m_view, Standard_False);
  if (m_ctx->DetectedOwner() != target) return false;
  m_hoverCycled = true;
  m_cycledAt = devicePos(m_trackingCursor);
  m_view->InvalidateImmediate();
  requestRedraw();
  if (trace::enabled()) trace::log(QStringLiteral("select other: Tab hovers %1 of %2").arg(std::find(owners.begin(), owners.end(), target) - owners.begin() + 1).arg(n));
  return true;
}

bool Viewport::cycleKey(QEvent* e) {
  if (e->type() != QEvent::KeyPress) return false;
  auto* k = static_cast<QKeyEvent*>(e);
  if ((k->key() != Qt::Key_Tab && k->key() != Qt::Key_Backtab) || (k->modifiers() & ~(Qt::ShiftModifier | Qt::KeypadModifier))) return false;
  if (!cycleHover(k->key() == Qt::Key_Tab && !(k->modifiers() & Qt::ShiftModifier))) return false;
  e->accept();
  return true;
}

void Viewport::benchHoverAt(const QPointF& at) {
  if (!m_initialised) return;
  m_view->Redraw();  // the picker needs a frame after a camera change
  QMouseEvent move(QEvent::MouseMove, at, mapToGlobal(at), Qt::NoButton, Qt::NoButton, Qt::NoModifier);
  QCoreApplication::sendEvent(this, &move);
  paintEvent(nullptr);
}
