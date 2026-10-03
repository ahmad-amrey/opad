// The units service (UI-123): what lengths, angles, areas, volumes and masses look like in every document unit and
// precision, what a value box reads back, and that a change of unit is announced. The in-app side is the units bench
// (app/CoreBench.cpp: the document switched to inches, a Distance result in inches).
#include <QCoreApplication>

#include "Units.hpp"
#include "check.hpp"

using units::Display;
using units::Kind;

static Display in(const std::string& unit, int decimals = 3) {
  Display d;
  d.length = unit;
  d.decimals = decimals;
  return d;
}

TEST(lengths_in_every_document_unit) {
  CHECK_EQ(units::format(Kind::Length, 30, -1, in("mm")), "30.000 mm");
  CHECK_EQ(units::format(Kind::Length, 30, -1, in("in")), "1.181 in");
  CHECK_EQ(units::format(Kind::Length, 30, -1, in("cm")), "3.000 cm");
  CHECK_EQ(units::format(Kind::Length, 1500, 2, in("m")), "1.50 m");
  CHECK_EQ(units::format(Kind::Length, 0.25, 0, in("um")), QString::fromUtf8("250 µm"));
  CHECK_EQ(units::format(Kind::Length, 609.6, 1, in("ft")), "2.0 ft");
  CHECK_EQ(units::number(Kind::Length, -0.0001, -1, in("mm")), "0.000");  // never "-0.000"
  CHECK_EQ(units::number(Kind::Length, -12.5, 1, in("mm")), "-12.5");
  CHECK_EQ(units::vector(Kind::Length, {25.4, 50.8, 0}, 2, in("in")), "(1.00, 2.00, 0.00) in");
  CHECK_NEAR(units::toDisplay(Kind::Length, 25.4, in("in")), 1, 1e-12);
  CHECK_NEAR(units::fromDisplay(Kind::Length, 2, in("ft")), 609.6, 1e-9);
  CHECK(units::mmPer("yd") == 0 && units::lengthUnits().size() == 6);
}

TEST(fractional_inches) {
  Display d = in("in");
  d.fraction = 64;
  CHECK_EQ(units::format(Kind::Length, 25.4 * 1.1875, -1, d), "1 3/16 in");
  CHECK_EQ(units::format(Kind::Length, 25.4 * 0.5, -1, d), "1/2 in");
  CHECK_EQ(units::format(Kind::Length, 25.4 * 2, -1, d), "2 in");
  CHECK_EQ(units::format(Kind::Length, -25.4 * 0.75, -1, d), "-3/4 in");
  CHECK_EQ(units::format(Kind::Length, 0.1, -1, d), "0 in");
  CHECK_EQ(units::format(Kind::Length, 30, -1, d), "1 3/16 in");  // 1.1811 in to the nearest 1/64, reduced
  d.length = "mm";
  CHECK_EQ(units::format(Kind::Length, 30, -1, d), "30.000 mm");  // fractions are for inches only
}

TEST(angles_areas_volumes_masses) {
  Display d = in("mm", 2);
  CHECK_EQ(units::format(Kind::Angle, 45, -1, d), QString::fromUtf8("45.00°"));
  d.radians = true;
  CHECK_EQ(units::format(Kind::Angle, 180, 4, d), "3.1416 rad");
  CHECK_EQ(units::format(Kind::Area, 600, -1, in("cm", 2)), QString::fromUtf8("6.00 cm²"));
  CHECK_EQ(units::format(Kind::Area, 645.16, -1, in("in", 2)), QString::fromUtf8("1.00 in²"));
  CHECK_EQ(units::format(Kind::Volume, 16387.064, -1, in("in", 3)), QString::fromUtf8("1.000 in³"));
  CHECK_EQ(units::format(Kind::Volume, 6000, -1, in("mm", 0)), QString::fromUtf8("6000 mm³"));
  CHECK_EQ(units::format(Kind::Mass, 250, 1, in("mm")), "250.0 g");
  CHECK_EQ(units::format(Kind::Mass, 2500, 2, in("m")), "2.50 kg");
  CHECK_EQ(units::format(Kind::Mass, 453.59237, 2, in("in")), "1.00 lb");
}

TEST(value_boxes_read_back) {
  auto near = [](std::optional<double> v, double expected) { return v && std::abs(*v - expected) < 1e-9; };
  CHECK(near(units::parse(Kind::Length, "12.5", in("mm")), 12.5));
  CHECK(near(units::parse(Kind::Length, "2", in("in")), 50.8));  // a bare number is in the display unit
  CHECK(near(units::parse(Kind::Length, "3 mm", in("in")), 3));
  CHECK(near(units::parse(Kind::Length, "1/2 in", in("mm")), 12.7));
  CHECK(near(units::parse(Kind::Length, "1 1/2\"", in("mm")), 38.1));
  CHECK(near(units::parse(Kind::Length, "1 3/16", in("in")), 30.1625));
  CHECK(near(units::parse(Kind::Length, "2'", in("mm")), 609.6));
  CHECK(near(units::parse(Kind::Length, QString::fromUtf8("250 µm"), in("mm")), 0.25));
  CHECK(near(units::parse(Kind::Length, "20 mm + 1 in", in("mm")), 45.4));
  CHECK(std::abs(*units::parse(Kind::Length, units::format(Kind::Length, 30, 6, in("ft")), in("mm")) - 30) < 1e-3);  // what is shown reads back
  CHECK(!units::parse(Kind::Length, "", in("mm")) && !units::parse(Kind::Length, "abc", in("mm")) && !units::parse(Kind::Length, "30 deg", in("mm")));
  CHECK(near(units::parse(Kind::Angle, "30", in("mm")), 30));
  CHECK(near(units::parse(Kind::Angle, QString::fromUtf8("90°"), in("in")), 90));
  Display radians = in("mm");
  radians.radians = true;
  CHECK(near(units::parse(Kind::Angle, "1.5707963267948966", radians), 90));
  CHECK(near(units::parse(Kind::Angle, "45 deg", radians), 45));
  CHECK(near(units::parse(Kind::Area, QString::fromUtf8("2 in²"), in("in")), 2 * 645.16));
  CHECK(near(units::parse(Kind::Volume, "3", in("cm")), 3000));
  CHECK(near(units::parse(Kind::Mass, "1.5 kg", in("mm")), 1500));
  CHECK(!units::parse(Kind::Mass, "1.5 kg", in("in")));
}

