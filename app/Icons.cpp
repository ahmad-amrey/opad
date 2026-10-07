#include "Icons.hpp"

#include <QDir>
#include <QFile>
#include <QHash>
#include <QIconEngine>
#include <QImage>
#include <QPainter>
#include <QPainterPath>
#include <QRegularExpression>
#include <cmath>

#include "Theme.hpp"
#include "opad/cache.hpp"

namespace {

// Inner SVG markup per icon, copied verbatim from opad-icons.js; areas add theirs through OPAD_ICON_TABLE (static
// initialisers in their own files, which reach this table through the function-local static, whatever the order).
QHash<QString, QString>& table() {
  static QHash<QString, QString> t = {
      {"open", R"(<path d="M3 6h6l2 2h10v12H3z"/><path d="M3 11h18"/>)"},
      {"import", R"(<path d="M12 3v11M8 10l4 4 4-4M4 17v3h16v-3"/>)"},
      {"save", R"(<path d="M5 3h11l3 3v15H5z"/><path d="M8 3v5h7V3M8 21v-6h8v6"/>)"},
      {"export", R"(<path d="M12 14V3M8 7l4-4 4 4M4 17v3h16v-3"/>)"},
      {"fit", R"(<path d="M3 8V3h5M16 3h5v5M21 16v5h-5M8 21H3v-5"/><rect x="8" y="8" width="8" height="8"/>)"},
      {"home", R"(<path d="M3 11l9-8 9 8M5 10v10h14V10"/>)"},
      {"ortho", R"(<path d="M4 8h12v12H4zM4 8l4-4h12v12l-4 4M16 8l4-4"/>)"},
      {"shaded", R"(<path d="M4 8l8-4 8 4v8l-8 4-8-4z" fill="currentColor" fill-opacity=".3"/>)"},
      {"shadedEdges", R"(<path d="M4 8l8-4 8 4v8l-8 4-8-4z" fill="currentColor" fill-opacity=".3"/><path d="M4 8l8 4 8-4M12 12v8"/>)"},
      {"wireframe", R"(<path d="M4 8l8-4 8 4v8l-8 4-8-4zM4 8l8 4 8-4M12 12v8"/><path d="M4 16l8-4 8 4M12 4v8" opacity=".45"/>)"},
      {"extensions", R"(<path d="M3 18l9-9"/><path d="M12 9l9-6" stroke-dasharray="2 3"/><circle cx="12" cy="9" r="2"/>)"},
      {"tracking", R"(<path d="M3 12h18M12 3v18" stroke-dasharray="2 3"/><circle cx="12" cy="12" r="3"/>)"},
      {"drawing", R"(<path d="M4 3h12l4 4v14H4zM16 3v5h4"/><path d="M7 17l3-6 6 6z"/>)"},
      {"mesh", R"(<path d="M4 8l8-4 8 4v8l-8 4-8-4zM4 8l8 4 8-4M12 12v8M12 4v8M4 8l8 12 8-12"/>)"},
      {"grid", R"(<rect x="3" y="3" width="18" height="18"/><path d="M3 9h18M3 15h18M9 3v18M15 3v18"/>)"},
      {"section", R"(<path d="M4 8l8-4 8 4v8l-8 4-8-4z"/><path d="M2 15L22 5" stroke-dasharray="3 2"/>)"},
      {"isolate", R"(<circle cx="12" cy="12" r="4"/><path d="M12 2v3M12 19v3M2 12h3M19 12h3"/>)"},
      {"showAll", R"(<path d="M2 12s4-6 10-6 10 6 10 6-4 6-10 6S2 12 2 12z"/><circle cx="12" cy="12" r="3"/>)"},
      {"distance", R"(<path d="M3 12h18M3 8v8M21 8v8M8 10l-3 2 3 2M16 10l3 2-3 2"/>)"},
      {"angle", R"(<path d="M4 20L18 6M4 20h16"/><path d="M13 20a9 9 0 0 0-2.6-6.4"/>)"},
      {"radius", R"(<circle cx="12" cy="12" r="9"/><path d="M12 12l6.4-6.4"/><circle cx="12" cy="12" r="1.2" fill="currentColor"/>)"},
      {"bbox", R"(<rect x="4" y="8" width="12" height="12" stroke-dasharray="3 2"/><path d="M4 8l4-4h12v12l-4 4M16 8l4-4"/>)"},
      {"interference", R"(<rect x="3" y="3" width="12" height="12"/><rect x="9" y="9" width="12" height="12"/><path d="M9 11l4 4M11 9l4 4" stroke-width="1.2"/>)"},
      {"printcheck", R"(<path d="M9 3h6v4l-3 3-3-3z"/><path d="M4 21h16M6 18h12M8 15h8"/>)"},
      {"pen", R"(<path d="M4 20l1-5L17 3l4 4L9 19zM14 6l4 4M5 15l4 4"/>)"},
      {"eraser", R"(<path d="M3 14l10-10 8 8-9 9H9zM7 10l8 8M12 21h9"/>)"},
      {"annotate", R"(<path d="M4 4h16v11H10l-4 4v-4H4z"/><path d="M8 9h8M8 12h5"/>)"},
      {"pin", R"(<path d="M9 3h6v6l3 3H6l3-3zM12 12v9"/>)"},
      {"rename", R"(<path d="M12 4v16M9 4h6M9 20h6M4 9h4v6H4M20 9h-4v6h4"/>)"},
      {"hide", R"(<path d="M2 12s4-6 10-6 10 6 10 6-4 6-10 6S2 12 2 12z"/><circle cx="12" cy="12" r="3"/><path d="M4 4l16 16"/>)"},
      {"eye", R"(<path d="M2 12s4-6 10-6 10 6 10 6-4 6-10 6S2 12 2 12z"/><circle cx="12" cy="12" r="3"/>)"},
      {"lock", R"(<rect x="5" y="11" width="14" height="10"/><path d="M8 11V7a4 4 0 0 1 8 0v4"/>)"},
      {"delete", R"(<path d="M4 7h16M9 7V4h6v3M6 7l1 14h10l1-14M10 11v6M14 11v6"/>)"},
      {"restore", R"(<path d="M3 12a9 9 0 1 0 3-6.7M3 4v5h5"/>)"},
      {"search", R"(<circle cx="11" cy="11" r="6"/><path d="M20 20l-4.5-4.5"/>)"},
      {"settings", R"(<circle cx="12" cy="12" r="7"/><circle cx="12" cy="12" r="3"/><path d="M12 3v2M12 19v2M3 12h2M19 12h2M5.6 5.6l1.4 1.4M18.4 5.6l-1.4 1.4M18.4 18.4l-1.4-1.4M5.6 18.4l1.4-1.4"/>)"},
      {"git", R"(<circle cx="6" cy="5" r="2"/><circle cx="6" cy="19" r="2"/><circle cx="18" cy="8" r="2"/><path d="M6 7v10M18 10c0 5-12 3-12 7"/>)"},
      {"move", R"(<path d="M12 2v20M2 12h20M9 5l3-3 3 3M9 19l3 3 3-3M5 9l-3 3 3 3M19 9l3 3-3 3"/>)"},
      {"reparent", R"(<path d="M4 4h6v5H4zM14 15h6v5h-6zM7 9v9h7"/>)"},
      {"doc", R"(<path d="M6 3h8l4 4v14H6z"/><path d="M14 3v4h4"/>)"},
      {"component", R"(<path d="M4 8l8-4 8 4v8l-8 4-8-4z"/><path d="M4 8l8 4 8-4M12 12v8"/>)"},
      {"body", R"(<path d="M4 8l8-4 8 4v8l-8 4-8-4z" fill="currentColor" fill-opacity=".35"/><path d="M4 8l8 4 8-4M12 12v8"/>)"},
      {"chevronRight", R"(<path d="M9 6l6 6-6 6"/>)"},
      {"chevronDown", R"(<path d="M6 9l6 6 6-6"/>)"},
      {"chevronUp", R"(<path d="M6 15l6-6 6 6"/>)"},
      {"close", R"(<path d="M6 6l12 12M18 6L6 18"/>)"},
      {"float", R"(<rect x="3" y="7" width="14" height="14"/><path d="M7 3h14v14"/>)"},
      {"min", R"(<path d="M5 12h14"/>)"},
      {"max", R"(<rect x="5" y="5" width="14" height="14"/>)"},
      {"flip", R"(<path d="M12 3v18M9 8L4 12l5 4zM15 8l5 4-5 4z"/>)"},
      {"warning", R"(<path d="M12 3l10 18H2z"/><path d="M12 10v4M12 18h.01"/>)"},
      {"check", R"(<path d="M5 12l5 5L20 7"/>)"},
      {"issue", R"(<circle cx="12" cy="12" r="9"/><path d="M12 8v5M12 16h.01"/>)"},
      {"dot", R"(<circle cx="12" cy="12" r="3" fill="currentColor"/>)"},
      {"recent", R"(<circle cx="12" cy="12" r="9"/><path d="M12 7v5l3 3"/>)"},
      {"step", R"(<path d="M6 4h12M6 20h12M12 4v16"/><path d="M8 8l4 4-4 4M16 8l-4 4 4 4" opacity=".5"/>)"},
      {"plus", R"(<path d="M12 5v14M5 12h14"/>)"},
      {"expandAll", R"(<rect x="4" y="4" width="16" height="16" rx="3"/><path d="M12 8v8M8 12h8"/>)"},
      {"collapseAll", R"(<rect x="4" y="4" width="16" height="16" rx="3"/><path d="M8 12h8"/>)"},
      {"rollLeft", R"(<path d="M5 12a7.5 7.5 0 1 0 2.4-5.5"/><path d="M4 3v5h5"/>)"},
      {"rollRight", R"(<path d="M19 12a7.5 7.5 0 1 1-2.4-5.5"/><path d="M20 3v5h-5"/>)"},
      {"locate", R"(<circle cx="12" cy="12" r="6"/><circle cx="12" cy="12" r="1.5" fill="currentColor"/><path d="M12 2v4M12 18v4M2 12h4M18 12h4"/>)"},
      {"more", R"(<circle cx="5" cy="12" r="1.5" fill="currentColor"/><circle cx="12" cy="12" r="1.5" fill="currentColor"/><circle cx="19" cy="12" r="1.5" fill="currentColor"/>)"},
      {"commit", R"(<circle cx="12" cy="12" r="4"/><path d="M2 12h6M16 12h6"/>)"},
      {"browse", R"(<path d="M3 6h6l2 2h10v12H3z"/><circle cx="14" cy="14" r="2.5"/><path d="M16 16l2 2"/>)"},
      {"filterBodies", R"(<path d="M4 8l8-4 8 4v8l-8 4-8-4z" fill="currentColor" fill-opacity=".35"/>)"},
      {"filterFaces", R"(<path d="M4 8l8-4 8 4-8 4z" fill="currentColor" fill-opacity=".5"/><path d="M4 8v8l8 4 8-4V8M12 12v8" opacity=".4"/>)"},
      {"filterEdges", R"(<path d="M4 8l8-4 8 4v8l-8 4-8-4z" opacity=".35"/><path d="M4 8l8 4"/>)"},
      {"filterVertices", R"(<path d="M4 8l8-4 8 4v8l-8 4-8-4zM4 8l8 4 8-4M12 12v8" opacity=".35"/><circle cx="12" cy="12" r="2.5" fill="currentColor"/>)"},
      {"coil", R"(<path d="M5 6c0-2 14-2 14 0s-14 2.5-14 4.5 14 2 14 0M5 10.5c0 2 14 2.5 14 4.5s-14 2-14 0M19 15c0 2-14 2.5-14 4.5"/>)"},
      {"gear", R"(<circle cx="12" cy="12" r="6"/><circle cx="12" cy="12" r="2"/><path d="M12 2.5v3.5M12 18v3.5M2.5 12H6M18 12h3.5M5.3 5.3l2.4 2.4M16.3 16.3l2.4 2.4M5.3 18.7l2.4-2.4M16.3 7.7l2.4-2.4"/>)"},
      {"thicken", R"(<path d="M3 15c5-7 13-7 18 0"/><path d="M3 19c5-7 13-7 18 0" opacity=".55"/><path d="M12 10v4M10.5 12.5L12 14l1.5-1.5"/>)"},
      {"offset", R"(<rect x="7" y="7" width="10" height="10"/><rect x="3" y="3" width="18" height="18" rx="2" opacity=".55"/>)"},
      {"project", R"(<path d="M4 17l8-3 8 3-8 3z"/><path d="M8 5h8v5H8z" opacity=".6"/><path d="M12 10v5M10.5 13.5L12 15l1.5-1.5"/>)"},
      {"cursor", R"(<path d="M6 3l12 9-5.5 1.2L15.5 20l-2.6 1.1-3-6.7L6 18z"/>)"},
      {"xrayHighlight", R"(<rect x="10" y="3" width="11" height="11" fill="currentColor" fill-opacity=".35"/><rect x="3" y="10" width="11" height="11"/><path d="M10 10h4v4h-4z" stroke-dasharray="2 1.5"/>)"},
      {"hoverHighlight", R"(<path d="M3 8l7-4 7 4v7l-7 4-7-4z" fill="currentColor" fill-opacity=".3"/><path d="M14 11l7 5.2-3 .6 1.7 3.4-1.5.7-1.7-3.4L14 20z"/>)"},
      {"sketch", R"(<path d="M3 17l9-5 9 5-9 4z" opacity=".5"/><path d="M6 13V5h8l4 4v4" /><path d="M9 9h5"/>)"},
      {"finish", R"(<circle cx="12" cy="12" r="9"/><path d="M7.5 12.5l3 3 6-7"/>)"},
      {"extrude", R"(<path d="M4 16l8-3 8 3-8 3z"/><path d="M4 16V9l8-3 8 3v7M12 6v7" opacity=".55"/><path d="M12 1v4M10 3l2-2 2 2"/>)"},
      {"revolve", R"(<path d="M12 3v18" stroke-dasharray="3 2"/><path d="M12 6h5v10h-5"/><path d="M5 12a7 3 0 0 0 14 0" opacity=".6"/><path d="M5 12l-1.5 2M5 12l2 1.5"/>)"},
      {"sweep", R"(<path d="M4 19c0-9 6-13 16-13"/><rect x="2" y="17" width="5" height="4"/><rect x="17" y="3.5" width="5" height="5" opacity=".6"/>)"},
      {"loft", R"(<ellipse cx="12" cy="5" rx="4" ry="1.8"/><path d="M4 19l4-14M20 19l-4-14"/><path d="M4 19h16v1.5H4z"/>)"},
      {"pipe", R"(<path d="M4 20V11a5 5 0 0 1 5-5h11"/><path d="M8 20v-9a1 1 0 0 1 1-1h11" opacity=".6"/><ellipse cx="20" cy="8" rx="1.2" ry="2"/>)"},
      {"hole", R"(<path d="M3 9l9-4 9 4-9 4z"/><ellipse cx="12" cy="9" rx="3" ry="1.4"/><path d="M9 9v7c0 1 6 1 6 0V9" opacity=".6"/><path d="M3 9v6l9 4 9-4V9" opacity=".4"/>)"},
      {"box", R"(<path d="M4 8l8-4 8 4v8l-8 4-8-4z"/><path d="M4 8l8 4 8-4M12 12v8"/>)"},
      {"cylinder", R"(<ellipse cx="12" cy="6" rx="7" ry="2.5"/><path d="M5 6v12c0 1.4 3.1 2.5 7 2.5s7-1.1 7-2.5V6"/>)"},
      {"sphere", R"(<circle cx="12" cy="12" r="9"/><ellipse cx="12" cy="12" rx="9" ry="3.2" opacity=".6"/>)"},
      {"cone", R"(<path d="M12 3L5 18M12 3l7 15"/><ellipse cx="12" cy="18" rx="7" ry="2.5"/>)"},
      {"torus", R"(<ellipse cx="12" cy="12" rx="9" ry="5"/><ellipse cx="12" cy="11.3" rx="3.6" ry="1.5"/>)"},
      {"fillet", R"(<path d="M4 20V11a7 7 0 0 1 7-7h9"/><path d="M4 4h5M4 4v5" opacity=".45" stroke-dasharray="2 2"/>)"},
      {"chamfer", R"(<path d="M4 20v-9l7-7h9"/><path d="M4 4h5M4 4v5" opacity=".45" stroke-dasharray="2 2"/>)"},
      {"shell", R"(<path d="M4 8l8-4 8 4v8l-8 4-8-4z"/><path d="M7 9.5l5-2.5 5 2.5v5l-5 2.5-5-2.5z" opacity=".6"/>)"},
      {"draft", R"(<path d="M6 20L9 5h9l3 15z"/><path d="M6 20V5h3" opacity=".45" stroke-dasharray="2 2"/>)"},
      {"presspull", R"(<path d="M4 14l8-3 8 3-8 3z"/><path d="M4 14v4l8 3 8-3v-4" opacity=".6"/><path d="M12 2v7M9.5 4.5L12 2l2.5 2.5M9.5 6.5L12 9l2.5-2.5"/>)"},
      {"scale", R"(<rect x="3" y="11" width="10" height="10"/><path d="M8 7V3h13v13h-4" opacity=".6"/><path d="M13 11l6-6M15 5h4v4"/>)"},
      {"combine", R"(<circle cx="9" cy="12" r="6"/><circle cx="15" cy="12" r="6"/><path d="M12 6.8a6 6 0 0 1 0 10.4 6 6 0 0 1 0-10.4z" fill="currentColor" fill-opacity=".4"/>)"},
      {"split", R"(<path d="M4 8l8-4 8 4v8l-8 4-8-4z"/><path d="M2 17L22 7" stroke-dasharray="3 2"/>)"},
      {"mirror", R"(<path d="M12 2v20" stroke-dasharray="3 2"/><path d="M9 6L3 18h6z"/><path d="M15 6l6 12h-6z" opacity=".55"/>)"},
      {"patternRect", R"(<rect x="3" y="3" width="6" height="6"/><rect x="15" y="3" width="6" height="6" opacity=".55"/><rect x="3" y="15" width="6" height="6" opacity=".55"/><rect x="15" y="15" width="6" height="6" opacity=".55"/>)"},
      {"patternCirc", R"(<circle cx="12" cy="12" r="8" stroke-dasharray="2 3" opacity=".5"/><circle cx="12" cy="4" r="2.2"/><circle cx="19" cy="16" r="2.2" opacity=".55"/><circle cx="5" cy="16" r="2.2" opacity=".55"/>)"},
      {"patternPoints", R"(<circle cx="6" cy="7" r="2.6"/><circle cx="17" cy="5" r="2.6" opacity=".55"/><circle cx="18" cy="17" r="2.6" opacity=".55"/><circle cx="8" cy="18" r="2.6" opacity=".55"/><path d="M6 7l11-2M6 7l12 10M6 7l2 11" stroke-dasharray="1.5 2" stroke-width="1" opacity=".5"/>)"},
      {"plane", R"(<path d="M3 15l8-9h10l-8 9z" fill="currentColor" fill-opacity=".25"/><path d="M12 10.5V3M10 5l2-2 2 2"/>)"},
      {"axis", R"(<path d="M3 21L21 3" stroke-dasharray="5 2 1 2"/><circle cx="12" cy="12" r="2"/>)"},
      {"fx", R"(<path d="M10 4c-3 0-3 2-3 5v7c0 3-1 4-3 4M4 11h7"/><path d="M13 12l7 8M20 12l-7 8"/>)"},
      {"regen", R"(<path d="M20 12a8 8 0 1 1-2.3-5.6M20 3v5h-5"/>)"},
      {"line", R"(<path d="M5 19L19 5"/><circle cx="5" cy="19" r="1.8" fill="currentColor"/><circle cx="19" cy="5" r="1.8" fill="currentColor"/>)"},
      {"rect", R"(<rect x="4" y="6" width="16" height="12"/><circle cx="4" cy="6" r="1.6" fill="currentColor"/><circle cx="20" cy="18" r="1.6" fill="currentColor"/>)"},
      {"crect", R"(<rect x="4" y="6" width="16" height="12"/><circle cx="12" cy="12" r="1.6" fill="currentColor"/><path d="M12 12l8 6" opacity=".5" stroke-dasharray="2 2"/>)"},
      {"circle", R"(<circle cx="12" cy="12" r="8"/><circle cx="12" cy="12" r="1.5" fill="currentColor"/>)"},
      {"circle3", R"(<circle cx="12" cy="12" r="8"/><circle cx="12" cy="4" r="1.6" fill="currentColor"/><circle cx="5" cy="16" r="1.6" fill="currentColor"/><circle cx="19" cy="16" r="1.6" fill="currentColor"/>)"},
      {"arc3", R"(<path d="M4 17a9 9 0 0 1 16 0"/><circle cx="4" cy="17" r="1.6" fill="currentColor"/><circle cx="20" cy="17" r="1.6" fill="currentColor"/><circle cx="12" cy="8" r="1.6" fill="currentColor"/>)"},
      {"arcc", R"(<path d="M5 17A9 9 0 0 1 17 6"/><circle cx="13" cy="17" r="1.6" fill="currentColor"/><path d="M13 17H5M13 17l4-11" opacity=".5" stroke-dasharray="2 2"/>)"},
      {"polygon", R"(<path d="M12 3l8 4.5v9L12 21l-8-4.5v-9z"/>)"},
      {"slot", R"(<path d="M8 7h8a5 5 0 0 1 0 10H8A5 5 0 0 1 8 7z"/><path d="M8 12h8" opacity=".5" stroke-dasharray="2 2"/>)"},
      {"ellipse", R"(<ellipse cx="12" cy="12" rx="9" ry="5.5"/><circle cx="12" cy="12" r="1.4" fill="currentColor"/>)"},
      {"spline", R"(<path d="M3 17c4-12 7 6 10-3s4-8 8-7"/><circle cx="3" cy="17" r="1.5" fill="currentColor"/><circle cx="21" cy="7" r="1.5" fill="currentColor"/>)"},
      {"point", R"(<circle cx="12" cy="12" r="2.2" fill="currentColor"/><path d="M12 4v4M12 16v4M4 12h4M16 12h4"/>)"},
      // The sketch tools listed in the More tools menus (they all showed the generic sketch icon).
      {"rect3", R"(<path d="M4 15l9-9 7 7-9 9z"/><circle cx="4" cy="15" r="1.6" fill="currentColor"/><circle cx="13" cy="6" r="1.6" fill="currentColor"/><circle cx="20" cy="13" r="1.6" fill="currentColor"/>)"},
      {"circle2", R"(<circle cx="12" cy="12" r="8"/><path d="M4 12h16" opacity=".45" stroke-dasharray="2 2"/><circle cx="4" cy="12" r="1.6" fill="currentColor"/><circle cx="20" cy="12" r="1.6" fill="currentColor"/>)"},
      {"tangentCircle", R"(<path d="M3 20h18M3 20L13 3" opacity=".55"/><circle cx="11" cy="14.2" r="5.8"/>)"},
      {"tangentArc", R"(<path d="M3 18h8" opacity=".55"/><path d="M11 18a7 7 0 0 0 7-7V5"/><circle cx="11" cy="18" r="1.6" fill="currentColor"/>)"},
      {"cslot", R"(<path d="M8 8h8a4 4 0 0 1 0 8H8a4 4 0 0 1 0-8z"/><circle cx="12" cy="12" r="1.5" fill="currentColor"/>)"},
      {"arcslot", R"(<path d="M3.5 16a8.5 8.5 0 0 1 17 0M8 16a4 4 0 0 1 8 0M3.5 16a2.25 2.25 0 0 0 4.5 0M16 16a2.25 2.25 0 0 0 4.5 0"/>)"},
      {"polygonOuter", R"(<path d="M12 3l7.8 4.5v9L12 21l-7.8-4.5v-9z"/><circle cx="12" cy="12" r="6.8" opacity=".5"/>)"},
      {"controlSpline", R"(<path d="M4 18C7 6 15 18 20 6"/><path d="M4 18L8 7l8 10 4-11" opacity=".45" stroke-dasharray="2 2"/>)"},
      {"conic", R"(<path d="M4 19C8 5 16 5 20 19"/><path d="M4 19L12 7l8 12" opacity=".45" stroke-dasharray="2 2"/>)"},
      {"text", R"(<path d="M5 5h14M12 5v14"/><path d="M8.5 19h7" opacity=".55"/>)"},
      {"rotate", R"(<path d="M19 12a7 7 0 1 1-2.05-4.95"/><path d="M19 4v4h-4"/>)"},
      {"copy", R"(<rect x="9" y="9" width="11" height="11"/><path d="M5 15V5h10" opacity=".6"/>)"},
      {"extend", R"(<path d="M3 12h11"/><path d="M14 12h5" stroke-dasharray="2 2"/><path d="M21 5v14"/>)"},
      {"breakCurve", R"(<path d="M4 4l6 6M14 14l6 6M20 4l-6 6M10 14l-6 6"/><circle cx="12" cy="12" r="1.6" fill="currentColor"/>)"},
      {"subtract", R"(<circle cx="9" cy="12" r="6"/><circle cx="15" cy="12" r="6" opacity=".5" stroke-dasharray="2 2"/>)"},
      {"intersect", R"(<circle cx="9" cy="12" r="6" opacity=".4"/><circle cx="15" cy="12" r="6" opacity=".4"/><path d="M12 7.2a6 6 0 0 1 0 9.6a6 6 0 0 1 0-9.6z"/>)"},
      {"heal", R"(<path d="M3 17l7-4.5M14 11.5L21 7"/><circle cx="12" cy="12" r="2" fill="currentColor"/>)"},
      {"explode", R"(<rect x="9" y="9" width="6" height="6"/><path d="M6.5 6.5L3 3M17.5 6.5L21 3M6.5 17.5L3 21M17.5 17.5L21 21"/>)"},
      {"simplify", R"(<path d="M3 8c2-3 4 3 6 0s4 3 6 0 4 3 6 0" opacity=".45"/><path d="M3 17l18-2"/>)"},
      {"image", R"(<rect x="3" y="5" width="18" height="14"/><path d="M3 16l5-5 5 5 3-3 5 5"/><circle cx="16" cy="9" r="1.5" fill="currentColor"/>)"},
      {"breakLink", R"(<path d="M10 7H8a5 5 0 0 0 0 10h2M14 7h2a5 5 0 0 1 0 10h-2"/><path d="M12 3v3M12 18v3" opacity=".6"/>)"},
      {"magnet", R"(<path d="M6 4v8a6 6 0 0 0 12 0V4"/><path d="M6 8h3.5M14.5 8H18" opacity=".6"/>)"},
      {"list", R"(<path d="M9 7h11M9 12h11M9 17h11"/><circle cx="5" cy="7" r="1.2" fill="currentColor"/><circle cx="5" cy="12" r="1.2" fill="currentColor"/><circle cx="5" cy="17" r="1.2" fill="currentColor"/>)"},
      {"trim", R"(<circle cx="7" cy="7" r="2.5"/><circle cx="7" cy="17" r="2.5"/><path d="M9 8.5L20 17M9 15.5L20 7"/>)"},
      {"dimension", R"(<path d="M4 6v12M20 6v12M4 12h16M7 9.5L4 12l3 2.5M17 9.5l3 2.5-3 2.5"/>)"},
      {"construction", R"(<path d="M4 20L20 4" stroke-dasharray="4 3"/>)"},
      {"cHorizontal", R"(<path d="M3 12h18"/><path d="M9 7h6M9 17h6" opacity=".5"/>)"},
      {"cVertical", R"(<path d="M12 3v18"/><path d="M7 9v6M17 9v6" opacity=".5"/>)"},
      {"cCoincident", R"(<circle cx="12" cy="12" r="3" fill="currentColor"/><path d="M3 21l6-6M21 3l-6 6"/>)"},
      {"cParallel", R"(<path d="M5 20L13 4M11 20l8-16"/>)"},
      {"cPerpendicular", R"(<path d="M4 19h16M12 19V5"/>)"},
      {"cTangent", R"(<circle cx="12" cy="14" r="6"/><path d="M3 8h18"/>)"},
      {"cEqual", R"(<path d="M5 9h14M5 15h14"/>)"},
      {"cConcentric", R"(<circle cx="12" cy="12" r="8"/><circle cx="12" cy="12" r="4"/><circle cx="12" cy="12" r="1" fill="currentColor"/>)"},
      {"cMidpoint", R"(<path d="M3 17L21 7"/><path d="M12 12m-2.5 0a2.5 2.5 0 1 0 5 0a2.5 2.5 0 1 0 -5 0" fill="currentColor"/>)"},
      {"cSymmetric", R"(<path d="M12 3v18" stroke-dasharray="3 2"/><path d="M8 8l-4 4 4 4M16 8l4 4-4 4"/>)"},
      {"cCollinear", R"(<path d="M3 17l7-4M14 11l7-4"/><path d="M10 13l4-2" opacity=".4" stroke-dasharray="2 2"/>)"},
      {"cFix", R"(<rect x="5" y="11" width="14" height="10"/><path d="M8 11V7a4 4 0 0 1 8 0v4"/>)"},
  };
  return t;
}

// ---------------------------------------------------------------- SVG path data -> QPainterPath
struct PathParser {
  QString d;
  int i = 0;
  QPainterPath path;
  QPointF cur, start, lastCtrl;
  QChar lastCmd;

