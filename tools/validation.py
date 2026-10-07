#!/usr/bin/env python3
"""OPAD's simulations against published reference solutions (benchmarks), each built and run through OPAD's MCP server as
an agent would, its numbers compared with the reference and the error reported.

    python3 tools/validation.py build/linux/bin/opad-cli [--out DIR] [--only NAME[,NAME]] [--list]

Cases (each skipped when its engine is missing):

  nafems_t4      NAFEMS T4: two-dimensional heat transfer with convection. A 0.6 x 1.0 m plate, k 52 W/m.K, one edge at 100 degC,
                 two edges cooled by h 750 W/m2K to 0 degC, one insulated: 18.25 degC at E (0.6, 0.2). CalculiX.
  flat_plate     A heated aluminium plate in a uniform laminar stream (the duct of the air solved): its mean film coefficient
                 against Pohlhausen's isothermal plate, Nu = 0.664 Re^1/2 Pr^1/3 on both faces. OpenFOAM.
  cavity_1e4     Natural convection in a cubic cavity, two opposite walls held at different temperatures, the rest insulated:
  cavity_1e5     the hot wall's mean Nusselt number against Fusegi, Hyun, Kuwahara and Farouk (1991), Int. J. Heat Mass Transfer
                 34(6): 2.100 at Ra 1e4, 4.361 at Ra 1e5 (de Vahl Davis's square cavity, 1983, gives 2.243 and 4.519 in two
                 dimensions). A sealed enclosure with warm air rising: OpenFOAM and CalculiX.
  radiation_box  A heated block in a closed box in still air: radiation by rays (OPAD's view factors, the default) against
                 CalculiX's own cavity radiation on the same mesh (the view factors alone against exact ones: test_radiation).

report.json and report.md in the output folder: per case, each quantity with the reference, OPAD's value, the error and
the tolerance it is held to, and where the reference comes from.
"""
import argparse
import json
import math
import pathlib
import sys
import time
import traceback

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import sim_eval as se  # noqa: E402  the MCP client and the scenario helpers

CASES = []


def case(name, title, reference):
    def wrap(fn):
        CASES.append((name, title, reference, fn))
        return fn
    return wrap


def air(T):
    """Dry air at T degC, 1 atm: rho, mu, k, cp, nu, alpha, Pr, beta (as OPAD's sim/airflow.cpp)."""
    Tk = T + 273.15
    rho = 101325 / (287.05 * Tk)
    mu = 1.458e-6 * Tk ** 1.5 / (Tk + 110.4)
    k = 0.0241 * (Tk / 273.15) ** 0.81
    cp = 1006.0
    return {"rho": rho, "mu": mu, "k": k, "cp": cp, "nu": mu / rho, "alpha": k / (rho * cp), "Pr": mu * cp / k, "beta": 1 / Tk}


def engines(s):
    return s.run("mechanism").get("engines", {})


# ---------------------------------------------------------------------------------------------------------------- cases
@case("nafems_t4", "NAFEMS T4: 2D heat transfer with convection",
      "NAFEMS, The Standard NAFEMS Benchmarks (TNSB rev. 3, 1990), test T4: T(E) = 18.25 degC")
def nafems_t4(s):
    if not engines(s).get("thermal"):
        s.note("CalculiX is not installed: skipped")
        return
    # 600 x 1000 mm, 10 mm thick (its faces insulated: two-dimensional). AB (y = 0) at 100 degC; BC (x = 600) and CD (y = 1000)
    # to 0 degC by h 750 W/m2K; DA (x = 0) insulated.
    plate = s.box("Plate", (0, 0, 0), 600, 1000, 10, centered=False)
    s.run("load", kind="temperature", on=[se.face_rule(plate, "y", 0)], value=100, case="T4")
    s.run("load", kind="convection", on=[se.face_rule(plate, "x", 600)], h=750, ambient=0, case="T4")
    s.run("load", kind="convection", on=[se.face_rule(plate, "y", 1000)], h=750, ambient=0, case="T4")
    for size in (40, 20):
        st = s.run("study", kind="thermal", name=f"T4 {size} mm", settings={"case": "T4", "mesh_size": size, "materials": {"all": {"k": 52}}},
                   probes=[[600, 200, 5]])
        s.check(f"T at E, {size} mm elements (degC)", st["probes"][0]["temperature_C"], 18.25, 0.02 if size == 20 else 0.04)


