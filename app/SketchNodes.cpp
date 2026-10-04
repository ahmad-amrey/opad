#include "SketchEditor.hpp"
#include "Jobs.hpp"
#include "opad/design/sketch_edit.hpp"
#include "I18n.hpp"
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QLabel>
#include <QPointer>
#include <QVBoxLayout>
#include <algorithm>

void SketchEditor::insertSplineNode(double u,double v) {
  if(!m_active) return;
  const opad::design::SkEntity* e=nullptr;double nearest=tol();
  for(const auto& candidate:m_sk.entities) if(candidate.type==opad::design::SkEntity::Type::Spline) {const double d=distanceTo(candidate,u,v);if(d<nearest) {nearest=d;e=&candidate;}}
  if(!e || e->type!=opad::design::SkEntity::Type::Spline) return emit status(tr("Alt-click inside a spline to insert a node."));
  begin_change();
  try {const int point=opad::design::insert_spline_node(m_sk,e->id,u,v);m_sel={point};end_change(tr("Insert spline node"));}
  catch(const std::exception& error) {cancel_change();emit status(i18n::t(QString::fromUtf8(error.what())));}
}

void SketchEditor::editSplineNode() {
  if(!m_active || m_sel.size()!=1 || (m_tool!="select" && m_tool!="spline")) return emit status(tr("Select one spline node first."));
  const int point=m_sel.front();int entity=0,index=-1;
  for(const auto& e:m_sk.entities) if(e.type==opad::design::SkEntity::Type::Spline && e.degree && !e.fixed) {
    const auto it=std::find(e.p.begin(),e.p.end(),point);if(it!=e.p.end()) {entity=e.id;index=int(it-e.p.begin());break;}
  }
  if(!entity) return emit status(tr("Choose a control node on an editable spline."));
  const auto* spline=m_sk.entity(entity);
  m_options["weight"]=QString::number(spline->weights[index],'g',12);
  m_options["incoming"]=QString::number(index>0?spline->weights[index-1]:1,'g',12);
  m_options["outgoing"]=QString::number(index+1<int(spline->p.size())?spline->weights[index+1]:1,'g',12);
  setTool("node");

}

void SketchEditor::findOpenVertices() {
  if(!m_active) return;
  auto snapshot=std::make_shared<opad::design::Sketch>(m_sk);auto result=std::make_shared<std::vector<int>>();
  QPointer<SketchEditor> guard(this);
  m_jobs->async(tr("Finding open sketch ends"),[snapshot,result](Progress progress){if(!progress.cancelled())*result=opad::design::dangling_vertices(*snapshot);},[this,guard,snapshot,result](bool ok,const QString& error){
    if(!guard || !m_active || snapshot->to_json()!=m_sk.to_json()) return;
    if(!ok) return emit status(error);
    m_dangling={result->begin(),result->end()};m_sel=*result;rebuild();emit changed();
    emit status(result->empty()?tr("All curve endpoints meet another curve or form a closed curve."):tr("%1 open endpoints highlighted in red.").arg(result->size()));
  });
}