  void skip() {
    while (i < d.size() && (d[i].isSpace() || d[i] == ',')) ++i;
  }
  bool number(double& out) {
    skip();
    int s = i;
    if (i < d.size() && (d[i] == '-' || d[i] == '+')) ++i;
    bool digits = false;
    while (i < d.size() && (d[i].isDigit() || d[i] == '.')) { ++i; digits = true; }
    if (i < d.size() && (d[i] == 'e' || d[i] == 'E')) {
      ++i;
      if (i < d.size() && (d[i] == '-' || d[i] == '+')) ++i;
      while (i < d.size() && d[i].isDigit()) ++i;
    }
    if (!digits) { i = s; return false; }
    out = d.mid(s, i - s).toDouble();
    return true;
  }
  bool flag(int& out) {
    skip();
    if (i < d.size() && (d[i] == '0' || d[i] == '1')) { out = d[i] == '1'; ++i; return true; }
    return false;
  }

  void arc(double rx, double ry, double rot, int large, int sweep, QPointF to) {
    // SVG endpoint parameterisation -> centre parameterisation (W3C implementation notes F.6.5).
    if (rx == 0 || ry == 0 || to == cur) { path.lineTo(to); cur = to; return; }
    double phi = rot * M_PI / 180.0, cph = std::cos(phi), sph = std::sin(phi);
    double dx = (cur.x() - to.x()) / 2, dy = (cur.y() - to.y()) / 2;
    double x1 = cph * dx + sph * dy, y1 = -sph * dx + cph * dy;
    rx = std::fabs(rx); ry = std::fabs(ry);
    double lambda = (x1 * x1) / (rx * rx) + (y1 * y1) / (ry * ry);
    if (lambda > 1) { rx *= std::sqrt(lambda); ry *= std::sqrt(lambda); }
    double num = rx * rx * ry * ry - rx * rx * y1 * y1 - ry * ry * x1 * x1;
    double den = rx * rx * y1 * y1 + ry * ry * x1 * x1;
    double coef = (large != sweep ? 1 : -1) * std::sqrt(std::max(0.0, num / den));
    double cx1 = coef * rx * y1 / ry, cy1 = -coef * ry * x1 / rx;
    double cx = cph * cx1 - sph * cy1 + (cur.x() + to.x()) / 2, cy = sph * cx1 + cph * cy1 + (cur.y() + to.y()) / 2;
    auto ang = [](double ux, double uy, double vx, double vy) {
      double dot = ux * vx + uy * vy, len = std::sqrt((ux * ux + uy * uy) * (vx * vx + vy * vy));
      double a = std::acos(std::clamp(dot / len, -1.0, 1.0));
      return (ux * vy - uy * vx) < 0 ? -a : a;
    };
    double theta1 = ang(1, 0, (x1 - cx1) / rx, (y1 - cy1) / ry);
    double dtheta = ang((x1 - cx1) / rx, (y1 - cy1) / ry, (-x1 - cx1) / rx, (-y1 - cy1) / ry);
    if (!sweep && dtheta > 0) dtheta -= 2 * M_PI;
    else if (sweep && dtheta < 0) dtheta += 2 * M_PI;
    // Flatten into cubic segments in the rotated frame.
    int segs = std::max(1, static_cast<int>(std::ceil(std::fabs(dtheta) / (M_PI / 2))));
    double delta = dtheta / segs;
    double t = 4.0 / 3.0 * std::tan(delta / 4);
    double th = theta1;
    for (int s = 0; s < segs; ++s) {
      double c1 = std::cos(th), s1 = std::sin(th), c2 = std::cos(th + delta), s2 = std::sin(th + delta);
      QPointF p1(c1 - t * s1, s1 + t * c1), p2(c2 + t * s2, s2 - t * c2), p3(c2, s2);
      auto map = [&](QPointF p) { double x = p.x() * rx, y = p.y() * ry; return QPointF(cph * x - sph * y + cx, sph * x + cph * y + cy); };
      path.cubicTo(map(p1), map(p2), map(p3));
      th += delta;
    }
    cur = to;
  }