@case("flat_plate", "Laminar flat plate in a uniform stream (air solved)",
      "Pohlhausen (1921) / Incropera, Fundamentals of Heat and Mass Transfer, eq. 7.30: Nu_L = 0.664 Re_L^1/2 Pr^1/3")
def flat_plate(s):
    if not engines(s).get("cfd"):
        s.note("OpenFOAM is not installed: skipped")
        return
    # A 60 mm (along the stream) x 60 mm x 3 mm aluminium plate making 1 W in air at 1 m/s: nearly one temperature (Bi ~ 1e-4).
    L, W, t, U, P = 60.0, 60.0, 3.0, 1.0, 1.0
    plate = s.box("Plate", (0, 0, 0), L, W, t, centered=False)
    s.material([plate], "aluminium-6061")
    s.run("load", kind="heat", on=[plate], value=P, case="Plate")
    s.run("load", kind="convection", on=[plate], h="forced", velocity=U, vector=[1, 0, 0], case="Plate")
    st = s.run("study", kind="thermal", name="Plate", settings={"case": "Plate", "air": "cfd", "ambient": 25,
                                                                "cfd": {"cell_size": 1.0, "padding": 25, "upstream": 30, "downstream": 60}})
    T = st["bodies"]["Plate"]["mean_temperature_C"]
    a = air(25 + (T - 25) / 2)
    Re = U * L * 1e-3 / a["nu"]
    h_ref = 0.664 * math.sqrt(Re) * a["Pr"] ** (1 / 3) * a["k"] / (L * 1e-3)
    A = (2 * L * W + 2 * (L + W) * t) * 1e-6  # both faces and the edges (taken at the faces' mean film)
    h = P / (A * (T - 25))
    s.note(f"Re {Re:.0f}, plate at {T:.2f} degC, {st.get('cells')} cells; edges counted at the faces' film")
    s.check("mean film coefficient (W/m2K)", h, h_ref, 0.15)


def cavity(s, Ra, Nu_ref):
    if not engines(s).get("cfd"):
        s.note("OpenFOAM is not installed: skipped")
        return
    # A 40 mm cube of air: copper walls at x = 0 (hot) and x = L (cold), 2 mm thick and held on their outsides; the four others a
    # 1 mm insulating frame (k 0.01 W/m.K: its conduction a few percent of the air's). Gravity along -Z. dT from Ra.
    L, t, f = 40.0, 2.0, 1.0
    Tm = 25.0
    a = air(Tm)
    dT = Ra * a["nu"] * a["alpha"] / (9.80665 * a["beta"] * (L * 1e-3) ** 3)
    frame = s.box("Frame", (0, -f, -f), L, L + 2 * f, L + 2 * f, centered=False)
    s.run("feature", kind="box", inputs={"plane": {"origin": [-1, 0, 0], "normal": [0, 0, 1]}, "length": L + 2, "width": L, "height": L,
                                         "centered": False, "operation": "cut", "targets": [frame]})
    hot = s.box("Hot", (-t, -f, -f), t, L + 2 * f, L + 2 * f, centered=False)
    cold = s.box("Cold", (L, -f, -f), t, L + 2 * f, L + 2 * f, centered=False)
    s.material([hot, cold], "copper")
    s.run("load", kind="temperature", on=[se.face_rule(hot, "x", -t)], value=Tm + dT / 2, case="Cavity")
    s.run("load", kind="temperature", on=[se.face_rule(cold, "x", L + t)], value=Tm - dT / 2, case="Cavity")
    st = s.run("study", kind="thermal", name=f"Cavity Ra {Ra:g}",
               settings={"case": "Cavity", "air": "cfd", "ambient": Tm, "materials": {frame: {"k": 0.01, "name": "insulation"}},
                         "cfd": {"enclosure": frame, "sealed": True, "buoyancy": True, "radiation": False, "quality": "normal",
                                 "buoyant_first": 1500, "buoyant_pass": 500}})
    Q = st["bodies"]["Hot"].get("to_air_W", float("nan"))
    Qc = st["bodies"]["Cold"].get("to_air_W", float("nan"))
    Nu = Q * L * 1e-3 / (a["k"] * dT * (L * 1e-3) ** 2)
    s.note(f"dT {dT:.2f} K, the hot wall gives the air {Q:.4f} W, the cold one takes {-Qc:.4f} W; {st.get('cells')} air cells, "
           f"{st.get('air_passes')} passes; {json.dumps(st.get('seconds'))}")
    s.check(f"hot wall's mean Nusselt number at Ra {Ra:g}", Nu, Nu_ref, 0.10)
    s.check("what the hot wall gives the cold one takes (W)", -Qc, Q, 0.05)


