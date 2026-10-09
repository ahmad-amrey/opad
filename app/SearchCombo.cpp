#include "SearchCombo.hpp"

#include <QComboBox>
#include <QCompleter>
#include <QLineEdit>

namespace search_combo {

void enable(QComboBox* combo, bool freeText) {
  if (!combo || combo->property("searchable").toBool()) return;
  combo->setProperty("searchable", true);
  const QString placeholder = combo->isEditable() ? combo->lineEdit()->placeholderText() : QString();
  combo->setEditable(true);
  combo->setInsertPolicy(QComboBox::NoInsert);  // what is typed searches, it never becomes an item
  auto* completer = new QCompleter(combo->model(), combo);
  completer->setCaseSensitivity(Qt::CaseInsensitive);
  completer->setFilterMode(Qt::MatchContains);
  completer->setCompletionMode(QCompleter::PopupCompletion);
  completer->setMaxVisibleItems(12);
  combo->setCompleter(completer);
  QObject::connect(completer, qOverload<const QString&>(&QCompleter::activated), combo, [combo](const QString& text) {
    const int at = combo->findText(text, Qt::MatchFixedString);
    if (at < 0) return;
    combo->setCurrentIndex(at);
    emit combo->activated(at);  // as a click on it in the list
  });
  if (!freeText)
    QObject::connect(combo->lineEdit(), &QLineEdit::editingFinished, combo, [combo] {
      const int at = combo->findText(combo->lineEdit()->text(), Qt::MatchFixedString);
      if (at >= 0 && at != combo->currentIndex()) {
        combo->setCurrentIndex(at);
        emit combo->activated(at);
      } else if (combo->currentIndex() >= 0) {
        combo->lineEdit()->setText(combo->itemText(combo->currentIndex()));  // no item of that name: the chosen one stays
      }
    });
  combo->lineEdit()->setPlaceholderText(placeholder.isEmpty() ? QObject::tr("Type to search") : placeholder);
}

}  // namespace search_combo
