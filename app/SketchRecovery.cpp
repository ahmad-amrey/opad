#include "SketchEditor.hpp"

opad::json SketchEditor::agentContext() const {
  opad::json selected=opad::json::array();
  for(size_t i=0;i<std::min(size_t(100),m_sel.size());++i)selected.push_back(m_sel[i]);
  return {{"id",m_id},{"name",m_name.toStdString()},{"frame",m_frame.to_json()},{"plane",m_plane},
    {"edit_revision",m_modelRevision},{"degrees_of_freedom",m_solved.dof},{"selected_entities",selected},
    {"selected_total",m_sel.size()},{"entities",m_sk.entities.size()},{"points",m_sk.points.size()},
    {"modified",m_modified},{"visible",m_visible},{"write_policy","Finish or cancel the active editor before agent edits."}};
}
#include "Jobs.hpp"

void SketchEditor::captureRecovery(std::function<void(opad::json,const QString&)> done) {
  if(!m_active){done({},{});return;}
  if(busy() || m_dragging || m_inChange){done({},tr("Wait for the sketch operation to finish."));return;}
  const auto session=m_session,revision=m_modelRevision;
  auto copy=std::make_shared<opad::design::Sketch>();
  auto state=std::make_shared<opad::json>(opad::json{{"type","sketch"},{"id",m_id},{"name",m_name.toStdString()},
    {"plane",m_plane},{"frame",m_frame.to_json()},{"visible",m_visible},{"modified",m_modified}});
  if(m_id.empty() && !m_doc->activeComponent().empty())(*state)["component"]=m_doc->activeComponent();  // a new sketch goes there on Finish (UI-33)
  auto index=std::make_shared<size_t>(0);auto valid=std::make_shared<bool>(true);
  const auto points=m_sk.points.size(),entities=m_sk.entities.size(),constraints=m_sk.constraints.size(),images=m_sk.images.size(),patterns=m_sk.patterns.size();
  const auto total=points+entities+constraints+images+patterns;
  copy->points.reserve(points);copy->entities.reserve(entities);copy->constraints.reserve(constraints);copy->id_watermark=m_sk.id_watermark;
  QPointer<SketchEditor> self(this);
  m_jobs->sliced(tr("Capturing sketch"),[=,this](Job& job){
    if(!m_active || session!=m_session || revision!=m_modelRevision || busy() || m_dragging || m_inChange){*valid=false;return false;}
    if(*index>=total)return false;
    size_t i=(*index)++;
    if(i<points)copy->points.push_back(m_sk.points[i]);
    else if((i-=points)<entities)copy->entities.push_back(m_sk.entities[i]);
    else if((i-=entities)<constraints)copy->constraints.push_back(m_sk.constraints[i]);
    else if((i-=constraints)<images)copy->images.push_back(m_sk.images[i]);
    else {i-=images;copy->patterns.push_back(m_sk.patterns[i]);}
    if(*index%512==0)job.setPhase(tr("Capturing sketch"),int(100*(*index)/std::max<size_t>(1,total)));
    return *index<total;
  },[=,this](bool ok){
    if(!self)return;
    if(!ok || !*valid || session!=m_session || revision!=m_modelRevision){done({},tr("Sketch changed during capture; retrying later."));return;}
    m_jobs->async(tr("Preparing sketch recovery"),[copy,state](Progress){(*state)["geometry"]=copy->to_json();},
      [self,state,done](bool complete,const QString& error){if(self)done(complete?std::move(*state):opad::json{},error);});
  });
}

void SketchEditor::restoreRecovery(const opad::json& state) {
  const auto id=state.value("id",std::string());
  const auto* original=m_doc->scene.sketch(id);
  const auto initial=original?original->geometry:opad::json::object();
  begin(original?id:std::string(),QString::fromStdString(state.value("name","Recovered sketch")),
    state.at("plane"),opad::Frame::from_json(state.at("frame")),state.at("geometry"));
  m_initialGeometry=initial;m_modified=true;
  setVisible(state.value("visible",true));emit changed();
}
