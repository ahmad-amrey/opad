#include "Theme.hpp"

#include "Icons.hpp"

#include <QApplication>
#include <QFontDatabase>
#include <QPalette>
#include <QStyleFactory>

namespace {
Tokens g_tokens = theme::tokens(true);
QString g_uiFamily, g_monoFamily;

QString pick(const QStringList& wanted, const QString& fallback) {
  for (const QString& f : wanted)
    if (QFontDatabase::hasFamily(f)) return f;
  return fallback;
}
}  // namespace

namespace theme {

Tokens tokens(bool dark) {
  Tokens t;
  t.dark = dark;
  if (dark) {
    t.bg = QColor("#1e1f22"); t.bg2 = QColor("#26272b"); t.bg3 = QColor("#2f3136"); t.bg4 = QColor("#3a3d43");
    t.line = QColor("#3b3e45"); t.fg = QColor("#dcdde0"); t.fg2 = QColor("#9b9ea6"); t.fg3 = QColor("#666a73");
    t.vp = QColor("#232529"); t.sel = QColor("#2f78e0"); t.selbg = QColor(47, 120, 224, 51); t.hov = QColor("#33c3d6");
    t.amber = QColor("#e3a63a"); t.green = QColor("#4fbd6f"); t.red = QColor("#e2584c");
    t.mtop = QColor("#a2aab4"); t.mleft = QColor("#7f8791"); t.mright = QColor("#656d77"); t.medge = QColor("#1a1c1f");
    t.cap = QColor("#5d7fa8"); t.onsel = QColor("#ffffff");
  } else {
    t.bg = QColor("#ececee"); t.bg2 = QColor("#f8f8f9"); t.bg3 = QColor("#e0e1e4"); t.bg4 = QColor("#d2d4d8");
    t.line = QColor("#cfd1d5"); t.fg = QColor("#1e1f23"); t.fg2 = QColor("#5d616a"); t.fg3 = QColor("#a3a6ad");
    t.vp = QColor("#e4e5e8"); t.sel = QColor("#1f6fe0"); t.selbg = QColor(31, 111, 224, 38); t.hov = QColor("#0f96a8");
    t.amber = QColor("#b5720c"); t.green = QColor("#2d9550"); t.red = QColor("#cc3d31");
    t.mtop = QColor("#d3d7dc"); t.mleft = QColor("#b3b9c1"); t.mright = QColor("#949ba5"); t.medge = QColor("#4b5058");
    t.cap = QColor("#7fa0c9"); t.onsel = QColor("#ffffff");
  }
  return t;
}

const Tokens& current() { return g_tokens; }

QString css(const QColor& c) {
  if (c.alpha() == 255) return c.name();
  return QString("rgba(%1,%2,%3,%4)").arg(c.red()).arg(c.green()).arg(c.blue()).arg(c.alphaF(), 0, 'f', 2);
}

QFont ui(int px, int weight) {
  if (g_uiFamily.isEmpty()) g_uiFamily = pick({"IBM Plex Sans", "Segoe UI", "Inter", "Helvetica Neue", "DejaVu Sans"}, QApplication::font().family());
  QFont f(g_uiFamily);
  f.setPixelSize(px);
  f.setWeight(static_cast<QFont::Weight>(weight));
  return f;
}

QFont mono(int px) {
  if (g_monoFamily.isEmpty()) g_monoFamily = pick({"JetBrains Mono", "Cascadia Mono", "Consolas", "DejaVu Sans Mono", "Menlo", "Liberation Mono"}, "monospace");
  QFont f(g_monoFamily);
  f.setPixelSize(px);
  return f;
}

QString stylesheet(const Tokens& t) {
  const QString uiF = ui().family(), monoF = mono().family();
  const QString bg = css(t.bg), bg2 = css(t.bg2), bg3 = css(t.bg3), bg4 = css(t.bg4), line = css(t.line), fg = css(t.fg),
                fg2 = css(t.fg2), fg3 = css(t.fg3), sel = css(t.sel), selbg = css(t.selbg), amber = css(t.amber), red = css(t.red),
                onsel = css(t.onsel);
  QString s;
  s += QString("* { font-family: '%1'; font-size: 13px; color: %2; }\n").arg(uiF, fg);
  s += QString("QMainWindow, QDialog, QWidget#central { background: %1; }\n").arg(bg);
  // Splitters: 5 px, bg fill, 1 px line each side, centred dotted grip in fg3 that says the bar can be dragged.
  // Qt names the separator after its own shape: :vertical is the upright bar beside the browser, :horizontal the
  // one above the timeline. (Swapped, the upright bar gets top/bottom borders, i.e. none, and sideways dots.)
  s += QString("QMainWindow::separator { background: %1; width: 5px; height: 5px; }\n"
               "QMainWindow::separator:vertical { border-left: 1px solid %2; border-right: 1px solid %2; image: url(%3); }\n"
               "QMainWindow::separator:horizontal { border-top: 1px solid %2; border-bottom: 1px solid %2; image: url(%4); }\n")
           .arg(bg, line, icons::gripFile(t.fg3, true), icons::gripFile(t.fg3, false));
  s += QString("QMenuBar { background: %1; min-height: 24px; max-height: 24px; padding: 0; }\n"
               "QMenuBar::item { padding: 0 8px; height: 24px; color: %2; background: transparent; }\n"
               "QMenuBar::item:selected { background: %3; color: %4; }\n").arg(bg, fg, bg3, fg);
  s += QString("QMenu { background: %1; border: 1px solid %2; padding: 4px; }\n"
               "QMenu::item { height: 28px; padding: 0 24px 0 32px; border-radius: 3px; }\n"
               "QMenu::item:selected { background: %3; color: %4; }\n"
               "QMenu::item:disabled { color: %5; }\n"
               "QMenu::separator { height: 1px; background: %2; margin: 4px 8px; }\n"
               "QMenu::icon { left: 8px; }\n").arg(bg3, line, sel, onsel, fg3);
  s += QString("QToolBar#ribbonHost { background: %1; border: none; padding: 0; spacing: 0; }\n").arg(bg);
  s += QString("QTabBar#ribbonTabs { background: %1; }\n"
               "QTabBar#ribbonTabs::tab { height: 28px; padding: 0 12px; margin-right: 2px; color: %2; background: transparent; border: 1px solid transparent; border-bottom: none; border-top-left-radius: 3px; border-top-right-radius: 3px; }\n"
               "QTabBar#ribbonTabs::tab:hover { background: %3; }\n"
               "QTabBar#ribbonTabs::tab:selected { background: %4; border-color: %5; color: %6; }\n").arg(bg, fg2, bg3, bg2, line, fg);
  s += QString("QWidget#ribbonStrip { background: %1; border-top: 1px solid %2; border-bottom: 1px solid %2; }\n"
               "QToolButton#ribbonTool { min-width: 56px; padding: 0 8px; border: 1px solid transparent; border-radius: 3px; font-size: 11px; color: %3; background: transparent; }\n"
               "QToolButton#ribbonTool:hover { background: %4; }\n"
               "QToolButton#ribbonTool:checked { background: %4; border-color: %2; }\n"
               "QToolButton#ribbonTool:disabled { color: %5; }\n"
               "QToolButton#ribbonSettings { border: 1px solid transparent; border-radius: 3px; background: transparent; }\n"
               "QToolButton#ribbonSettings:hover, QToolButton#ribbonSettings:pressed { background: %4; }\n"
               "QToolButton#ribbonSettings::menu-indicator { image: none; width: 0px; }\n"
               "QFrame#ribbonSep { background: %2; max-width: 1px; min-width: 1px; margin: 12px 4px; }\n"
               "QLabel#ribbonLabel { color: %6; font-size: 12px; }\n").arg(bg2, line, fg, bg3, fg3, fg2);
  s += QString("QWidget#segmented { border: 1px solid %1; border-radius: 3px; background: %2; }\n"
               "QToolButton#segment { height: 26px; padding: 0 10px; border: none; border-radius: 2px; color: %3; background: transparent; }\n"
               "QToolButton#segment:hover { background: %4; }\n"
               "QToolButton#segment:checked { background: %4; color: %5; }\n"
               "QToolButton#segmentPrimary:checked { background: %6; color: %7; }\n").arg(line, bg2, fg2, bg3, fg, sel, onsel);
  s += QString("QDockWidget { background: %1; }\n"
               "QDockWidget > QWidget { background: %1; }\n"
               "QWidget#dockHeader { background: %1; border-bottom: 1px solid %2; }\n"
               "QLabel#dockTitle { color: %3; font-weight: 500; }\n"
               "QToolButton#dockButton { border: none; background: transparent; padding: 0; }\n"
               "QToolButton#dockButton:hover { background: %4; border-radius: 3px; }\n"
               "QWidget#timelineDock { background: %1; }\n").arg(bg2, line, fg2, bg3);
  s += QString("QComboBox::down-arrow { image: url(%1); width: 12px; height: 12px; }\n"
               "QCheckBox::indicator:checked { image: url(%2); }\n"
               "QSpinBox::up-arrow, QDoubleSpinBox::up-arrow { image: url(%3); width: 10px; height: 10px; }\n"
               "QSpinBox::down-arrow, QDoubleSpinBox::down-arrow { image: url(%1); width: 10px; height: 10px; }\n"
               "QSpinBox::up-button, QDoubleSpinBox::up-button, QSpinBox::down-button, QDoubleSpinBox::down-button { border: none; background: transparent; width: 16px; }\n"
               "QTreeView::branch:has-children:!has-siblings:closed, QTreeView::branch:closed:has-children:has-siblings { image: url(%4); }\n"
               "QTreeView::branch:open:has-children:!has-siblings, QTreeView::branch:open:has-children:has-siblings { image: url(%1); }\n")
           .arg(icons::file("chevronDown", t.fg3, 12), icons::file("check", t.onsel, 12), icons::file("chevronUp", t.fg3, 12), icons::file("chevronRight", t.fg3, 12));
  s += QString("QLineEdit, QComboBox, QSpinBox, QDoubleSpinBox { background: %1; border: 1px solid %2; border-radius: 3px; height: 28px; min-height: 26px; padding: 0 8px; color: %3; selection-background-color: %4; }\n"
               "QLineEdit:focus, QComboBox:focus { border-color: %4; }\n"
               "QLineEdit#mono, QLabel#mono { font-family: '%5'; }\n"
               "QComboBox::drop-down { border: none; width: 20px; }\n"
               "QComboBox QAbstractItemView { background: %6; border: 1px solid %2; selection-background-color: %4; selection-color: %7; }\n").arg(bg2, line, fg, sel, monoF, bg3, onsel);
  s += QString("QPushButton { height: 28px; min-height: 26px; padding: 0 12px; border: 1px solid %1; border-radius: 3px; background: %2; color: %3; }\n"
               "QPushButton:hover { background: %4; }\n"
               "QPushButton:pressed { background: %5; }\n"
               "QPushButton#primary { background: %6; border-color: %6; color: %7; font-weight: 500; }\n"
               "QPushButton#primary:hover { background: %8; }\n"
               "QPushButton#outline { background: transparent; height: 22px; min-height: 20px; padding: 0 8px; font-size: 12px; }\n"
               "QPushButton:disabled { color: %9; }\n").arg(line, bg2, fg, bg3, bg4, sel, onsel, css(t.sel.lighter(115)), fg3);
  s += QString("QTreeWidget, QTreeView, QListWidget { background: transparent; border: none; outline: none; show-decoration-selected: 1; }\n"
               "QTreeWidget::item, QTreeView::item, QListWidget::item { height: 28px; border: none; }\n"
               "QTreeWidget::item:hover, QListWidget::item:hover { background: %2; }\n"
               "QTreeWidget::item:selected, QListWidget::item:selected { background: %3; color: %4; }\n"
               "QTreeWidget#browserTree { show-decoration-selected: 0; }\n"
               "QTreeWidget#browserTree::item:selected, QTreeWidget#browserTree::item:hover { background: transparent; color: %4; }\n"
               "QTreeView::branch { background: transparent; }\n"
               "QHeaderView::section { background: %1; color: %5; border: none; border-bottom: 1px solid %6; height: 24px; padding-left: 8px; font-size: 11px; text-transform: uppercase; }\n").arg(bg, bg3, selbg, fg, fg3, line);
  s += QString("QLabel#toolPanelTitle { font-weight: 500; }\n"
               "QLabel#toolPanelContext { color: %1; font-size: 12px; }\n").arg(fg3);
  s += QString("QStatusBar { background: %1; border-top: 1px solid %2; min-height: 24px; max-height: 24px; color: %3; font-size: 12px; }\n"
               "QStatusBar::item { border: none; }\n"
               "QStatusBar QLabel { color: %3; font-size: 12px; }\n").arg(bg, line, fg2);
  s += QString("QToolTip { background: %1; color: %2; border: 1px solid %3; border-radius: 3px; padding: 6px 8px; font-size: 12px; }\n").arg(bg3, fg, line);
  s += QString("QScrollBar:vertical { background: transparent; width: 10px; margin: 0; }\n"
               "QScrollBar::handle:vertical { background: %1; border-radius: 3px; min-height: 24px; margin: 2px; }\n"
               "QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; }\n"
               "QScrollBar:horizontal { background: transparent; height: 10px; }\n"
               "QScrollBar::handle:horizontal { background: %1; border-radius: 3px; min-width: 24px; margin: 2px; }\n"
               "QScrollBar::add-line:horizontal, QScrollBar::sub-line:horizontal { width: 0; }\n").arg(bg4);
  s += QString("QSlider::groove:horizontal { height: 2px; background: %1; }\n"
               "QSlider::sub-page:horizontal { background: %2; }\n"
               "QSlider::handle:horizontal { width: 12px; height: 12px; margin: -5px 0; border-radius: 6px; background: %2; border: 2px solid %3; }\n").arg(bg4, sel, bg2);
  s += QString("QCheckBox { spacing: 8px; } QCheckBox::indicator { width: 14px; height: 14px; border: 1px solid %1; border-radius: 3px; background: %2; }\n"
               "QCheckBox::indicator:checked { background: %3; border-color: %4; }\n"
               "QRadioButton::indicator { width: 14px; height: 14px; border: 1px solid %1; border-radius: 7px; background: %2; }\n"
               "QRadioButton::indicator:checked { background: %4; border: 3px solid %2; }\n").arg(line, bg2, selbg, sel);
  s += QString("QProgressBar { background: %1; border: none; border-radius: 2px; }\n"
               "QProgressBar::chunk { background: %2; border-radius: 2px; }\n").arg(bg4, sel);
  s += QString("QLabel#progressTitle { color: %1; font-weight: 500; }\n"
               "QPushButton#progressCancel { height: 18px; min-height: 18px; max-height: 18px; padding: 0 10px; font-size: 12px; border: 1px solid %2; border-radius: 3px; background: %3; color: %4; }\n"
               "QPushButton#progressCancel:hover { background: %5; }\n"
               "QPushButton#progressCancel:disabled { color: %6; }\n").arg(fg, line, bg2, fg, bg3, fg3);
  s += QString("QLabel#secondary { color: %1; font-size: 12px; } QLabel#tertiary { color: %2; font-size: 11px; }\n"
               "QLabel#sectionHeader { color: %2; font-size: 11px; letter-spacing: 0.04em; font-weight: 500; padding: 8px 0 4px 0; }\n"
               "QLabel#panelTitle { font-size: 14px; font-weight: 500; }\n"
               "QLabel#chip { background: %3; border: 1px solid %4; border-radius: 3px; padding: 2px 6px; font-size: 11px; color: %5; }\n"
               "QLabel#chipSel { background: %3; border: 1px solid %6; border-radius: 3px; padding: 2px 6px; font-size: 11px; color: %6; }\n"
               "QLabel#badge { background: %7; border-radius: 8px; padding: 1px 5px; font-size: 11px; font-family: '%8'; }\n").arg(fg2, fg3, bg2, line, fg, sel, bg4, monoF);
  s += QString("QToolButton#vpButton { background: %1; border: 1px solid %2; border-radius: 4px; padding: 0; }\n"
               "QToolButton#vpButton:hover { background: %3; }\n").arg(bg2, line, bg3);
  // The note / hand drawing editor (AnnotationEditor.cpp): pickers as chips, mono section heads, keys in the buttons.
  s += QString("QToolButton[annotationChoice=\"true\"] { background: %1; border: 1px solid %2; border-radius: 3px; padding: 0 6px; color: %3; }\n"
               "QToolButton[annotationChoice=\"true\"]:hover { background: %4; }\n"
               "QToolButton[annotationChoice=\"true\"]:checked { background: %5; border-color: %6; }\n"
               "QLabel[annotationRole=\"section\"] { color: %7; font-family: '%8'; font-size: 10px; letter-spacing: 0.06em; }\n"
               "QLabel[annotationRole=\"value\"], QLabel[annotationRole=\"key\"] { color: %7; font-family: '%8'; font-size: 10px; }\n"
               "QLabel[annotationRole=\"index\"] { color: %7; font-family: '%8'; font-size: 11px; }\n"
               "QFrame[annotationRole=\"rule\"] { background: %2; border: none; }\n"
               "QToolButton[annotationRole=\"flat\"] { border: none; background: transparent; padding: 0; border-radius: 3px; }\n"
               "QToolButton[annotationRole=\"flat\"]:hover { background: %4; }\n").arg(bg2, line, fg, bg3, selbg, sel, fg3, monoF);
  s += QString("QPlainTextEdit[annotationRole=\"text\"] { background: %1; border: 1px solid %2; border-radius: 3px; padding: 2px 4px; color: %3; }\n"
               "QPlainTextEdit[annotationRole=\"text\"]:focus { border-color: %4; }\n"
               "QPushButton[annotationRole=\"primary\"] { background: %4; border-color: %4; }\n"
               "QPushButton[annotationRole=\"primary\"]:hover { background: %6; }\n"
               "QPushButton[annotationRole=\"primary\"]:disabled { background: %7; border-color: %7; }\n"
               "QPushButton[annotationRole=\"primary\"] QLabel { color: %5; font-weight: 500; }\n"
               "QPushButton[annotationRole=\"primary\"] QLabel[annotationRole=\"key\"], QPushButton[annotationRole=\"primary\"] QLabel:disabled { color: %8; font-weight: 400; }\n"
               "QPushButton QLabel:disabled { color: %9; }\n")
           .arg(bg, line, fg, sel, onsel, css(t.sel.lighter(115)), css(QColor(t.sel.red(), t.sel.green(), t.sel.blue(), 110)),
                css(QColor(255, 255, 255, 170)), fg3);
  s += QString("QFrame[annotationRole=\"badge\"] { background: %1; border: none; }\n"
               "QFrame[annotationRole=\"badge\"] QLabel { color: %2; background: transparent; font-size: 12px; }\n").arg(sel, onsel);
  s += QString("QFrame#card { background: %1; border: 1px solid %2; border-radius: 3px; }\n"
               "QFrame#card[state=\"open\"] { border-color: %3; }\n"
               "QFrame#card[state=\"unresolved\"] { border: 1px dashed %4; }\n"
               "QFrame#card[state=\"resolved\"] { background: transparent; border-color: %2; }\n"
               "QFrame#card[state=\"resolved\"] QLabel { color: %5; }\n"
               "QFrame#card[state=\"measure\"] { border-color: %6; }\n"
               "QFrame#dropzone { border: 2px dashed %7; border-radius: 3px; background: transparent; }\n"
               "QFrame#infoCard { background: %1; border: 1px solid %2; border-radius: 3px; }\n").arg(bg3, line, amber, red, fg2, sel, fg3);
  s += QString("QWidget#overlay { background: %1; border: 1px solid %2; border-radius: 3px; }\n"
               "QLineEdit#paletteInput { height: 40px; min-height: 38px; font-size: 14px; border: none; border-bottom: 1px solid %2; border-radius: 0; background: %1; }\n"
               "QListWidget#paletteList::item { height: 28px; padding-left: 4px; }\n").arg(bg3, line);
  s += QString("QLabel#keycap { background: %1; border: 1px solid %2; border-radius: 3px; padding: 0 4px; font-family: '%3'; font-size: 11px; color: %4; }\n").arg(bg4, line, monoF, fg2);
  return s;
}

void apply(bool dark) {
  g_tokens = tokens(dark);
  const Tokens& t = g_tokens;
  qApp->setStyle(QStyleFactory::create("Fusion"));
  QPalette p;
  p.setColor(QPalette::Window, t.bg);
  p.setColor(QPalette::WindowText, t.fg);
  p.setColor(QPalette::Base, t.bg);
  p.setColor(QPalette::AlternateBase, t.bg2);
  p.setColor(QPalette::ToolTipBase, t.bg3);
  p.setColor(QPalette::ToolTipText, t.fg);
  p.setColor(QPalette::Text, t.fg);
  p.setColor(QPalette::PlaceholderText, t.fg3);
  p.setColor(QPalette::Button, t.bg2);
  p.setColor(QPalette::ButtonText, t.fg);
  p.setColor(QPalette::Highlight, t.sel);
  p.setColor(QPalette::HighlightedText, t.onsel);
  p.setColor(QPalette::Mid, t.line);
  p.setColor(QPalette::Dark, t.bg);
  p.setColor(QPalette::Light, t.bg3);
  p.setColor(QPalette::Disabled, QPalette::Text, t.fg3);
  p.setColor(QPalette::Disabled, QPalette::ButtonText, t.fg3);
  p.setColor(QPalette::Disabled, QPalette::WindowText, t.fg3);
  qApp->setPalette(p);
  qApp->setFont(ui(13));
  qApp->setStyleSheet(stylesheet(t));
  emit notifier()->changed();
}

Notifier* notifier() {
  static Notifier n;
  return &n;
}

}  // namespace theme
