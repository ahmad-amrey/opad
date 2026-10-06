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
  if (step <= 0) step = std::min(1e-3, duration / 2000);
  const int sub = std::max(1, int(std::ceil(duration / (frames - 1) / step)));
  step = duration / (frames - 1) / sub;
  if ((frames - 1) * sub > 20000000) throw Error("a dynamic study takes at most 20 million steps: lengthen the step or shorten the duration");

  // Start from a state that meets every joint.
  Mechanism mech(scene0);
  for (const auto& p : mech.problems()) run.warnings.push_back(p);
  if (const auto r = mech.settle(); !r.ok) throw Error("the joints are not met at the start and cannot be: " + r.error);
  const Scene scene = mech.posed(scene0);

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
        // Limits on the free coordinates, in the link's own measures (marker 1 relative to marker 2).
        if (const auto lim = j.def.find("limits"); lim != j.def.end() && lim->is_object() && j.kind != "rigid" && !j.def.value("locked", false)) {
          for (const auto& [c, v] : lim->items()) {
            if (!v.is_array() || v.size() != 2) continue;
            const int ci = coord_index(*k, c);
            if (ci < 0) continue;
            const bool angle = k->coords[size_t(ci)].angle;
            const double unit = angle ? kPi / 180 : kMm;
            ChLinkLimit* l = nullptr;
            if (c == "rotation") l = &link->LimitRz();
            else if (c == "translation") l = j.kind == "pin_slot" ? &link->LimitX() : &link->LimitZ();
            else if (c == "x") l = &link->LimitX();
            else if (c == "y") l = &link->LimitY();
            if (!l) continue;
            l->SetActive(true);
            l->SetMin(v[0].get<double>() * unit);
            l->SetMax(v[1].get<double>() * unit);
            unilateral = true;
          }
        }
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
                                   [target](double t) { return -(target(t + 1e-6) - target(t - 1e-6)) / 2e-6; });
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
    c->residual = [=](const std::vector<Mat4>& fr, double) {
      auto at = [&](int i) { return i < int(fr.size()) ? fr[size_t(i)] : ground_ref; };
      const double q1 = joint_coordinates(ka, at(iab) * fa1, at(ia) * fb1, *refa)[c1];
      const double q2 = joint_coordinates(kb, at(ibb) * fa2, at(ib) * fb2, *refb)[c2];
      return (q2 - q2c - ratio * (q1 - q1c)) * w;
    };
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
    auto solver = chrono_types::make_shared<ChSolverSparseQR>();
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
      if (!torque_drive && !jm.spring.is_object() && jm.friction == 0) continue;
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
  sys.DoAssembly(AssemblyLevel::FULL);
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
    if (!p.fixed) masses[n ? n->name : p.node] = {{"mass_kg", p.mass.mass}, {"centre", p.mass.centre}};
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
