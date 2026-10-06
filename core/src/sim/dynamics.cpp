// Dynamic studies with Project Chrono (sim/study.hpp).
//
// Units: Chrono works in SI (m, kg, s); OPAD's geometry is in mm. Parts are ChBodyAuxRef bodies whose reference frame is
// the part's placement, with the mass, centre and inertia of their solids (sim/inertia.hpp). Joints are Chrono's
// ChLinkLock joints at the joint's frames (marker 1 on the part, marker 2 on the base, so its relative motion is the
// joint's coordinates); gear, rack and pinion, lead screw, the screw joint's thread and position or speed drives are one
// scalar constraint each (CoordConstraint below) on the same coordinates the kinematic solver uses, so both solvers agree
// to the last digit. Torque and force drives, springs, dampers and joint friction are forces on the joint's coordinate,
// worked out before each step.
#include "opad/sim/study.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <set>

#include "opad/sim/inertia.hpp"
#include "opad/sim/joints.hpp"
#include "opad/sim/kinematics.hpp"

#ifdef OPAD_HAVE_CHRONO
#include <BRepMesh_IncrementalMesh.hxx>
#include <BRep_Tool.hxx>
#include <Poly_Triangulation.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Face.hxx>

#include "chrono/collision/ChCollisionShapeTriangleMesh.h"
#include "chrono/collision/ChCollisionSystem.h"
#include "chrono/geometry/ChTriangleMeshConnected.h"
#include "chrono/physics/ChBodyAuxRef.h"
#include "chrono/physics/ChContactMaterialNSC.h"
#include "chrono/physics/ChLinkBase.h"
#include "chrono/physics/ChLinkLock.h"
#include "chrono/physics/ChSystemNSC.h"
#include "chrono/solver/ChConstraintNgeneric.h"
#include "chrono/solver/ChDirectSolverLS.h"
#include "chrono/solver/ChIterativeSolverLS.h"
#include "chrono/solver/ChSolverAPGD.h"
#include "chrono/solver/ChSystemDescriptor.h"
#include "chrono/timestepper/ChTimestepperHHT.h"

#include "opad/design/expr.hpp"
#include "opad/geometry.hpp"
#endif

namespace opad::sim {

#ifndef OPAD_HAVE_CHRONO

StudyRun run_dynamic(const Document&, const Scene&, const json&, const Progress&) {
  throw Error("dynamic studies need Project Chrono: this build has none (configure with -DOPAD_CHRONO=ON)");
}

#else

using namespace chrono;

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kMm = 1e-3;  // m per mm

ChVector3d to_m(const Vec3& p) { return ChVector3d(p[0] * kMm, p[1] * kMm, p[2] * kMm); }

ChFramed frame_m(const Mat4& m) {
  ChMatrix33<> R;
  for (int i = 0; i < 3; ++i)
    for (int j = 0; j < 3; ++j) R(i, j) = m.at(i, j);
  return ChFramed(ChVector3d(m.at(0, 3) * kMm, m.at(1, 3) * kMm, m.at(2, 3) * kMm), R);
}

Mat4 mat_mm(const ChFramed& f) {
  Mat4 m;
  const ChMatrix33<>& R = f.GetRotMat();
  for (int i = 0; i < 3; ++i) {
    for (int j = 0; j < 3; ++j) m.at(i, j) = R(i, j);
    m.at(i, 3) = f.GetPos()[i] / kMm;
  }
  return m;
}

Mat4 mat_of_frame(const Frame& f) {
  const Vec3 z = f.normal();
  Mat4 m;
  for (int i = 0; i < 3; ++i) {
    m.at(i, 0) = f.x[size_t(i)];
    m.at(i, 1) = f.y[size_t(i)];
    m.at(i, 2) = z[size_t(i)];
    m.at(i, 3) = f.origin[size_t(i)];
  }
  return m;
}

// A body's reference frame (the part's placement) in mm, as the kinematic definitions take it.
Mat4 ref_mm(const ChBodyAuxRef* b) { return mat_mm(b->GetFrameRefToAbs()); }

// ---------------------------------------------------------------- a scalar constraint on joint coordinates
// C(frames of its bodies, t) = 0 with a numerical Jacobian over each body's 6 velocity coordinates (linear velocity of
// the centre of mass in the world, angular velocity in the body's own axes), the way Chrono's ChVariablesBody take them.
class CoordConstraint : public ChLinkBase {
 public:
  std::vector<ChBodyAuxRef*> bodies;
  std::function<double(const std::vector<Mat4>&, double)> residual;  // m or rad
  std::function<double(double)> rate;                                 // dC/dt at fixed positions
  double C = 0;
  double reaction = 0;  // the integrator's multiplier
  // HHT converges this constraint's multiplier to force / (1 + alpha) (checked at alpha 0, -0.02 and -0.1 against
  // I x angular acceleration; Chrono's own joints report theirs unscaled): force() undoes it.
  double scale = 1;
  double force() const { return reaction * scale; }  // N (per m of the equation) or N.m (per rad)

  CoordConstraint* Clone() const override { return new CoordConstraint(*this); }
  unsigned int GetNumConstraintsBilateral() override { return 1; }
  unsigned int GetNumAffectedCoords() override { return 6 * unsigned(bodies.size()); }
  ChFramed GetFrame1Abs() const override { return ChFramed(); }
  ChFramed GetFrame2Abs() const override { return ChFramed(); }
  ChWrenchd GetReaction1() const override { return {VNULL, VNULL}; }
  ChWrenchd GetReaction2() const override { return {VNULL, VNULL}; }

  void Setup() {
    std::vector<ChVariables*> vars;
    for (auto* b : bodies) vars.push_back(&b->Variables());
    cx.SetVariables(vars);
  }

  std::vector<Mat4> frames(int perturbed = -1, int k = 0, double h = 0) const {
    std::vector<Mat4> out;
    for (size_t i = 0; i < bodies.size(); ++i) {
      const ChBodyAuxRef* b = bodies[i];
      ChFramed ref = b->GetFrameRefToAbs();
      if (int(i) == perturbed) {
        if (k < 3) {
          ChVector3d d(0, 0, 0);
          d[k] = h;
          ref.SetPos(ref.GetPos() + d);
        } else {
          ChVector3d axis(0, 0, 0);
          axis[k - 3] = 1;
          const ChVector3d com = b->GetPos();
          const ChQuaterniond q = b->GetRot() * QuatFromAngleAxis(h, axis);  // turn about the body's own axis
          const ChQuaterniond dq = q * b->GetRot().GetConjugate();
          ref.SetPos(com + dq.Rotate(ref.GetPos() - com));
          ref.SetRot(dq * ref.GetRot());
        }
      }
      out.push_back(mat_mm(ref));
    }
    return out;
  }

  void Update(double time, bool update_assets) override {
    ChLinkBase::Update(time, update_assets);
    C = residual(frames(), time);
    const double h = 1e-7;
    for (size_t i = 0; i < bodies.size(); ++i)
      for (int k = 0; k < 6; ++k) {
        const double hk = k < 3 ? h : h;
        const double cp = residual(frames(int(i), k, hk), time), cm = residual(frames(int(i), k, -hk), time);
        cx.Get_Cq_N(i)(k) = (cp - cm) / (2 * hk);
      }
  }