TEST(labels_value_boxes_and_defaults) {
  CHECK_EQ(units::compact(Kind::Length, 25.4, in("in")), "1 in");
  CHECK_EQ(units::compact(Kind::Length, 12.5, in("mm")), "12.5 mm");
  CHECK_EQ(units::compact(Kind::Length, 30, in("in")), "1.181 in");
  CHECK_EQ(units::compact(Kind::Length, -0.0001, in("mm")), "0 mm");
  CHECK_EQ(units::compact(Kind::Angle, 45, in("mm")), QString::fromUtf8("45°"));
  Display fractions = in("in");
  fractions.fraction = 16;
  CHECK_EQ(units::compact(Kind::Length, 25.4 * 1.1875, fractions), "1 3/16 in");
  CHECK_EQ(units::editable(Kind::Length, 30, in("mm")), "30 mm");
  CHECK_EQ(units::editable(Kind::Length, 30, in("in")), "1.181102 in");
  CHECK_EQ(units::editable(Kind::Length, 0.25, in("um")), "250 um");  // the parser's name, not µm
  CHECK_EQ(units::editable(Kind::Angle, 45, in("in")), "45 deg");
  Display radians = in("mm");
  radians.radians = true;
  CHECK_EQ(units::editable(Kind::Angle, 45, radians), "0.785398 rad");
  CHECK(std::abs(*units::parse(Kind::Length, units::editable(Kind::Length, 30, fractions), in("mm")) - 30) < 1e-4);  // never a fraction
  CHECK_EQ(units::preset(10, in("mm")), "10 mm");
  CHECK_EQ(units::preset(10, in("in")), "0.5 in");
  CHECK_EQ(units::preset(20, in("in")), "1 in");
  CHECK_EQ(units::preset(2, in("in")), "0.1 in");
  CHECK_EQ(units::preset(5, in("in")), "0.2 in");
  CHECK_EQ(units::preset(0.05, in("in")), "0.002 in");
  CHECK_EQ(units::preset(0, in("in")), "0 in");
  CHECK_EQ(units::preset(-10, in("in")), "-0.5 in");
  CHECK_EQ(units::preset(40, in("cm")), "4 cm");  // exact where it is short
  CHECK_EQ(units::preset(15, in("cm")), "1.5 cm");
  CHECK_EQ(units::preset(0.05, in("m")), "0.00005 m");
  CHECK_EQ(units::preset(2, in("um")), "2000 um");
  CHECK_EQ(units::preset(10, in("ft")), "0.025 ft");
  CHECK_EQ(units::presetText("2 mm", in("in")), "0.1 in");
  CHECK_EQ(units::presetText("2.50 mm", in("mm")), "2.50 mm");  // as written
  CHECK_EQ(units::presetText("10 mm * 2", in("in")), "10 mm * 2");  // an expression is kept
  CHECK_EQ(units::presetText("45 deg", in("in")), "45 deg");
  CHECK_EQ(units::presetText("6", in("in")), "6");
}

TEST(the_document_unit_is_followed_and_announced) {
  int announced = 0;
  QObject::connect(units::notifier(), &units::Notifier::changed, [&announced] { ++announced; });
  units::setDocumentUnit("mm");
  const int start = announced;
  units::setDocumentUnit("in");
  CHECK(units::current().length == "in" && announced == start + 1);
  units::setDocumentUnit("in");
  CHECK_EQ(announced, start + 1);  // the same unit again: nothing to redraw
  units::setSessionUnit("cm");  // viewer mode: shown in another unit, the document's stays
  CHECK(units::current().length == "cm" && units::documentUnit() == "in" && announced == start + 2);
  units::setDocumentUnit("ft");
  CHECK(units::current().length == "cm" && announced == start + 2);
  units::setSessionUnit("");
  CHECK(units::current().length == "ft" && announced == start + 3);
  units::setDocumentUnit("parsec");
  CHECK_EQ(units::current().length, "mm");
  CHECK_EQ(units::format(Kind::Length, 12.7, 1), "12.7 mm");
  CHECK_EQ(units::symbol(Kind::Length), "mm");
}

int main(int argc, char** argv) {
  QCoreApplication app(argc, argv);
  return check::run_all(argc, argv);
}