  void parse() {
    while (true) {
      skip();
      if (i >= d.size()) break;
      QChar c = d[i];
      if (c.isLetter()) { lastCmd = c; ++i; }
      else if (lastCmd.isNull()) break;
      QChar cmd = lastCmd;
      bool rel = cmd.isLower();
      QChar u = cmd.toUpper();
      auto pt = [&](double x, double y) { return rel ? cur + QPointF(x, y) : QPointF(x, y); };
      double a, b, c2, d2, e, f;
      if (u == 'M') {
        if (!number(a) || !number(b)) break;
        cur = pt(a, b); start = cur; path.moveTo(cur); lastCmd = rel ? 'l' : 'L';
      } else if (u == 'L') {
        if (!number(a) || !number(b)) break;
        cur = pt(a, b); path.lineTo(cur);
      } else if (u == 'H') {
        if (!number(a)) break;
        cur = QPointF(rel ? cur.x() + a : a, cur.y()); path.lineTo(cur);
      } else if (u == 'V') {
        if (!number(a)) break;
        cur = QPointF(cur.x(), rel ? cur.y() + a : a); path.lineTo(cur);
      } else if (u == 'C') {
        if (!number(a) || !number(b) || !number(c2) || !number(d2) || !number(e) || !number(f)) break;
        QPointF p1 = pt(a, b), p2 = pt(c2, d2), p3 = pt(e, f);
        path.cubicTo(p1, p2, p3); lastCtrl = p2; cur = p3;
      } else if (u == 'S') {
        if (!number(a) || !number(b) || !number(c2) || !number(d2)) break;
        QPointF p1 = (lastCmd.toUpper() == 'S' || lastCmd.toUpper() == 'C') ? cur * 2 - lastCtrl : cur;
        QPointF p2 = pt(a, b), p3 = pt(c2, d2);
        path.cubicTo(p1, p2, p3); lastCtrl = p2; cur = p3;
      } else if (u == 'A') {
        int large, sweep;
        if (!number(a) || !number(b) || !number(c2) || !flag(large) || !flag(sweep) || !number(e) || !number(f)) break;
        arc(a, b, c2, large, sweep, pt(e, f));
      } else if (u == 'Z') {
        path.closeSubpath(); cur = start;
      } else {
        break;
      }
      if (u != 'C' && u != 'S') lastCtrl = cur;
      if (u == 'Z') lastCmd = QChar();
    }
  }
};

// The value of name="..." where the name starts a word (as \b would: "opacity" also matches in "fill-opacity", which the
// icons were drawn with). Compiling a regular expression per lookup made building the menus take half a second.
QString attr_str(const QString& attrs, const char* name) {
  const QString key = QLatin1String(name) + QLatin1String("=\"");
  for (qsizetype at = attrs.indexOf(key); at >= 0; at = attrs.indexOf(key, at + 1)) {
    const QChar before = at > 0 ? attrs[at - 1] : QChar(' ');
    if (before.isLetterOrNumber() || before == '_') continue;
    const qsizetype start = at + key.size(), end = attrs.indexOf('"', start);
    if (end < 0) return {};
    return attrs.mid(start, end - start);
  }
  return {};
}
double attr_num(const QString& attrs, const char* name, double def) {
  const QString value = attr_str(attrs, name);
  if (value.isNull()) return def;
  bool ok = false;
  const double v = value.toDouble(&ok);
  return ok ? v : 0.0;  // as QString::toDouble gave before
}

void render(QPainter& p, const QString& markup, const QColor& color) {
  // Ellipses too: without them the cylinder, sphere and cone icons lost their rims and the torus icon was blank.
  static const QRegularExpression el("<(path|rect|circle|ellipse)([^>]*?)/?>");
  auto it = el.globalMatch(markup);
  while (it.hasNext()) {
    auto m = it.next();
    QString tag = m.captured(1), attrs = m.captured(2);
    QPainterPath path;
    if (tag == "path") {
      PathParser pp{attr_str(attrs, "d")};
      pp.parse();
      path = pp.path;
    } else if (tag == "rect") {
      path.addRect(attr_num(attrs, "x", 0), attr_num(attrs, "y", 0), attr_num(attrs, "width", 0), attr_num(attrs, "height", 0));
    } else if (tag == "ellipse") {
      path.addEllipse(QPointF(attr_num(attrs, "cx", 0), attr_num(attrs, "cy", 0)), attr_num(attrs, "rx", 0), attr_num(attrs, "ry", 0));
    } else {
      double r = attr_num(attrs, "r", 0);
      path.addEllipse(QPointF(attr_num(attrs, "cx", 0), attr_num(attrs, "cy", 0)), r, r);
    }
    p.save();
    p.setOpacity(attr_num(attrs, "opacity", 1.0));
    QString fill = attr_str(attrs, "fill");
    if (fill == "currentColor") {
      QColor fc = color;
      fc.setAlphaF(attr_num(attrs, "fill-opacity", 1.0));
      p.fillPath(path, fc);
    }
    QPen pen(color, 2.0, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
    QString dash = attr_str(attrs, "stroke-dasharray");
    if (!dash.isEmpty()) {
      QVector<qreal> pattern;
      for (const QString& v : dash.split(' ', Qt::SkipEmptyParts)) pattern << v.toDouble() / 2.0;
      pen.setDashPattern(pattern);
    }
    p.strokePath(path, pen);
    p.restore();
  }
}

QHash<QString, QPixmap> g_cache;

QStringList& clashList() {
  static QStringList names;
  return names;
}

// Renders the icon when it is first painted, at that display scale and state only: drawing every icon at four scales
// and three states up front cost most of a second at startup.
class LazyIcon : public QIconEngine {
 public:
  LazyIcon(QString name, QColor normal, QColor disabled, QColor selected, int size)
      : m_name(std::move(name)), m_normal(normal), m_disabled(disabled.isValid() ? disabled : normal),
        m_selected(selected.isValid() ? selected : normal), m_size(size) {}
  QSize actualSize(const QSize& size, QIcon::Mode, QIcon::State) override {
    const int side = std::min({size.width(), size.height(), m_size});  // never larger than drawn, as with pixmaps
    return {side, side};
  }
  QPixmap pixmap(const QSize& size, QIcon::Mode mode, QIcon::State state) override { return scaledPixmap(size, mode, state, 1.0); }
  QPixmap scaledPixmap(const QSize& size, QIcon::Mode mode, QIcon::State, qreal scale) override {
    const QColor& c = mode == QIcon::Disabled ? m_disabled : mode == QIcon::Selected ? m_selected : m_normal;
    QPixmap pm = icons::pixmap(m_name, c, m_size, scale);
    const int side = std::min({size.width(), size.height(), m_size});
    if (side >= m_size || side <= 0) return pm;
    QPixmap smaller = pm.scaled(QSize(side, side) * scale, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    smaller.setDevicePixelRatio(scale);
    return smaller;
  }
  void paint(QPainter* painter, const QRect& rect, QIcon::Mode mode, QIcon::State state) override {
    const qreal scale = painter->device() ? painter->device()->devicePixelRatioF() : 1.0;
    const QPixmap pm = scaledPixmap(rect.size(), mode, state, scale);
    const QSize logical = (QSizeF(pm.size()) / pm.devicePixelRatio()).toSize();
    painter->drawPixmap(QRect(rect.center() - QPoint(logical.width() / 2, logical.height() / 2), logical), pm);
  }
  QList<QSize> availableSizes(QIcon::Mode, QIcon::State) override { return {QSize(m_size, m_size)}; }
  QIconEngine* clone() const override { return new LazyIcon(*this); }
  QString key() const override { return QStringLiteral("opad-lazy"); }

 private:
  QString m_name;
  QColor m_normal, m_disabled, m_selected;
  int m_size;
};

}  // namespace

namespace icons {

IconTable::IconTable(std::initializer_list<std::pair<const char*, const char*>> rows) {
  for (const auto& [name, markup] : rows) {
    const QString key = QString::fromUtf8(name);
    if (!table().contains(key)) table().insert(key, QString::fromUtf8(markup));
    else if (table().value(key) != QString::fromUtf8(markup)) clashList().append(key);
  }
}

QStringList clashes() { return clashList(); }

bool has(const QString& name) { return table().contains(name); }

QPixmap pixmap(const QString& name, const QColor& color, int size, qreal dpr) {
  QString key = QString("%1|%2|%3|%4").arg(name, color.name(QColor::HexArgb)).arg(size).arg(dpr);
  auto it = g_cache.find(key);
  if (it != g_cache.end()) return *it;
  QPixmap pm(static_cast<int>(size * dpr), static_cast<int>(size * dpr));
  pm.setDevicePixelRatio(dpr);
  pm.fill(Qt::transparent);
  if (table().contains(name)) {
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    p.scale(size / 24.0, size / 24.0);
    render(p, table().value(name), color);
  }
  g_cache.insert(key, pm);
  return pm;
}

QIcon icon(const QString& name, const QColor& normal, const QColor& disabled, const QColor& selected, int size) {
  return QIcon(new LazyIcon(name, normal, disabled, selected, size));
}

QIcon themed(const QString& name, int size) {
  const Tokens& t = theme::current();
  return icon(name, t.fg, t.fg3, t.onsel, size);
}

QIcon appIcon() {
  QIcon ic;
  for (int size : {16, 24, 32, 48, 64, 256}) ic.addFile(QString(":/res/opad-%1.png").arg(size), QSize(size, size));
  return ic;
}

void clearCache() { g_cache.clear(); }

QString gripFile(const QColor& color, bool vertical) {
  QString dir = QString::fromStdString((opad::cache_dir() / "ui-icons").string());
  QDir().mkpath(dir);
  QString path = QString("%1/grip-%2-%3.png").arg(dir, vertical ? "v" : "h", color.name(QColor::HexArgb).mid(1));
  if (!QFile::exists(path)) {
    QImage img(vertical ? QSize(2, 14) : QSize(14, 2), QImage::Format_ARGB32_Premultiplied);
    img.fill(Qt::transparent);
    for (int i = 0; i < 14; i += 4)  // four 2 px dots, 2 px apart
      for (int a = 0; a < 2; ++a)
        for (int b = 0; b < 2; ++b) img.setPixelColor(vertical ? QPoint(a, i + b) : QPoint(i + b, a), color);
    img.save(path);
  }
  return path.replace('\\', '/');
}

QString file(const QString& name, const QColor& color, int size) {
  QString dir = QString::fromStdString((opad::cache_dir() / "ui-icons").string());
  QDir().mkpath(dir);
  QString path = QString("%1/%2-%3-%4.png").arg(dir, name, color.name(QColor::HexArgb).mid(1)).arg(size);
  if (!QFile::exists(path)) pixmap(name, color, size, 2.0).save(path);
  return path.replace('\\', '/');
}

}  // namespace icons
