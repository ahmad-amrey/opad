#pragma once
// Procedural animated clips for the command help (UI-107, design notes B §4). app/help/clips.json describes each clip
// as an ordered list of vector items (grid, curves, extruded and turned solids, dimensions, a cursor with click
// ripples, value boxes, key caps, constraint glyphs, snap markers, cards, chips) whose properties are keyframed with
// easing; templates ("use") share whole families (constraints, picks, handle drags, typed values). Everything is
// painted with QPainter from the theme tokens on every frame: no media files, all artwork original, sharp at any
// scale. The scene is model space and never mirrored; the caption bar, cards, chips and key caps follow the layout
// direction, and numbers keep their order (LRE..PDF). ClipView plays one clip: a 30 fps timer only while visible, the
// whole clip or one step's segment (a tool panel's waiting step), the hold-and-fade loop, and the still frame with
// the clicks numbered when motion is reduced (ui/tipAnimate, by default the system's animation setting).
#include <QElapsedTimer>
#include <QImage>
#include <QList>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QWidget>

class QPainter;
struct Tokens;

namespace clips {
struct Step {
  double from = 0, to = 0;
  QString caption;  // English; translated when painted
};
struct Options {
  const Tokens* tokens = nullptr;  // theme::current() when null (contact sheets render light and dark)
  bool still = false;              // the still frame: the clicks numbered, every caption in the bar
  bool rtl = false;                // the caption bar, cards, chips and key caps mirrored
  bool caption = true;             // the caption bar under the scene
};
// Loads app/help/clips.json (embedded, help.qrc; <app dir>/help/clips.json laid over it by id); `path` reads only that
// file (tests). Loaded on first use.
void load(const QString& path = QString());
QStringList ids();
bool has(const QString& id);
QStringList problems();  // what the last load found wrong: unknown element, property, colour, template or parameter
double duration(const QString& id);
double stillTime(const QString& id);
QList<Step> steps(const QString& id);
int stepAt(const QString& id, double t);
QStringList texts(const QString& id);  // the translatable English texts (captions, labels, chips, cards)
// The clip steps [first, last] a tool panel loops while the tool waits for its step `step` of `count` (a tool lists its
// own steps, which need not match the clip's): the clip's "guide" entry for that step when it has one, else the clip's
// steps shared out evenly; once every step is done (step >= count) the last one. {-1, -1}: no such clip.
QPair<int, int> guideRange(const QString& id, int step, int count);
// The frame at t seconds into r (clipped to its rounded corners).
void paint(QPainter& p, const QRectF& r, const QString& id, double t, const Options& o = {});
QImage frame(const QString& id, double t, QSize size, qreal dpr = 1, const Options& o = {});
// ui/tipAnimate; by default the system's animation setting (Windows: client area animation).
bool animations();
}  // namespace clips

class ClipView : public QWidget {
  Q_OBJECT
 public:
  static constexpr double kHold = 0.6, kFade = 0.2;  // the loop holds the last frame, then fades to the first
  explicit ClipView(const QString& clip = QString(), QWidget* parent = nullptr);
  void setClip(const QString& id);
  QString clip() const { return m_clip; }
  // Loops one step's segment (a tool panel's waiting step); -1 plays the whole clip.
  void setStep(int step) { setRange(step, step); }
  void setRange(int first, int last);  // steps first..last as one segment
  int step() const { return m_first; }
  QPair<int, int> range() const { return {m_first, m_last}; }
  bool playing() const { return m_timer.isActive(); }
  bool still() const { return !clips::animations(); }
  double time() const;  // seconds into the clip of the frame shown
  QSize sizeHint() const override { return {288, 162}; }

 protected:
  void paintEvent(QPaintEvent*) override;
  void showEvent(QShowEvent*) override;
  void hideEvent(QHideEvent*) override;

 private:
  void sync();
  double phase(double& first, double& fade) const;
  QString m_clip;
  int m_first = -1, m_last = -1;
  QTimer m_timer;
  QElapsedTimer m_clock;
};

class QToolButton;
// The guide slot of a tool panel (ToolStepsPanel, SketchPanel, FeaturePanel; design notes B §4): the running command's
// clip, looping the segment of the step the tool waits for. Its header folds it; folded or not is remembered per
// command (help/guide/<id>), and a command run more than kUses times starts folded (help/uses/<id>). No slot for a
// command without a clip, nor with ui/toolGuide off.
class ToolGuide : public QWidget {
  Q_OBJECT
 public:
  static constexpr int kUses = 5, kHeight = 150;
  explicit ToolGuide(QWidget* parent = nullptr);
  void setCommand(const QString& id);    // a new run of the command (counted); empty: no slot
  void setWaiting(int step, int count);  // the tool waits for its step `step` of `count` (count when all are done)
  QString command() const { return m_id; }
  bool shown() const { return !isHidden(); }
  bool expanded() const;
  void setExpanded(bool on);  // and remembered for the command
  ClipView* view() const { return m_view; }
  static bool enabled();  // ui/toolGuide, on by default
 signals:
  void resized();  // shown, hidden, folded or unfolded: the panel fits itself again

 private:
  void sync();
  QString m_id;
  QToolButton* m_head;
  ClipView* m_view;
  int m_step = 0, m_count = 0;
};
