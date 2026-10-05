#include "AnnotationsPanel.hpp"

#include <QFrame>
#include <QHBoxLayout>
#include <QPushButton>
#include <QScrollArea>
#include <QStyle>
#include <QVBoxLayout>

#include <set>

#include "I18n.hpp"
#include "KeyText.hpp"
#include "Notes.hpp"
#include "Theme.hpp"
#include "Units.hpp"

// ---------------------------------------------------------------- AnnotationsPanel
AnnotationsPanel::AnnotationsPanel(AppDocument* doc, QWidget* parent) : QWidget(parent), m_doc(doc) {
  connect(units::notifier(), &units::Notifier::changed, this, &AnnotationsPanel::rebuild);  // pinned values in the shown unit
  auto* layout = new QVBoxLayout(this);
  layout->setContentsMargins(12, 8, 12, 8);
  layout->setSpacing(8);
  auto* bar = new QHBoxLayout();
  bar->setSpacing(8);
  m_author = new QComboBox(this);
  m_type = new QComboBox(this);m_type->setObjectName("annotationTypeFilter");
  m_type->addItem(tr("All types"),QString());
  for(const auto& s:notes::styles()) m_type->addItem(i18n::t(s.label),QString::fromLatin1(s.id));
  m_status = new QComboBox(this);
  m_status->addItems({tr("All"), tr("Open"), tr("Unresolved"), tr("Resolved")});
  m_count = new QLabel(this);
  m_count->setObjectName("secondary");
  bar->addWidget(m_author, 1);
  bar->addWidget(m_type);
  bar->addWidget(m_status);
  bar->addWidget(m_count);
  layout->addLayout(bar);
  auto* scroll = new QScrollArea(this);
  scroll->setWidgetResizable(true);
  scroll->setFrameShape(QFrame::NoFrame);
  m_cards = new QWidget(scroll);
  auto* cl = new QVBoxLayout(m_cards);
  cl->setContentsMargins(0, 0, 0, 0);
  cl->setSpacing(8);
  cl->addStretch();
  scroll->setWidget(m_cards);
  layout->addWidget(scroll, 1);
  auto* add = new QPushButton(this);
  auto label = [add] {  // Note's key now (keys::notifier), none without one
    const QString key = keys::text("annotate.add");
    add->setText(key.isEmpty() ? tr("Add note") : tr("Add note") + QStringLiteral("   ") + key);
  };
  label();
  connect(keys::notifier(), &keys::Notifier::changed, add, label);
  add->setObjectName("primary");
  layout->addWidget(add);
  connect(add, &QPushButton::clicked, this, &AnnotationsPanel::addRequested);
  connect(m_type,&QComboBox::currentIndexChanged,this,[this]{rebuild();emit typeFilterChanged(m_type->currentData().toString().toStdString());});
  connect(m_author, &QComboBox::currentIndexChanged, this, [this](int) { rebuild(); });
  connect(m_status, &QComboBox::currentIndexChanged, this, [this](int) { rebuild(); });
  connect(doc, &AppDocument::changed, this, &AnnotationsPanel::rebuild);
  connect(theme::notifier(), &theme::Notifier::changed, this, &AnnotationsPanel::rebuild);
}