@case("cavity_1e4", "Natural convection in a cubic cavity, Ra 1e4",
      "Fusegi, Hyun, Kuwahara, Farouk (1991), Int. J. Heat Mass Transfer 34(6) 1543-1557: mean Nu on the hot wall 2.100")
def cavity_1e4(s):
    cavity(s, 1e4, 2.100)


@case("cavity_1e5", "Natural convection in a cubic cavity, Ra 1e5",
      "Fusegi, Hyun, Kuwahara, Farouk (1991), Int. J. Heat Mass Transfer 34(6) 1543-1557: mean Nu on the hot wall 4.361")
def cavity_1e5(s):
    cavity(s, 1e5, 4.361)


@case("radiation_box", "Radiation between a heated block and its closed box (rays against CalculiX's cavity radiation)",
      "CalculiX 2.21 cavity radiation (*RADIATE ... CR: its own view factors, the dense radiosity system) on the same mesh; "
      "the view factors themselves against Incropera table 13.2 in tests/test_radiation.cpp")
def radiation_box(s):
    if not engines(s).get("cfd"):
        s.note("OpenFOAM is not installed: skipped")
        return
    # A 20 mm copper block making 1 W in the middle of a closed 60 mm ABS box (3 mm walls) in still air, no gravity: the
    # air only conducts, so radiation (emissivity 0.9 both) carries most of the heat from the block to the box.
    box = s.box("Box", (0, 0, 0), 60, 60, 60, centered=False)
    s.run("feature", kind="box", inputs={"plane": {"origin": [3, 3, 3], "normal": [0, 0, 1]}, "length": 54, "width": 54, "height": 54,
                                         "centered": False, "operation": "cut", "targets": [box]})
    s.material([box], "abs")
    block = s.box("Block", (20, 20, 20), 20, 20, 20, centered=False)
    s.material([block], "copper")
    s.run("load", kind="heat", on=[block], value=1.0, case="Rad")
    got = {}
    for model in ("calculix", True):
        st = s.run("study", kind="thermal", name=f"Radiation {model}",
                   settings={"case": "Rad", "air": "cfd", "ambient": 25, "materials": {box: {"emissivity": 0.9}, block: {"emissivity": 0.9}},
                             "cfd": {"enclosure": box, "sealed": True, "buoyancy": False, "radiation": model, "quality": "quick"}})
        got[str(model)] = st
        s.note(f"{'rays' if model is True else model}: block {st['bodies']['Block']['max_temperature_C']:.2f} degC, radiated "
               f"{st.get('radiated_W', 0):.3f} W, to the air {st.get('to_air_W', 0):.3f} W; {json.dumps(st.get('seconds'))}")
    ref, rays = got["calculix"], got["True"]
    s.check("block's rise over the room (K)", rays["bodies"]["Block"]["max_temperature_C"] - 25, ref["bodies"]["Block"]["max_temperature_C"] - 25, 0.03)
    s.check("box's rise over the room (K)", rays["bodies"]["Box"]["mean_temperature_C"] - 25, ref["bodies"]["Box"]["mean_temperature_C"] - 25, 0.05)


