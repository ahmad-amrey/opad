#pragma once
// The value card beside the pointer on the sheet canvas (TODO 11 UI-79, UI-82, UI-122): the fields a step takes, the focused
// one framed, typed values in the text colour and what the pointer gives in grey. The annotation and view tools own one each.
#include <QFontMetrics>
#include <QPainter>
#include <QWidget>

#include <algorithm>
#include <vector>

#include "Theme.hpp"

class SheetValueCard : public QWidget {
 public:
  struct Cell {
    QString label, text;
    bool typed = false, focused = false;
  };
  explicit SheetValueCard(QWidget* parent) : QWidget(parent) {
    setAttribute(Qt::WA_TransparentForMouseEvents);
    setFocusPolicy(Qt::NoFocus);
    setLayoutDirection(Qt::LeftToRight);  // numbers keep their order under right-to-left
    hide();
  }
  void set(std::vector<Cell> cells) {
    m_cells = std::move(cells);
    const QFontMetrics fm(theme::ui(12)), fb(theme::ui(12, QFont::DemiBold));
    int w = 8;
    for (const auto& c : m_cells) w += fm.horizontalAdvance(c.label) + 5 + std::max(36, fb.horizontalAdvance(c.text) + 14) + 9;
    resize(w, 30);
    update();
  }

 protected:
  void paintEvent(QPaintEvent*) override {
    const Tokens& t = theme::current();
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(QPen(t.line, 1));
    p.setBrush(t.bg2);
    p.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), 5, 5);
    const QFont font = theme::ui(12), bold = theme::ui(12, QFont::DemiBold);
    const QFontMetrics fm(font), fb(bold);
    int x = 8;
    for (const auto& c : m_cells) {
      p.setFont(font);
      p.setPen(t.fg2);
      const int lw = fm.horizontalAdvance(c.label);
      p.drawText(QRect(x, 0, lw, height()), Qt::AlignVCenter | Qt::AlignLeft, c.label);
      x += lw + 5;
      const int bw = std::max(36, fb.horizontalAdvance(c.text) + 14);
      const QRectF box(x, 5, bw, height() - 10);
      p.setPen(c.focused ? QPen(t.sel, 1.5) : QPen(t.line, 1));
      p.setBrush(t.bg);
      p.drawRoundedRect(box, 3, 3);
      p.setFont(c.typed ? bold : font);
      p.setPen(c.typed ? t.fg : t.fg3);
      p.drawText(box, Qt::AlignCenter, c.text);
      x += bw + 9;
    }
  }

 private:
  std::vector<Cell> m_cells;
};