void AnnotationsPanel::rebuild() {
  QString currentAuthor = m_author->currentText();
  std::set<std::string> authors;
  std::set<std::string> deleted(m_doc->scene.deleted_ops.begin(), m_doc->scene.deleted_ops.end());
  std::set<std::string> removed;
  for(const auto& op:m_doc->doc.ops) if(op.type=="delete" && !deleted.count(op.id) && op.data.value("reason","")=="annotation_deleted") removed.insert(op.data.value("target",""));
  std::set<std::string> unresolved;
  for (const auto& a : m_doc->scene.annotations) if (a.unresolved) unresolved.insert(a.id);
  for (const auto& m : m_doc->scene.measurements) if(m.unresolved) unresolved.insert(m.id);
  for (const auto& op : m_doc->doc.ops) if (op.type == "annotation" || op.type == "measurement") authors.insert(op.data.value("by", ""));
  m_author->blockSignals(true);
  m_author->clear();
  m_author->addItem(tr("All authors"));
  for (const auto& a : authors) m_author->addItem(QString::fromStdString(a));
  int idx = m_author->findText(currentAuthor);
  m_author->setCurrentIndex(idx < 0 ? 0 : idx);
  m_author->blockSignals(false);
  std::string filterAuthor = m_author->currentIndex() > 0 ? m_author->currentText().toStdString() : std::string();
  int status = m_status->currentIndex();

  auto* cl = static_cast<QVBoxLayout*>(m_cards->layout());
  while (cl->count() > 1) {
    QLayoutItem* it = cl->takeAt(0);
    delete it->widget();
    delete it;
  }
  int total = 0, shown = 0;
  for (const auto& op : m_doc->doc.ops) {
    if ((op.type != "annotation" && op.type != "measurement") || op.data.contains("reply_to")) continue;
    if(removed.count(op.id)) continue;
    ++total;
    std::string by = op.data.value("by", "");
    bool resolved = deleted.count(op.id) > 0, unres = unresolved.count(op.id) > 0;
    QString state = resolved ? "resolved" : unres ? "unresolved" : "open";
    if (!filterAuthor.empty() && by != filterAuthor) continue;
    if ((status == 1 && state != "open") || (status == 2 && state != "unresolved") || (status == 3 && state != "resolved")) continue;
    opad::Ref anchor;
    try { anchor = opad::Ref::from_json(op.type=="measurement"?op.data.at("refs").at(0):op.data.at("anchor")); } catch (...) {}
    std::vector<opad::Ref> targets;  // what a click lights up: the note's anchor, every pick of a pinned measurement
    if (op.type == "measurement")
      try { for (const auto& r : op.data.at("refs")) targets.push_back(opad::Ref::from_json(r)); } catch (...) {}
    NoteInfo n;
    n.id = op.id; n.by = by; n.ts = op.data.value("ts", ""); n.text = op.data.value("text", ""); n.body = anchor.body;
    n.style = op.data.value("style", "note");
    for (const auto& a : m_doc->scene.annotations) if (a.id == op.id) { n.style = a.style; n.text = a.text; n.comments = a.comments; anchor = a.anchor; n.body = anchor.body; }  // after edits (a re-pick)
    if(op.type=="measurement") {
      n.measurement=true;
      const auto result=op.data.value("result",opad::json::object());
      n.value=tr("%1 measurement").arg(i18n::t(QString::fromStdString(op.data.value("kind",""))));
      const std::string unit=result.contains("unit") && result["unit"].is_string()?result["unit"].get<std::string>():"mm";  // mm, deg or mm2 (an area)
      if(result.contains("value") && result["value"].is_number()) n.value+=" - "+units::format(unit=="deg"?units::Kind::Angle:unit=="mm2"?units::Kind::Area:units::Kind::Length,result["value"].get<double>());
      if(unit=="mm2" && result.contains("perimeter") && result["perimeter"].is_number()) n.value+=" · "+tr("perimeter %1").arg(units::format(units::Kind::Length,result["perimeter"].get<double>()));
      else if(result.contains("size") && result["size"].is_array() && result["size"].size()==3) n.value+=" - "+units::vector(units::Kind::Length,result["size"].get<std::array<double,3>>());
      for(const auto& m:m_doc->scene.measurements) if(m.id==op.id) {n.text=m.text;n.style=m.style;n.comments=m.comments;if(!m.refs.empty())targets=m.refs;}
    }
    if (targets.empty()) targets.push_back(anchor);
    n.state = state;
    if(!m_type->currentData().toString().isEmpty() && n.style!=m_type->currentData().toString().toStdString()) continue;
    ++shown;
    n.target = anchor.kind == opad::Ref::Kind::Point ? tr("point") : m_doc->nodeName(anchor.body);
    if (anchor.kind != opad::Ref::Kind::Body && anchor.kind != opad::Ref::Kind::Point) n.target += QString(" › %1 %2").arg(i18n::t(opad::Ref::kind_name(anchor.kind))).arg(anchor.index);
    auto* card = new NoteCard(n, m_cards, m_doc);
    // The current card (the one clicked last, which Resolve acts on) is ringed in the selection colour.
    card->setStyleSheet(card->styleSheet() + QString("QFrame#card[current=\"true\"] { border: 2px solid %1; }").arg(theme::current().sel.name()));
    card->setProperty("current", n.id == m_current);
    connect(card, &NoteCard::resolveRequested, this, &AnnotationsPanel::resolveRequested);
    connect(card, &NoteCard::restoreRequested, this, &AnnotationsPanel::restoreRequested);
    connect(card, &NoteCard::styleRequested, this, &AnnotationsPanel::styleRequested);
    // A click shows what the note is pinned to: the face, edge, point or body (help audit P9.4), not its whole body.
    connect(card, &NoteCard::pressed, this, [this, card, targets] {
      m_current = card->note().id;
      markCurrent();
      emit targetRequested(targets);
    });
    cl->insertWidget(cl->count() - 1, card);
  }
  m_count->setText(tr("%1 of %2").arg(shown).arg(total));
}

void AnnotationsPanel::markCurrent() {
  for (NoteCard* card : m_cards->findChildren<NoteCard*>()) {
    const bool current = card->note().id == m_current;
    if (card->property("current").toBool() == current) continue;
    card->setProperty("current", current);
    card->style()->unpolish(card);
    card->style()->polish(card);
    card->update();
  }
}
