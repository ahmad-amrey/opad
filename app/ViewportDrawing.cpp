// The 2D vocabulary of the view (UI-118): drawings spoken of as objects on layers. The ink, line weights and types of a
// drawing's bodies are its looks (ViewportLooks.cpp, ViewportSettings.cpp); the model is Drawing2D.hpp.
#include "Viewport.hpp"

void Viewport::setDrawingWords(bool on) {
  if (m_drawingWords == on) return;
  m_drawingWords = on;
  m_hoverOwner = nullptr;  // the next frame words the hovered entity again
  requestRedraw();
}

QString Viewport::drawingWord(const std::string& type) {
  if (type == "line") return tr("Line");
  if (type == "arc") return tr("Arc");
  if (type == "circle") return tr("Circle");
  if (type == "ellipse") return tr("Ellipse");
  if (type == "spline") return tr("Spline");
  if (type == "fill") return tr("Fill");
  if (type == "point") return tr("Point");
  if (type == "group") return tr("Group");
  return tr("Curve");
}

bool Viewport::benchHover(const QPointF& at) {
  if (!m_initialised) return false;
  m_view->Redraw();  // the picker clips to the camera range a frame sets
  const QPointF scale = viewScale();
  m_ctx->MoveTo(qRound(at.x() * scale.x()), qRound(at.y() * scale.y()), m_view, Standard_False);
  updateHover();
  return m_ctx->HasDetected();
}