  void IntStateGatherReactions(const unsigned int off_L, ChVectorDynamic<>& L) override { L(off_L) = reaction; }
  void IntStateScatterReactions(const unsigned int off_L, const ChVectorDynamic<>& L) override { reaction = L(off_L); }
  void IntLoadResidual_CqL(const unsigned int off_L, ChVectorDynamic<>& R, const ChVectorDynamic<>& L, const double c) override {
    cx.AddJacobianTransposedTimesScalarInto(R, L(off_L) * c);
  }
  void IntLoadConstraint_C(const unsigned int off, ChVectorDynamic<>& Qc, const double c, bool do_clamp, double recovery_clamp) override {
    double v = c * C;
    if (do_clamp) v = std::clamp(v, -recovery_clamp, recovery_clamp);
    Qc(off) += v;
  }
  void IntLoadConstraint_Ct(const unsigned int off, ChVectorDynamic<>& Qc, const double c) override {
    if (rate) Qc(off) += c * rate(GetChTime());
  }
  void IntToDescriptor(const unsigned int, const ChStateDelta&, const ChVectorDynamic<>&, const unsigned int off_L, const ChVectorDynamic<>& L,
                       const ChVectorDynamic<>& Qc) override {
    cx.SetLagrangeMultiplier(L(off_L));
    cx.SetRightHandSide(Qc(off_L));
  }
  void IntFromDescriptor(const unsigned int, ChStateDelta&, const unsigned int off_L, ChVectorDynamic<>& L) override {
    L(off_L) = cx.GetLagrangeMultiplier();
  }
  void InjectConstraints(ChSystemDescriptor& descriptor) override { descriptor.InsertConstraint(&cx); }
  void ConstraintsBiReset() override { cx.SetRightHandSide(0); }
  void ConstraintsBiLoad_C(double factor, double recovery_clamp, bool do_clamp) override {
    double v = factor * C;
    if (do_clamp) v = std::clamp(v, -recovery_clamp, recovery_clamp);
    cx.SetRightHandSide(cx.GetRightHandSide() + v);
  }
  void ConstraintsBiLoad_Ct(double factor) override {
    if (rate) cx.SetRightHandSide(cx.GetRightHandSide() + factor * rate(GetChTime()));
  }
  // The integrator's multiplier (gathered back as the old one by HHT) is what IntStateScatterReactions stored; a fetch after
  // a solve holds the last Newton increment, not the force, and must not overwrite it.
  void ConstraintsFetch_react(double) override {}

 private:
  ChConstraintNgeneric cx;
};

// Forces on joint coordinates (springs, dampers, friction, torque and force drives), worked out from the state the
// integrator is trying, at each of its iterations: stiff springs stay stable where a force held over the step would not.
// Reaches ChLinkLock's protected BuildLink(x, y, z, e0, e1, e2, e3) to switch single equations of a joint off.
struct LockAccess : ChLinkLock {
  static void keep(ChLinkLock& l, const std::array<bool, 7>& on) {
    auto build = static_cast<void (ChLinkLock::*)(bool, bool, bool, bool, bool, bool, bool)>(&LockAccess::BuildLink);
    (l.*build)(on[0], on[1], on[2], on[3], on[4], on[5], on[6]);
  }
};

// Leaves out the joint equations that repeat others (a planar loop of spatial joints). With them the system is
// rank-deficient: a direct QR returned garbage multipliers, and under MINRES the shared ones drifted along the null space
// (an engine's main bearing at 1e9 N, its motor torque ringing for hundreds of steps). Rows are taken greedily from a
// numerical Jacobian at the start, relations and drives first, then the joints' own: one that adds nothing to the span
// of those before it goes. Returns how many went.
int drop_redundant(ChSystem& sys) {
  sys.Setup();
  sys.Update(false);
  ChState X, Xk;
  ChStateDelta V, A;
  double T = 0;
  sys.StateSetup(X, V, A);
  sys.StateSetup(Xk, V, A);
  sys.StateGather(X, V, T);
  const int nc = int(sys.GetNumConstraints()), nv = int(sys.GetNumCoordsVelLevel());
  if (nc == 0 || nv == 0) return 0;
  Eigen::MatrixXd J(nc, nv);
  const double h = 1e-7;
  for (int j = 0; j < nv; ++j) {
    ChVectorDynamic<> cp(nc), cm(nc);
    cp.setZero();
    cm.setZero();
    ChStateDelta Dx(nv, &sys);
    Dx.setZero(nv, &sys);
    Dx(j) = h;
    sys.StateIncrementX(Xk, X, Dx);
    sys.StateScatter(Xk, V, T, true);
    sys.LoadConstraint_C(cp, 1.0, false);
    Dx(j) = -h;
    sys.StateIncrementX(Xk, X, Dx);
    sys.StateScatter(Xk, V, T, true);
    sys.LoadConstraint_C(cm, 1.0, false);
    J.col(j) = (cp - cm) / (2 * h);
  }
  sys.StateScatter(X, V, T, true);
  struct Row {
    ChLinkBase* link;
    int index;  // the link's k-th active equation
  };
  std::vector<Row> order;
  for (int pass = 0; pass < 2; ++pass)
    for (const auto& l : sys.GetLinks()) {
      const bool relation = bool(std::dynamic_pointer_cast<CoordConstraint>(l));
      if ((pass == 0) != relation || !l->IsActive()) continue;
      for (unsigned k = 0; k < l->GetNumConstraints(); ++k) order.push_back({l.get(), int(k)});
    }
  std::vector<Eigen::VectorXd> basis;
  std::map<ChLinkBase*, std::set<int>> dropped;
  int count = 0;
  for (const auto& r : order) {
    const Eigen::VectorXd row = J.row(int(r.link->GetOffset_L()) + r.index).transpose();
    Eigen::VectorXd w = row;
    for (int it = 0; it < 2; ++it)
      for (const auto& q : basis) w -= q.dot(w) * q;
    if (w.norm() > 1e-6 * std::max(row.norm(), 1e-12)) {
      basis.push_back(w / w.norm());
    } else {
      dropped[r.link].insert(r.index);
      ++count;
    }
  }
  for (const auto& [link, rows] : dropped) {
    if (auto lock = dynamic_cast<ChLinkLock*>(link)) {
      std::array<bool, 7> on{};
      int active = 0;
      for (unsigned i = 0; i < 7; ++i) {
        const bool locked = lock->GetMask().GetConstraint(i).GetMode() == ChConstraint::Mode::LOCK;
        on[i] = locked && !rows.count(active);
        if (locked) ++active;
      }
      LockAccess::keep(*lock, on);
    } else {
      link->SetDisabled(true);  // a relation repeating others (a closed loop of gears)
    }
  }
  if (count) {
    sys.Setup();
    sys.Update(false);
  }
  return count;
}

// Consistent accelerations and multipliers at the start. HHT takes the system's accelerations as its first old ones, and
// zeros there ring through the multipliers for hundreds of steps (a speed-driven crank's torque swung to six times its
// steady range). Solves M a = F + Cq' L, Cq a = -gamma, where gamma, the constraints' second derivative along the free
// motion (X + V t, no acceleration), comes from a central difference.
void consistent_start(ChSystem& sys) {
  ChState X, Xk;
  ChStateDelta V, A;
  double T = 0;
  sys.StateSetup(X, V, A);
  sys.StateSetup(Xk, V, A);
  sys.StateGather(X, V, T);
  const unsigned nc = sys.GetNumConstraints();
  const double vmax = V.size() ? V.lpNorm<Eigen::Infinity>() : 0;
  const double d = std::min(1e-4, 0.005 / (vmax + 1e-12));  // s: under 5 mrad or 5 mm of motion a step
  // Forward points only (drives hold their start before t = 0): C'' = (2 C0 - 5 C1 + 4 C2 - C3) / d^2 + O(d^2).
  ChVectorDynamic<> Qc(nc);
  Qc.setZero();
  const double w[4] = {2, -5, 4, -1};
  for (int k = 0; k < 4; ++k) {
    ChStateDelta Dx = V * (k * d);
    sys.StateIncrementX(Xk, X, Dx);
    sys.StateScatter(Xk, V, T + k * d, true);
    sys.LoadConstraint_C(Qc, w[k] / (d * d), false);
  }
  sys.StateScatter(X, V, T, true);
  ChVectorDynamic<> R(sys.GetNumCoordsVelLevel()), L(nc);
  R.setZero();
  L.setZero();
  sys.LoadResidual_F(R, 1.0);
  if (!sys.StateSolveCorrection(A, L, R, Qc, 1.0, 0, 0, X, V, T, false, false, true)) return;
  if (!A.allFinite() || !L.allFinite()) return;
  sys.StateScatterAcceleration(A);
  sys.StateScatterReactions(L);
}

class CoordForces : public ChPhysicsItem {
 public:
  struct Wrench {
    ChBodyAuxRef* body;
    ChVector3d force, point, torque;  // world: a force at a point, and a torque
  };
  std::function<void(double, std::vector<Wrench>&)> eval;