# ---------------------------------------------------------------------------------------------------------------- runner
def write_markdown(report, out):
    lines = ["# OPAD validation against published references", "",
             f"{report['passed']} of {report['checks']} checks within tolerance. Each case was built and run through OPAD's MCP server.", ""]
    for c in report["cases"]:
        lines += [f"## {c['title']}", "", f"Reference: {c['reference']}", ""]
        if c["checks"]:
            lines += ["| Quantity | Reference | OPAD | Error | Tolerance | |", "|---|---|---|---|---|---|"]
            for k in c["checks"]:
                if "got" in k:
                    lines.append(f"| {k['check']} | {k['want']:.4g} | {k['got']:.4g} | {100 * k['error']:.1f} % | {100 * k['tolerance']:.0f} % | "
                                 f"{'ok' if k['ok'] else 'FAIL'} |")
                else:
                    lines.append(f"| {k['check']} | | | | | {'ok' if k['ok'] else 'FAIL'} |")
            lines.append("")
        for n in c["notes"]:
            lines.append(f"- {n}")
        if c.get("error"):
            lines.append(f"- **Error**: {c['error']}")
        lines += [f"- {c['seconds']} s", ""]
    (out / "report.md").write_text("\n".join(lines))


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("cli")
    p.add_argument("--out", default="build/validation")
    p.add_argument("--only", default="")
    p.add_argument("--list", action="store_true")
    args = p.parse_args()
    if args.list:
        for name, title, _, _ in CASES:
            print(f"{name:14} {title}")
        return 0
    out = pathlib.Path(args.out).resolve()
    out.mkdir(parents=True, exist_ok=True)
    only = set(filter(None, args.only.split(",")))
    mcp = se.Mcp(args.cli)
    report = {"cases": []}
    for name, title, reference, fn in CASES:
        if only and name not in only:
            continue
        print(f"== {name}: {title}", flush=True)
        start = time.monotonic()
        sc = se.Scenario(mcp, out, name, False)
        error = None
        try:
            fn(sc)
        except Exception as e:  # noqa: BLE001 - a case that breaks is reported, the rest run
            error = f"{type(e).__name__}: {e}"
            traceback.print_exc()
        for k in sc.checks:
            extra = f" got {k['got']:.6g} want {k['want']:.6g} err {100 * k['error']:.1f} % (tol {100 * k['tolerance']:.0f} %)" if "got" in k else ""
            print(f"  [{'ok ' if k['ok'] else 'FAIL'}] {k['check']}{extra}", flush=True)
        for n in sc.notes:
            print(f"  note: {n}", flush=True)
        if error:
            print(f"  ERROR {error}", flush=True)
        report["cases"].append({"name": name, "title": title, "reference": reference, "checks": sc.checks, "notes": sc.notes, "error": error,
                                "seconds": round(time.monotonic() - start, 1)})
    mcp.close()
    report["checks"] = sum(len(c["checks"]) for c in report["cases"])
    report["passed"] = sum(1 for c in report["cases"] for k in c["checks"] if k["ok"])
    (out / "report.json").write_text(json.dumps(report, indent=1))
    write_markdown(report, out)
    failed = report["checks"] - report["passed"] + sum(1 for c in report["cases"] if c["error"])
    print(f"\n{report['passed']} of {report['checks']} checks within tolerance; report in {out}")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
