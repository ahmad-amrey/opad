#include "Motion.hpp"

#include <QSettings>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace motion {

bool system() {
#ifdef Q_OS_WIN
  BOOL enabled = TRUE;
  if (SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &enabled, 0)) return enabled;
#endif
  return true;
}

bool reduced() { return QSettings().value("ui/reduceMotion", !system()).toBool(); }

void setReduced(bool on) { QSettings().setValue("ui/reduceMotion", on); }

// Never zero: an OCCT animation of no length stays at its start (AIS_Animation::Update normalises by its own duration).
double seconds(double s) { return reduced() ? 0.001 : s; }

int milliseconds(int ms) { return reduced() ? 0 : ms; }

}  // namespace motion