  CoordForces* Clone() const override { return new CoordForces(*this); }
  void IntLoadResidual_F(const unsigned int off, ChVectorDynamic<>& R, const double c) override {
    std::vector<Wrench> ws;
    eval(GetChTime(), ws);
    const unsigned int displ = off - GetOffset_w();
    for (const auto& w : ws) {
      if (w.body->IsFixed()) continue;
      const unsigned int o = displ + w.body->GetOffset_w();
      const ChVector3d t = (w.point - w.body->GetPos()).Cross(w.force) + w.torque;
      const ChVector3d tl = w.body->TransformDirectionParentToLocal(t);
      for (int i = 0; i < 3; ++i) R(o + i) += c * w.force[i], R(o + 3 + i) += c * tl[i];
    }
  }
};

// ---------------------------------------------------------------- the model
struct PartBody {
  std::string node;
  std::shared_ptr<ChBodyAuxRef> body;
  PartMass mass;
  bool fixed = false;
};

struct JointModel {
  const Joint* src = nullptr;
  const JointKind* k = nullptr;
  int base = -1, part = -1;  // PartBody indices; -1 = the world (the ground body)
  Mat4 fa, fb;               // joint frames in each body's reference frame (mm)
  std::shared_ptr<std::vector<double>> ref = std::make_shared<std::vector<double>>();  // unwrap reference (rad, mm), moved on every step
  std::shared_ptr<ChLinkLock> link;
  std::shared_ptr<CoordConstraint> motor;   // position / speed drive
  std::string drive_mode;                   // position | speed | torque | force
  size_t drive_coord = 0;
  std::function<double(double)> drive;      // t -> value (deg, mm, N.mm... per mode, OPAD units)
  json spring;                              // {"coordinate", "stiffness", "damping", "rest"}
  double friction = 0;
  // Limits as one-sided springs on the coordinate (rad, mm; NaN: none), stiff for the part they stop and nearly
  // critically damped: Chrono's own ChLinkLock limits need its iterative solver and started the part off its value.
  std::vector<std::pair<double, double>> limits;
  std::vector<double> limit_k, limit_c;
};

ChBodyAuxRef* body_at(std::vector<PartBody>& parts, ChBodyAuxRef* ground, int i) { return i < 0 ? ground : parts[size_t(i)].body.get(); }

// Triangles of a part's bodies in the part's reference frame (m), for collision.
std::shared_ptr<ChTriangleMeshConnected> part_mesh(const Document& doc, const Scene& scene, const std::string& node, const Mat4& ref, double deflection) {
  auto mesh = chrono_types::make_shared<ChTriangleMeshConnected>();
  const Node* n = scene.node(node);
  const std::vector<std::string> bodies = n->kind == Node::Kind::Body ? std::vector<std::string>{node} : scene.bodies_under(node);
  const Mat4 to_part = ref.inverse();
  for (const auto& b : bodies) {
    const Node* bn = scene.node(b);
    if (!bn || bn->body_missing) continue;
    TopoDS_Shape shape = node_world_shape(doc, scene, b);
    BRepMesh_IncrementalMesh(shape, deflection, false, 0.35, true);
    for (TopExp_Explorer e(shape, TopAbs_FACE); e.More(); e.Next()) {
      const TopoDS_Face f = TopoDS::Face(e.Current());
      TopLoc_Location loc;
      const Handle(Poly_Triangulation) tri = BRep_Tool::Triangulation(f, loc);
      if (tri.IsNull()) continue;
      const gp_Trsf t = loc.Transformation();
      const bool reversed = f.Orientation() == TopAbs_REVERSED;
      for (int i = 1; i <= tri->NbTriangles(); ++i) {
        int a, bb, c;
        tri->Triangle(i).Get(a, bb, c);
        if (reversed) std::swap(bb, c);
        ChVector3d v[3];
        int idx[3] = {a, bb, c};
        for (int k = 0; k < 3; ++k) {
          const gp_Pnt p = tri->Node(idx[k]).Transformed(t);
          const Vec3 l = to_part.apply({p.X(), p.Y(), p.Z()});
          v[k] = ChVector3d(l[0] * kMm, l[1] * kMm, l[2] * kMm);
        }
        mesh->AddTriangle(v[0], v[1], v[2]);
      }
    }
  }
  return mesh;
}

design::ParamTable params_of(const Scene& s) {
  std::vector<design::ParamDef> defs;
  for (const auto& p : s.params) defs.push_back({p.id, p.name, p.expr, p.comment});
  return design::ParamTable(defs);
}

}  // namespace

