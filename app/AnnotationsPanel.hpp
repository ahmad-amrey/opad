#pragma once
#include <QComboBox>
#include <QLabel>
#include <QWidget>
#include <string>
#include <vector>

#include "AppDocument.hpp"

// ---------------------------------------------------------------- annotations
class AnnotationsPanel : public QWidget {
  Q_OBJECT
 public:
  explicit AnnotationsPanel(AppDocument* doc, QWidget* parent = nullptr);
  std::string currentOpId() const { return m_current; }
 signals:
  // A card was clicked: what its note is anchored to, or every pick of a pinned measurement, to light up in the view (help
  // audit P9.4).
  void targetRequested(const std::vector<opad::Ref>& anchors);
  void addRequested();
  void typeFilterChanged(const std::string& type);
  void resolveRequested(const std::string& opId);
  void restoreRequested(const std::string& opId);
  void styleRequested(const std::string& opId, const std::string& style);  // re-tag: an edit op
 public slots:
  void rebuild();
 private:
  void markCurrent();  // the card of m_current drawn as the current one (what Resolve acts on)
  AppDocument* m_doc;
  QComboBox* m_author;
  QComboBox* m_type;
  QComboBox* m_status;
  QLabel* m_count;
  QWidget* m_cards;
  std::string m_current;
};
