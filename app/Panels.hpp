#pragma once
// Secondary UI, styled per design_handoff_opad_desktop_ui: browser (F27), properties (F24), annotations (F32),
// section tab (F20), timeline (F28), command search (F30), shortcut editor (F31). One header per class
// (BrowserPanel.hpp, ToolPanel.hpp, ...); this umbrella includes them all, and what it included before the split.
#include <QComboBox>
#include <QDialog>
#include <QFrame>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QScrollArea>
#include <QSlider>
#include <QStyledItemDelegate>
#include <QTimer>
#include <QToolButton>
#include <QTreeWidget>
#include <QWidget>
#include <functional>
#include <set>
#include <unordered_map>
#include <string>
#include <vector>

#include "AppDocument.hpp"
#include "Theme.hpp"
#include "ShortcutEditor.hpp"

#include "AnnotationsPanel.hpp"
#include "BrowserPanel.hpp"
#include "CommandPalette.hpp"
#include "DockHeader.hpp"
#include "LoadShade.hpp"
#include "ProgressStrip.hpp"
#include "PropertiesPanel.hpp"
#include "SectionPanel.hpp"
#include "TimelineWidget.hpp"
#include "ToolPanel.hpp"
#include "ViewportChips.hpp"