StudyRun run_dynamic(const Document& doc, const Scene& scene0, const json& st, const Progress& progress) {
  StudyRun run;
  run.kind = "dynamic";
  const double duration = st.value("duration", 1.0);
  if (!(duration > 0) || duration > 3600) throw Error("a dynamic study's duration is a positive number of seconds (at most an hour)");
  int frames = std::clamp(st.value("frames", 101), 2, 20001);
  double step = st.value("step", 0.0);
  if (step <= 0) {
    // At most 1 ms, 1/2000 of the study, and a hundredth of a radian per step at the fastest speed drive.
    step = std::min(1e-3, duration / 2000);
    auto fastest = [&](const json& d) {
      if (!d.is_object() || d.value("mode", d.contains("speed") ? "speed" : "position") != "speed") return;
      const double v = std::fabs(d.value("value", d.value("speed", 0.0)));  // deg/s or mm/s
      if (v > 0) step = std::min(step, 0.01 / (v * kPi / 180));
    };
    for (const auto& j : scene0.joints) fastest(j.def.value("drive", json()));
    for (const auto& d : st.value("drivers", json::array())) fastest(d);
    step = std::max(step, 1e-6);
  }
  const int sub = std::max(1, int(std::ceil(duration / (frames - 1) / step)));
  step = duration / (frames - 1) / sub;
  if ((frames - 1) * sub > 20000000) throw Error("a dynamic study takes at most 20 million steps: lengthen the step or shorten the duration");

  // Start from a state that meets every joint.
  Mechanism mech(scene0);
  for (const auto& p : mech.problems()) run.warnings.push_back(p);
  if (const auto r = mech.settle(); !r.ok) throw Error("the joints are not met at the start and cannot be: " + r.error);
  const Scene scene = mech.posed(scene0);
  const int redundant = mech.analysis().value("redundant", 0);

  ChSystemNSC sys;
  const json g = st.value("gravity", json(true));
  ChVector3d gravity(0, 0, 0);
  if (g == true) gravity = ChVector3d(0, 0, -9.80665);
  else if (g.is_array() && g.size() == 3) gravity = ChVector3d(g[0].get<double>() * kMm, g[1].get<double>() * kMm, g[2].get<double>() * kMm);
  sys.SetGravitationalAcceleration(gravity);

  // ---- bodies: the mechanism's parts, plus bodies set free; everything else is the ground
  std::vector<std::string> nodes = mech.parts();
  for (const auto& f : st.value("free", json::array())) {
    const std::string id = f.get<std::string>();
    if (!scene.node(id)) throw Error("free: no body or component " + id);
    if (std::find(nodes.begin(), nodes.end(), id) == nodes.end()) nodes.push_back(id);
  }
  if (nodes.empty()) throw Error("a dynamic study needs joints between parts (or parts set free in settings.free)");
  auto ground = chrono_types::make_shared<ChBodyAuxRef>();
  ground->SetFixed(true);
  sys.AddBody(ground);
  std::set<std::string> grounded;
  for (const auto& j : scene.joints)
    if ((j.kind == "ground" || (j.kind == "rigid" && j.base.empty())) && j.error.empty()) grounded.insert(j.part);
  std::vector<PartBody> parts;
  std::map<std::string, int> part_at;
  for (const auto& id : nodes) {
    PartBody p;
    p.node = id;
    p.fixed = grounded.count(id) > 0;
    p.body = chrono_types::make_shared<ChBodyAuxRef>();
    const Mat4 ref = scene.world(id);
    p.body->SetFrameRefToAbs(frame_m(ref));
    if (!p.fixed) {
      p.mass = part_mass(doc, scene, id);
      for (const auto& n : p.mass.notes)
        if (std::find(run.warnings.begin(), run.warnings.end(), n) == run.warnings.end()) run.warnings.push_back(n);
      p.body->SetMass(p.mass.mass);
      // Centre and inertia in the part's own axes.
      const Mat4 inv = ref.inverse();
      const Vec3 c = inv.apply(p.mass.centre);
      p.body->SetFrameCOMToRef(ChFramed(to_m(c), QUNIT));
      ChMatrix33<> Iw, R;
      for (int r = 0; r < 3; ++r)
        for (int k = 0; k < 3; ++k) Iw(r, k) = p.mass.inertia[size_t(r * 3 + k)] * 1e-6, R(r, k) = ref.at(r, k);
      const ChMatrix33<> Il = R.transpose() * Iw * R;
      p.body->SetInertia(Il);
    } else {
      p.body->SetFixed(true);
    }
    sys.AddBody(p.body);
    part_at[id] = int(parts.size());
    parts.push_back(std::move(p));
  }

  // ---- joints
  const design::ParamTable params = params_of(scene);
  std::map<std::string, std::vector<json>> overrides;  // study drivers by joint
  for (const auto& d : st.value("drivers", json::array())) overrides[d.at("joint").get<std::string>()].push_back(d);
  std::vector<JointModel> joints;
  std::map<std::string, size_t> joint_at;
  bool unilateral = false;
  for (const auto& j : scene.joints) {
    const JointKind* k = joint_kind(j.kind);
    if (!k || k->relation || !j.error.empty()) continue;
    if (!part_at.count(j.part)) continue;
    JointModel m;
    m.src = &j;
    m.k = k;
    m.part = part_at[j.part];
    m.base = j.base.empty() ? -1 : part_at.count(j.base) ? part_at[j.base] : -1;
    const Mat4 base_ref = m.base < 0 ? Mat4() : scene.world(j.base);
    // A base that is not a part (a body the mechanism only references) is the ground: its frame goes into the world.
    m.fa = (j.base.empty() || m.base >= 0) ? mat_of_frame(j.at_base) : scene.world(j.base) * mat_of_frame(j.at_base);
    (void)base_ref;
    m.fb = mat_of_frame(j.at_part);
    for (size_t c = 0; c < k->coords.size(); ++c) m.ref->push_back(j.values[c] * (k->coords[c].angle ? kPi / 180 : 1.0));
    ChBodyAuxRef* pb = parts[size_t(m.part)].body.get();
    ChBodyAuxRef* bb = body_at(parts, ground.get(), m.base);
    const Mat4 wa = ref_mm(bb) * m.fa, wb = ref_mm(pb) * m.fb;
    if (j.kind != "ground" && !(parts[size_t(m.part)].fixed && (m.base < 0 || parts[size_t(m.base)].fixed))) {
      std::shared_ptr<ChLinkLock> link;
      if (j.kind == "rigid" || j.def.value("locked", false)) link = chrono_types::make_shared<ChLinkLockLock>();
      else if (j.kind == "revolute") link = chrono_types::make_shared<ChLinkLockRevolute>();
      else if (j.kind == "slider") link = chrono_types::make_shared<ChLinkLockPrismatic>();
      else if (j.kind == "cylindrical" || j.kind == "screw") link = chrono_types::make_shared<ChLinkLockCylindrical>();
      else if (j.kind == "pin_slot") link = chrono_types::make_shared<ChLinkLockRevolutePrismatic>();
      else if (j.kind == "planar") link = chrono_types::make_shared<ChLinkLockPlanar>();
      else if (j.kind == "ball") link = chrono_types::make_shared<ChLinkLockSpherical>();
      if (link) {
        link->Initialize(parts[size_t(m.part)].body, m.base < 0 ? std::static_pointer_cast<ChBody>(ground) : std::static_pointer_cast<ChBody>(parts[size_t(m.base)].body), false,
                         frame_m(wb), frame_m(wa));
        sys.AddLink(link);
        m.link = link;
      }
    }
    // The screw joint's thread: translation = pitch x rotation / 360.
    auto coord_constraint = [&](std::function<double(const std::vector<double>&, double)> f, std::function<double(double)> rate) {
      auto c = chrono_types::make_shared<CoordConstraint>();
      c->bodies = {pb};
      if (bb != ground.get()) c->bodies.push_back(bb);
      const std::string kind = j.kind;
      const Mat4 fa = m.fa, fb = m.fb;
      const bool has_base = bb != ground.get();
      const Mat4 ground_ref = ref_mm(ground.get());
      auto ref = m.ref;
      c->residual = [kind, fa, fb, has_base, ground_ref, f, ref](const std::vector<Mat4>& fr, double t) {
        const Mat4 a = (has_base ? fr[1] : ground_ref) * fa;
        const Mat4 b = fr[0] * fb;
        return f(joint_coordinates(kind, a, b, *ref), t);
      };
      c->rate = rate;
      c->Setup();
      sys.AddLink(c);
      return c;
    };
    if (j.kind == "screw" && m.link) {
      const double pitch = j.def.value("pitch", 0.0) * kMm;
      // The cylindrical joint's frames give rotation and translation; the thread ties them (translation is measured
      // along the axis in mm by joint_coordinates: back to m).
      auto c = chrono_types::make_shared<CoordConstraint>();
      c->bodies = {pb};
      if (bb != ground.get()) c->bodies.push_back(bb);
      const Mat4 fa = m.fa, fb = m.fb;
      const bool has_base = bb != ground.get();
      const Mat4 ground_ref = ref_mm(ground.get());
      auto ref = m.ref;
      c->residual = [fa, fb, has_base, ground_ref, pitch, ref](const std::vector<Mat4>& fr, double) {
        const Mat4 a = (has_base ? fr[1] : ground_ref) * fa;
        const Mat4 b = fr[0] * fb;
        const auto q = joint_coordinates("cylindrical", a, b, {ref->empty() ? 0.0 : (*ref)[0], 0});
        return q[1] * kMm - pitch * q[0] / (2 * kPi);
      };
      c->Setup();
      sys.AddLink(c);
    }
    // Drives: the study's drivers first, else the joint's own.
    json drive = j.def.value("drive", json());
    if (overrides.count(j.id)) drive = overrides[j.id].front();
    if (drive.is_object() && !j.def.value("locked", false)) {
      m.drive_mode = drive.value("mode", drive.contains("to") || drive.contains("table") ? "position" : drive.contains("speed") ? "speed" : "position");
      if (drive.contains("coordinate")) {
        const int ci = coord_index(*k, drive["coordinate"].get<std::string>());
        if (ci < 0) throw Error("\"" + j.name + "\" has no coordinate " + drive["coordinate"].dump() + " to drive");
        m.drive_coord = size_t(ci);
      }
      if (k->coords.empty()) throw Error("\"" + j.name + "\" (" + j.kind + ") has no coordinate to drive");
      const bool angle = k->coords[m.drive_coord].angle;
      const double start = j.values[m.drive_coord];
      std::function<double(double)> f;
      if (drive.contains("expr")) {
        const std::string e = drive["expr"].get<std::string>();
        const bool angular_value = (m.drive_mode == "position" || m.drive_mode == "speed") && angle;
        const design::Dim dim = angular_value ? design::Dim::Angle : (m.drive_mode == "position" || m.drive_mode == "speed") ? design::Dim::Length : design::Dim::None;
        const double unit = angular_value ? 180 / kPi : 1.0;
        params.as_with(dim, e, "t", design::Quantity{0});
        f = [params, e, dim, unit](double t) { return params.as_with(dim, e, "t", design::Quantity{t}) * unit; };
      } else if (drive.contains("table")) {
        std::vector<std::pair<double, double>> pts;
        for (const auto& p : drive["table"]) pts.push_back({p.at(0).get<double>(), p.at(1).get<double>()});
        std::sort(pts.begin(), pts.end());
        f = [pts](double t) {
          if (t <= pts.front().first) return pts.front().second;
          for (size_t i = 1; i < pts.size(); ++i)
            if (t <= pts[i].first) return pts[i - 1].second + (t - pts[i - 1].first) / std::max(1e-12, pts[i].first - pts[i - 1].first) * (pts[i].second - pts[i - 1].second);
          return pts.back().second;
        };
      } else if (drive.contains("to")) {
        const double from = drive.value("from", start), to = drive["to"].get<double>();
        f = [from, to, duration](double t) {
          const double x = std::clamp(t / duration, 0.0, 1.0);
          return from + (to - from) * x * x * (3 - 2 * x);  // a position drive starts and stops gently
        };
      } else {
        const double v = drive.value("value", drive.value("speed", 0.0));
        f = [v](double) { return v; };
      }
      m.drive = f;
      if (m.drive_mode == "position" || m.drive_mode == "speed") {
        const size_t ci = m.drive_coord;
        const double unit = angle ? kPi / 180 : kMm;  // OPAD units to Chrono's
        std::function<double(double)> target;
        if (m.drive_mode == "position") {
          target = [f, unit](double t) { return f(t) * unit; };
        } else {
          // The position a speed drive reaches: its integral from the start, tabulated on a fine grid.
          const int n = 4000;
          auto table = std::make_shared<std::vector<double>>(n + 1, 0.0);
          const double dt = duration / n;
          for (int i = 1; i <= n; ++i) (*table)[size_t(i)] = (*table)[size_t(i - 1)] + 0.5 * (f((i - 1) * dt) + f(i * dt)) * dt;
          const double s0 = start;
          target = [table, dt, n, s0, unit, f, duration](double t) {
            const double x = std::clamp(t / dt, 0.0, double(n));
            const int i = std::min(int(x), n - 1);
            double v = (*table)[size_t(i)] + (x - i) * ((*table)[size_t(i + 1)] - (*table)[size_t(i)]);
            if (t > duration) v = (*table)[size_t(n)] + f(duration) * (t - duration);
            return (s0 + v) * unit;
          };
        }
        const double ue = angle ? 1.0 : kMm;  // coordinates come in rad and mm
        m.motor = coord_constraint([ci, target, ue](const std::vector<double>& q, double t) { return q[ci] * ue - target(t); },
                                   [target](double t) {
                                     // One-sided at the start: the target holds still before t = 0 (a central difference
                                     // there gave half the speed, and Chrono's velocity assembly halved the start).
                                     const double a = std::max(t - 1e-6, 0.0), b = t + 1e-6;
                                     return -(target(b) - target(a)) / (b - a);
                                   });
      }
    }
    if (const auto lim = j.def.find("limits"); lim != j.def.end() && lim->is_object() && !j.def.value("locked", false) && j.kind != "rigid") {
      m.limits.assign(k->coords.size(), {NAN, NAN});
      m.limit_k.assign(k->coords.size(), 0.0);
      m.limit_c.assign(k->coords.size(), 0.0);
      // What the limit stops: the part's mass, or its inertia about the joint's axis (kg, kg.m2).
      const PartMass& pm = parts[size_t(m.part)].mass;
      const Mat4 wa = ref_mm(body_at(parts, ground.get(), m.base)) * m.fa;
      const Vec3 ax{wa.at(0, 2), wa.at(1, 2), wa.at(2, 2)}, o{wa.at(0, 3), wa.at(1, 3), wa.at(2, 3)};
      double Iaxis = 0;
      for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c) Iaxis += ax[size_t(r)] * pm.inertia[size_t(r * 3 + c)] * ax[size_t(c)];
      Vec3 dvec{pm.centre[0] - o[0], pm.centre[1] - o[1], pm.centre[2] - o[2]};
      const double along = dvec[0] * ax[0] + dvec[1] * ax[1] + dvec[2] * ax[2];
      const double d2 = dvec[0] * dvec[0] + dvec[1] * dvec[1] + dvec[2] * dvec[2] - along * along;
      Iaxis = (Iaxis + pm.mass * d2) * 1e-6;  // kg.m2
      const double w = 0.5 / step;           // the stop's own frequency: stiff, and stable at this step
      for (const auto& [cname, v] : lim->items()) {
        const int ci = coord_index(*k, cname);
        if (ci < 0 || !v.is_array() || v.size() != 2) continue;
        const bool ang = k->coords[size_t(ci)].angle;
        const double unit = ang ? kPi / 180 : 1.0;  // the coordinates come in rad and mm
        m.limits[size_t(ci)] = {v[0].get<double>() * unit, v[1].get<double>() * unit};
        const double inertia = ang ? std::max(Iaxis, 1e-12) : std::max(pm.mass, 1e-9) * 1e-3;  // per rad: kg.m2; per mm: kg.m/mm
        m.limit_k[size_t(ci)] = inertia * w * w;
        m.limit_c[size_t(ci)] = 1.8 * inertia * w;
      }
    }
    if (j.def.contains("spring") && j.def["spring"].is_object()) m.spring = j.def["spring"];
    m.friction = j.def.value("friction", 0.0);
    joint_at[j.id] = joints.size();
    joints.push_back(std::move(m));
  }

  // ---- relations: q2 - q2c = ratio (q1 - q1c), in the joints' own coordinates
  struct RelationModel {
    const Joint* src;
    std::shared_ptr<CoordConstraint> c;
    bool angular;
  };
  std::vector<RelationModel> relations;
  std::vector<std::function<void()>> carrier_updates;  // unwrap references of carrier-relative relations, moved every step
  for (const auto& j : scene.joints) {
    if (!is_relation(j.kind) || !j.error.empty() || j.joints.size() != 2) continue;
    if (!joint_at.count(j.joints[0]) || !joint_at.count(j.joints[1])) continue;
    const JointModel& a = joints[joint_at[j.joints[0]]];
    const JointModel& b = joints[joint_at[j.joints[1]]];
    const auto [n1, n2] = relation_coords(j.kind);
    const size_t c1 = size_t(coord_index(*a.k, n1)), c2 = size_t(coord_index(*b.k, n2));
    double ratio = 0;
    if (j.kind == "gear") ratio = j.def.value("ratio", -1.0);
    else if (j.kind == "rack_pinion") ratio = j.def.value("radius", 0.0) * (j.def.value("reverse", false) ? -1.0 : 1.0);
    else ratio = j.def.value("lead", 0.0) / (2 * kPi);
    const json at = j.def.value("values", json::array({0, 0}));
    const double q1c = at[0].get<double>() * (a.k->coords[c1].angle ? kPi / 180 : 1.0);
    const double q2c = at[1].get<double>() * (b.k->coords[c2].angle ? kPi / 180 : 1.0);
    // Bodies: the parts and bases of both joints (no duplicates, the ground left out).
    auto c = chrono_types::make_shared<CoordConstraint>();
    std::vector<ChBodyAuxRef*> bs;
    auto add = [&](ChBodyAuxRef* x) {
      if (x != ground.get() && std::find(bs.begin(), bs.end(), x) == bs.end()) bs.push_back(x);
    };
    ChBodyAuxRef* a_part = parts[size_t(a.part)].body.get();
    ChBodyAuxRef* a_base = body_at(parts, ground.get(), a.base);
    ChBodyAuxRef* b_part = parts[size_t(b.part)].body.get();
    ChBodyAuxRef* b_base = body_at(parts, ground.get(), b.base);
    for (auto* x : {a_part, a_base, b_part, b_base}) add(x);
    // A carrier (planet gears): each joint's part frame is read against where it sat in the carrier.
    ChBodyAuxRef* carrier = nullptr;
    Mat4 fc1, fc2;
    if (j.def.contains("carrier") && j.def["carrier"].is_string()) {
      const auto it = part_at.find(j.def["carrier"].get<std::string>());
      if (it == part_at.end()) throw Error("\"" + j.name + "\": its carrier is not a part of the study");
      carrier = parts[size_t(it->second)].body.get();
      add(carrier);
      const json cf = j.def.value("carrier_frames", json::array());
      if (cf.size() == 2) fc1 = mat_of_frame(Frame::from_json(cf[0])), fc2 = mat_of_frame(Frame::from_json(cf[1]));
    }
    c->bodies = bs;
    auto index = [bs](ChBodyAuxRef* x) { return int(std::find(bs.begin(), bs.end(), x) - bs.begin()); };
    const int ia = index(a_part), iab = index(a_base), ib = index(b_part), ibb = index(b_base);
    const Mat4 ground_ref = ref_mm(ground.get());
    const std::string ka = a.k->kind, kb = b.k->kind;
    const Mat4 fa1 = a.fa, fb1 = a.fb, fa2 = b.fa, fb2 = b.fb;
    auto refa = a.ref;
    auto refb = b.ref;
    const bool angular = b.k->coords[c2].angle;
    const double w = angular ? 1.0 : kMm;
    const int icar = carrier ? index(carrier) : -1;
    const bool ang1 = a.k->coords[c1].angle, ang2 = b.k->coords[c2].angle;
    auto cref = std::make_shared<std::array<double, 2>>(std::array<double, 2>{0, 0});
    // The carrier-relative coordinate of side 0 or 1 (radians or mm), unwrapped about cref.
    auto carried = [=](const std::vector<Mat4>& fr, int side) {
      auto at = [&](int i) { return i < int(fr.size()) ? fr[size_t(i)] : ground_ref; };
      const Mat4 P = side == 0 ? at(ia) * fb1 : at(ib) * fb2;
      const Mat4 C = at(icar) * (side == 0 ? fc1 : fc2);
      const Mat4 R = C.inverse() * P;
      if (!(side == 0 ? ang1 : ang2)) return R.at(2, 3);
      const double m = std::atan2(R.at(1, 0), R.at(0, 0)), r = (*cref)[size_t(side)];
      return r + std::remainder(m - r, 2 * kPi);
    };
    c->residual = [=](const std::vector<Mat4>& fr, double) {
      auto at = [&](int i) { return i < int(fr.size()) ? fr[size_t(i)] : ground_ref; };
      const double q1 = icar >= 0 ? carried(fr, 0) : joint_coordinates(ka, at(iab) * fa1, at(ia) * fb1, *refa)[c1];
      const double q2 = icar >= 0 ? carried(fr, 1) : joint_coordinates(kb, at(ibb) * fa2, at(ib) * fb2, *refb)[c2];
      return (q2 - q2c - ratio * (q1 - q1c)) * w;
    };
    if (icar >= 0) carrier_updates.push_back([c, carried, cref] {
        std::vector<Mat4> fr;
        for (auto* b : c->bodies) fr.push_back(ref_mm(b));
        *cref = {carried(fr, 0), carried(fr, 1)};
      });
    c->Setup();
    sys.AddLink(c);
    relations.push_back({&j, c, angular});
  }

  // ---- contacts
  const json contacts = st.value("contacts", json(false));
  std::vector<int> touching;  // part indices taking part (-1: the ground's bodies)
  if (contacts == true || contacts == "all") {
    for (size_t i = 0; i < parts.size(); ++i) touching.push_back(int(i));
  } else if (contacts.is_array()) {
    for (const auto& c : contacts) {
      const std::string id = c.get<std::string>();
      if (!part_at.count(id)) throw Error("contacts: " + id + " is not a part of the study");
      touching.push_back(part_at[id]);
    }
  }
  if (!touching.empty()) {
    if (touching.size() > 14) throw Error("contacts: at most 14 parts can touch in one study: list the ones that do");
    sys.SetCollisionSystemType(ChCollisionSystem::Type::BULLET);
    // Size of the model for the collision envelope.
    double size = 0;
    for (const auto& p : parts) {
      const Bnd_Box b = node_world_bbox(doc, scene, p.node);
      if (!b.IsVoid()) size = std::max(size, std::sqrt(b.SquareExtent()));
    }
    size = std::max(size, 10.0) * kMm;
    ChCollisionModel::SetDefaultSuggestedEnvelope(st.value("envelope", 0.004 * size / kMm) * kMm);
    ChCollisionModel::SetDefaultSuggestedMargin(0.002 * size);
    auto mat = chrono_types::make_shared<ChContactMaterialNSC>();
    mat->SetFriction(float(st.value("friction", 0.3)));
    mat->SetRestitution(float(st.value("restitution", 0.0)));
    const double deflection = st.value("mesh_deflection", std::max(0.01, 0.002 * size / kMm));
    // Jointed parts do not collide: each part its own family, its joined parts' families masked.
    std::map<int, int> family;
    for (size_t i = 0; i < touching.size(); ++i) family[touching[i]] = int(i) + 1;
    for (int i : touching) {
      PartBody& p = parts[size_t(i)];
      auto mesh = part_mesh(doc, scene, p.node, scene.world(p.node), deflection);
      if (mesh->GetNumTriangles() == 0) continue;
      auto shape = chrono_types::make_shared<ChCollisionShapeTriangleMesh>(mat, mesh, p.fixed, false, 0.0005 * size);
      p.body->AddCollisionShape(shape, ChFramed());
      p.body->EnableCollision(true);
    }
    // The ground: every body that is not a part.
    std::set<std::string> moving;
    for (const auto& p : parts)
      for (const auto& b : scene.node(p.node)->kind == Node::Kind::Body ? std::vector<std::string>{p.node} : scene.bodies_under(p.node)) moving.insert(b);
    bool any_ground = false;
    for (const auto& b : scene.all_bodies()) {
      if (moving.count(b) || !scene.effectively_visible(b)) continue;
      const Node* bn = scene.node(b);
      if (!bn || bn->body_missing || bn->representation != "solid") continue;
      auto mesh = part_mesh(doc, scene, b, Mat4(), deflection);
      if (mesh->GetNumTriangles() == 0) continue;
      ground->AddCollisionShape(chrono_types::make_shared<ChCollisionShapeTriangleMesh>(mat, mesh, true, false, 0.0005 * size), ChFramed());
      any_ground = true;
    }
    if (any_ground) ground->EnableCollision(true);
    for (int i : touching) {
      auto model = parts[size_t(i)].body->GetCollisionModel();
      if (!model) continue;
      model->SetFamily(family[i]);
      for (const auto& jm : joints) {
        int other = jm.part == i ? jm.base : jm.base == i ? jm.part : -2;
        if (other == -1) model->DisallowCollisionsWith(0);  // joined to the world: not to the ground's bodies
        else if (other >= 0 && family.count(other)) model->DisallowCollisionsWith(family[other]);
      }
    }
    if (ground->GetCollisionModel()) ground->GetCollisionModel()->SetFamily(0);
    unilateral = true;
  }

  // ---- solver
  if (unilateral) {
    auto solver = chrono_types::make_shared<ChSolverAPGD>();
    solver->SetMaxIterations(st.value("iterations", 300));
    solver->SetTolerance(1e-10);
    sys.SetSolver(solver);
    sys.SetMaxPenetrationRecoverySpeed(st.value("recovery_speed", 0.05));
  } else {
    // A mechanism with redundant equations (a planar loop of spatial joints) makes a rank-deficient system: a direct QR
    // returned garbage multipliers for it (an engine's motor torque 1000 times too large), MINRES gives the least-norm
    // ones, so every well-determined force (a motor's, a load's) comes out right and the indeterminate ones evenly shared.
    std::shared_ptr<ChSolver> solver;
    if (redundant > 0) {
      auto minres = chrono_types::make_shared<ChSolverMINRES>();
      minres->SetMaxIterations(st.value("iterations", 1000));
      minres->SetTolerance(1e-12);
      minres->EnableDiagonalPreconditioner(true);
      minres->EnableWarmStart(true);
      solver = minres;
      run.warnings.push_back(std::to_string(redundant) + " redundant joint equation(s): the reactions they share are statically indeterminate (least-norm shares reported)");
    } else {
      solver = chrono_types::make_shared<ChSolverSparseQR>();
    }
    sys.SetSolver(solver);
    sys.SetTimestepperType(ChTimestepper::Type::HHT);
    if (auto hht = std::dynamic_pointer_cast<ChTimestepperHHT>(sys.GetTimestepper())) {
      hht->SetAlpha(st.value("hht_alpha", -0.02));
      for (const auto& link : sys.GetLinks())
        if (auto c = std::dynamic_pointer_cast<CoordConstraint>(link)) c->scale = 1 + st.value("hht_alpha", -0.02);
      hht->SetMaxIters(30);
      hht->SetAbsTolerances(1e-10, 1e-10);
      hht->SetStepControl(false);
    }
  }

  // ---- start velocities: the drives' rates at t = 0 carried through the mechanism by the kinematic solver (a short
  // look-ahead), so a speed-driven crank starts with its rod and piston moving too (Chrono's velocity assembly left them
  // still and the first step was an impulse).
  {
    // Central differences (h either side): a forward one was off by a h / 2, at an engine's top dead centre enough to
    // ring through the first hundreds of steps.
    Values ahead, behind;
    const double h = 1e-5;
    for (const auto& jm : joints) {
      if (!jm.drive || (jm.drive_mode != "speed" && jm.drive_mode != "position")) continue;
      const double rate = jm.drive_mode == "speed" ? jm.drive(0) : (jm.drive(h) - jm.drive(0)) / h;  // deg/s or mm/s
      if (rate == 0) continue;
      std::vector<double> v(jm.src->values.size(), NAN);
      v[jm.drive_coord] = jm.src->values[jm.drive_coord] + rate * h;
      ahead[jm.src->id] = v;
      v[jm.drive_coord] = jm.src->values[jm.drive_coord] - rate * h;
      behind[jm.src->id] = v;
    }
    if (!ahead.empty()) {
      Mechanism later(scene), earlier(scene);
      if (later.drive(ahead).ok && earlier.drive(behind).ok)
        for (auto& p : parts) {
          if (p.fixed || !later.has_part(p.node)) continue;
          const Mat4 a = scene.world(p.node), b = later.part_world(p.node), e = earlier.part_world(p.node);
          const Vec3 local = a.inverse().apply(p.mass.centre);
          const Vec3 c1 = b.apply(local), cm = e.apply(local);
          p.body->SetPosDt(ChVector3d((c1[0] - cm[0]) / (2 * h) * kMm, (c1[1] - cm[1]) / (2 * h) * kMm, (c1[2] - cm[2]) / (2 * h) * kMm));
          // Angular velocity from the turn between the two placements (R+ R-^T), small: its axial vector over 2h.
          ChMatrix33<> R;
          for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 3; ++c) {
              double v = 0;
              for (int k = 0; k < 3; ++k) v += b.at(r, k) * e.at(c, k);
              R(r, c) = v;
            }
          p.body->SetAngVelParent(ChVector3d(R(2, 1) - R(1, 2), R(0, 2) - R(2, 0), R(1, 0) - R(0, 1)) / (4 * h));
        }
    }
  }

  // ---- outputs
  struct Recorder {
    std::vector<double> v;
  };
  std::map<std::string, std::vector<double>> rec;  // series name -> values
  std::vector<Series> templ;
  auto series = [&](const std::string& id, const std::string& name, const std::string& unit, const std::string& group) -> std::vector<double>& {
    if (!rec.count(name)) templ.push_back({id, name, unit, group, {}});
    return rec[name];
  };
  run.parts = nodes;
  // Traces.
  struct Trace {
    std::string name;
    int part;
    ChVector3d local;
  };
  std::vector<Trace> traces;
  for (const auto& tr : st.value("traces", json::array())) {
    const std::string part = tr.at("part").get<std::string>();
    if (!part_at.count(part)) throw Error("a trace's part " + part + " is not a part of the study");
    const Vec3 p = tr.at("point").get<Vec3>();
    const Vec3 l = scene.world(part).inverse().apply(p);
    const Node* n = scene.node(part);
    traces.push_back({tr.value("name", (n ? n->name : part) + " point"), part_at[part], to_m(l)});
  }

  auto joint_forces = [&](double t, std::vector<CoordForces::Wrench>& out) {
    for (auto& jm : joints) {
      const bool torque_drive = jm.drive && (jm.drive_mode == "torque" || jm.drive_mode == "force");
      if (!torque_drive && !jm.spring.is_object() && jm.friction == 0 && jm.limits.empty()) continue;
      ChBodyAuxRef* pb = parts[size_t(jm.part)].body.get();
      ChBodyAuxRef* bb = body_at(parts, ground.get(), jm.base);
      const Mat4 a = ref_mm(bb) * jm.fa, b = ref_mm(pb) * jm.fb;
      const auto q = joint_coordinates(jm.k->kind, a, b, *jm.ref);
      // Coordinate speeds from the bodies' motion: finite difference over a tiny time step.
      const double h = 1e-6;
      auto moved = [&](const ChBodyAuxRef* body, double dt) {
        ChFramed f = body->GetFrameRefToAbs();
        const ChVector3d w = body->GetAngVelParent();
        const ChVector3d com = body->GetPos();
        const ChVector3d v = body->GetPosDt();
        const double ang = w.Length() * dt;
        ChQuaterniond dq = QUNIT;
        if (ang > 0) dq = QuatFromAngleAxis(ang, w / w.Length());
        const ChVector3d pos = com + v * dt + dq.Rotate(f.GetPos() - com);
        f.SetPos(pos);
        f.SetRot(dq * f.GetRot());
        return mat_mm(f);
      };
      const auto q2 = joint_coordinates(jm.k->kind, moved(bb, h) * jm.fa, moved(pb, h) * jm.fb, *jm.ref);
      std::vector<double> qd(q.size());
      for (size_t c = 0; c < q.size(); ++c) qd[c] = (q2[c] - q[c]) / h;
      // Generalized force on each coordinate (N.m or N).
      std::vector<double> Q(q.size(), 0.0);
      if (torque_drive) Q[jm.drive_coord] += jm.drive(t) * (jm.k->coords[jm.drive_coord].angle ? kMm : 1.0);  // N.mm -> N.m; N
      if (jm.spring.is_object() && !q.empty()) {
        size_t c = 0;
        if (jm.spring.contains("coordinate")) c = size_t(std::max(0, coord_index(*jm.k, jm.spring["coordinate"].get<std::string>())));
        const bool ang = jm.k->coords[c].angle;
        const double rest = jm.spring.value("rest", 0.0) * (ang ? kPi / 180 : 1.0);
        // stiffness N/mm or N.mm/deg, damping N.s/mm or N.mm.s/deg
        const double k = jm.spring.value("stiffness", 0.0) * (ang ? 180 / kPi * kMm : 1.0);       // N/mm (per mm of q) | N.m/rad
        const double cd = jm.spring.value("damping", 0.0) * (ang ? 180 / kPi * kMm : 1.0);
        Q[c] += -k * (q[c] - rest) - cd * qd[c];
      }
      for (size_t c = 0; c < jm.limits.size() && c < q.size(); ++c) {
        const auto [lo, hi] = jm.limits[c];
        const double over = !std::isnan(hi) && q[c] > hi ? q[c] - hi : !std::isnan(lo) && q[c] < lo ? q[c] - lo : 0.0;
        if (over == 0) continue;
        // A stop pushes out and damps both ways while the coordinate is past it (it does not bounce), never pulling it in.
        double f = -jm.limit_k[c] * over - jm.limit_c[c] * qd[c];
        if (f * over > 0) f = 0;
        Q[c] += f;
      }
      if (jm.friction > 0)
        for (size_t c = 0; c < q.size(); ++c) {
          const bool ang = jm.k->coords[c].angle;
          const double v0 = ang ? 0.5 * kPi / 180 : 0.05;
          Q[c] += -jm.friction * (ang ? kMm : 1.0) * std::tanh(qd[c] / v0);
        }
      // Turn the generalized forces into a torque about the axis or a force along it, equal and opposite on the base.
      const ChFramed fa = frame_m(a), fb = frame_m(b);
      const ChVector3d ax = fa.GetRotMat().GetAxisX(), ay = fa.GetRotMat().GetAxisY(), az = fa.GetRotMat().GetAxisZ();
      for (size_t c = 0; c < q.size(); ++c) {
        if (Q[c] == 0) continue;
        const std::string& name = jm.k->coords[c].name;
        if (name == "rotation") {
          out.push_back({pb, VNULL, pb->GetPos(), az * Q[c]});
          out.push_back({bb, VNULL, bb->GetPos(), -az * Q[c]});
        } else {
          const ChVector3d dir = name == "x" || (name == "translation" && jm.k->kind == "pin_slot") ? ax : name == "y" ? ay : az;
          out.push_back({pb, dir * Q[c], fb.GetPos(), VNULL});
          out.push_back({bb, -dir * Q[c], fb.GetPos(), VNULL});
        }
      }
    }
  };

  double pe0 = 0;
  auto record = [&](double t) {
    run.t.push_back(t);
    std::vector<Mat4> pose;
    for (const auto& p : parts) pose.push_back(ref_mm(p.body.get()));
    run.poses.push_back(pose);
    double ke = 0, pe = 0;
    for (const auto& p : parts) {
      if (p.fixed) continue;
      const ChVector3d v = p.body->GetPosDt();
      const ChVector3d w = p.body->GetAngVelLocal();
      const ChMatrix33<>& I = p.body->GetInertia();
      ke += 0.5 * p.body->GetMass() * v.Length2() + 0.5 * w.Dot(I * w);
      pe += -p.body->GetMass() * gravity.Dot(p.body->GetPos());
    }
    if (run.t.size() == 1) pe0 = pe;
    series("energy", "Kinetic energy", "J", "energy").push_back(ke);
    series("energy", "Potential energy", "J", "energy").push_back(pe - pe0);
    series("energy", "Total energy", "J", "energy").push_back(ke + pe - pe0);
    for (auto& jm : joints) {
      ChBodyAuxRef* pb = parts[size_t(jm.part)].body.get();
      ChBodyAuxRef* bb = body_at(parts, ground.get(), jm.base);
      const auto q = joint_coordinates(jm.k->kind, ref_mm(bb) * jm.fa, ref_mm(pb) * jm.fb, *jm.ref);
      *jm.ref = q;
      for (size_t c = 0; c < q.size(); ++c) {
        const bool ang = jm.k->coords[c].angle;
        series(jm.src->id, jm.src->name + " " + jm.k->coords[c].name, ang ? "deg" : "mm", "value").push_back(q[c] * (ang ? 180 / kPi : 1.0));
      }
      if (jm.link) {
        const ChWrenchd r = jm.link->GetReaction2();
        series(jm.src->id, jm.src->name + " reaction force", "N", "reaction").push_back(r.force.Length());
        series(jm.src->id, jm.src->name + " reaction torque", "N.mm", "reaction").push_back(r.torque.Length() / kMm);
      }
      if (jm.motor || (jm.drive && (jm.drive_mode == "torque" || jm.drive_mode == "force"))) {
        const bool ang = jm.k->coords[jm.drive_coord].angle;
        // N.mm about the axis or N along it (the constraint's multiplier is N.m per rad or N per m)
        const double effort = jm.motor ? jm.motor->force() * (ang ? 1 / kMm : kMm) : jm.drive(t);
        series(jm.src->id, jm.src->name + (ang ? " motor torque" : " motor force"), ang ? "N.mm" : "N", "motor").push_back(effort);
      }
    }
    for (const auto& r : relations)
      series(r.src->id, r.src->name + (r.angular ? " torque" : " force"), r.angular ? "N.mm" : "N", "reaction").push_back(std::fabs(r.c->force()) * (r.angular ? 1 / kMm : kMm));
    for (const auto& tr : traces) {
      const ChVector3d p = parts[size_t(tr.part)].body->GetFrameRefToAbs().TransformPointLocalToParent(tr.local);
      series(parts[size_t(tr.part)].node, tr.name + " x", "mm", "trace").push_back(p.x() / kMm);
      series(parts[size_t(tr.part)].node, tr.name + " y", "mm", "trace").push_back(p.y() / kMm);
      series(parts[size_t(tr.part)].node, tr.name + " z", "mm", "trace").push_back(p.z() / kMm);
    }
    if (!touching.empty()) series("contacts", "Contacts", "", "contact").push_back(double(sys.GetNumContacts()));
  };

  auto forces = chrono_types::make_shared<CoordForces>();
  forces->eval = joint_forces;
  sys.AddOtherPhysicsItem(forces);
  // Positions meet the joints at the start; velocities start from the kinematic look-ahead's above, projected onto the
  // joints; accelerations and forces solve the equations of motion there (Chrono's are finite differences, their
  // multipliers impulses). With contacts the first step finds them.
  sys.DoAssembly(AssemblyLevel::POSITION | AssemblyLevel::VELOCITY);
  if (redundant > 0 && touching.empty()) {
    if (const int dropped = drop_redundant(sys)) {
      sys.SetSolver(chrono_types::make_shared<ChSolverSparseQR>());
      for (auto& w : run.warnings)
        if (w.find("redundant joint equation") != std::string::npos)
          w = std::to_string(dropped) + " redundant joint equation(s) left out: the reactions they would share are statically "
                                        "indeterminate (the other joints carry them)";
    }
  }
  if (touching.empty()) {
    consistent_start(sys);
    // HHT carries this constraint's multiplier as force / (1 + alpha) (see CoordConstraint::scale).
    for (const auto& link : sys.GetLinks())
      if (auto c = std::dynamic_pointer_cast<CoordConstraint>(link)) c->reaction /= c->scale;
  }
  record(0);
  const int total = (frames - 1) * sub;
  int done = 0;
  for (int f = 1; f < frames; ++f) {
    for (int s = 0; s < sub; ++s) {
      sys.DoStepDynamics(step);
      ++done;
      // Keep the unwrap references of the drive and relation constraints moving with the joints.
      for (auto& jm : joints) {
        ChBodyAuxRef* pb = parts[size_t(jm.part)].body.get();
        ChBodyAuxRef* bb = body_at(parts, ground.get(), jm.base);
        *jm.ref = joint_coordinates(jm.k->kind, ref_mm(bb) * jm.fa, ref_mm(pb) * jm.fb, *jm.ref);
      }
      for (auto& u : carrier_updates) u();
      if (progress && done % 200 == 0 && !progress(double(done) / total, "Simulating")) throw Error("cancelled");
    }
    record(sys.GetChTime());
    // Fail early on a blow-up rather than return nonsense.
    for (const auto& p : parts)
      if (!std::isfinite(p.body->GetPos().x()) || p.body->GetPos().Length() > 1e4)
        throw Error("the simulation blew up at t = " + std::to_string(sys.GetChTime()) + " s (a stiff spring, a step too long, or parts flung away): try a smaller step");
  }

  // Speeds of the joint values, and power of the motors.
  for (auto& s : templ) s.v = rec[s.name];
  // With contacts the start has no solved forces: those at t = 0 come from the first step.
  if (!touching.empty())
    for (auto& s : templ)
      if ((s.group == "reaction" || s.group == "motor") && s.v.size() > 1) s.v[0] = s.v[1];
  std::vector<Series> extra;
  for (const auto& s : templ) {
    if (s.group != "value") continue;
    std::vector<double> d(s.v.size(), 0.0);
    for (size_t i = 0; i < s.v.size(); ++i) {
      const size_t a = i ? i - 1 : 0, b = std::min(i + 1, s.v.size() - 1);
      d[i] = (s.v[b] - s.v[a]) / std::max(1e-15, run.t[b] - run.t[a]);
    }
    extra.push_back({s.id, s.name + " speed", s.unit + "/s", "speed", d});
  }
  for (const auto& s : templ) {
    if (s.group != "motor") continue;
    // power = effort x speed of the driven coordinate
    for (const auto& sp : extra) {
      if (sp.id != s.id) continue;
      const bool ang = sp.unit == "deg/s";
      std::vector<double> p(s.v.size());
      for (size_t i = 0; i < p.size(); ++i) p[i] = s.v[i] * sp.v[i] * (ang ? kPi / 180 * kMm : kMm);  // N.mm x deg/s, N x mm/s -> W
      extra.push_back({s.id, s.name.substr(0, s.name.rfind(' ')) + " power", "W", "motor", p});
      break;
    }
  }
  for (auto& e : extra) templ.push_back(std::move(e));
  run.series = std::move(templ);

  // ---- summary
  json joints_sum = json::object();
  for (const auto& s : run.series) {
    if (s.v.empty()) continue;
    const auto [lo, hi] = std::minmax_element(s.v.begin(), s.v.end());
    double peak = 0;
    for (double x : s.v) peak = std::max(peak, std::fabs(x));
    if (s.group == "value") joints_sum[s.name] = {{"min", *lo}, {"max", *hi}, {"end", s.v.back()}, {"unit", s.unit}};
    else if (s.group == "reaction" || s.group == "motor" || s.group == "speed") joints_sum[s.name] = {{"max", peak}, {"end", s.v.back()}, {"unit", s.unit}};
  }
  run.summary["frames"] = run.t.size();
  run.summary["duration"] = duration;
  run.summary["step"] = step;
  run.summary["outputs"] = joints_sum;
  json masses = json::object();
  for (const auto& p : parts) {
    const Node* n = scene.node(p.node);
    if (!p.fixed) masses[n ? n->name : p.node] = {{"mass_kg", p.mass.mass}, {"centre", p.mass.centre}, {"inertia_kg_mm2", p.mass.inertia}};
  }
  run.summary["parts"] = masses;
  for (const auto& s : run.series)
    if (s.name == "Total energy" && !s.v.empty()) {
      double drift = 0;
      for (double x : s.v) drift = std::max(drift, std::fabs(x - s.v.front()));
      run.summary["energy_drift_J"] = drift;
    }
  if (!run.warnings.empty()) run.summary["warnings"] = run.warnings;
  return run;
}

#endif

}  // namespace opad::sim
