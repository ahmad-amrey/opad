#include "SketchEditor.hpp"
#include "Jobs.hpp"
#include "opad/design/sketch_reference.hpp"
#include "opad/design/feature.hpp"
#include "opad/scene.hpp"
#include "AppDocument.hpp"
#include <QPointer>
#include <QCoreApplication>
#include <QCursor>
using namespace opad::design;

void SketchEditor::referenceHover() {
  const bool on=m_tool=="project"||m_tool=="intersect_body"||m_tool=="silhouette"||m_tool=="include3d";
  m_viewport->setEdgeHover(on);
  if(on) {
    const auto filter=option("projectionPick","edge");
    m_viewport->setSelectionFilter(m_tool=="intersect_body"||m_tool=="silhouette"||filter=="body"?Viewport::SelFilter::Body:filter=="face"?Viewport::SelFilter::Face:filter=="vertex"?Viewport::SelFilter::Vertex:Viewport::SelFilter::Edge);
  }
}
void SketchEditor::pickReference() {
  opad::Ref ref;
  // What the pointer rests on, else what is under the press: a click that came before any hover pass picked nothing.
  if(!m_viewport->hoveredReference(ref) && !m_viewport->referenceAt(m_viewport->mapFromGlobal(QCursor::pos()),ref))
    return emit status(tr("Pick source geometry in the view, or choose it in the panel."));
  m_options["projectionSource"]=QString::fromStdString(ref.to_json().dump());
  emit status(tr("Source picked. Choose linked or editable copy, then Apply."));emit workflowChanged();
}
bool SketchEditor::applyReference() {
  if(m_tool=="break_link") {
    const auto ids=m_sel;runSketchEdit(tr("Break projection link"),[ids](Sketch& sk){break_reference(sk,ids);});return true;
  }
  if(m_tool!="project"&&m_tool!="intersect_body"&&m_tool!="silhouette"&&m_tool!="include3d")return false;
  // Apply before a source is picked: say what is missing (it showed a JSON parse error).
  if(option("projectionSource").isEmpty()){if(!m_previewRequested)emit status(tr("Pick source geometry in the view, or choose it in the panel."));return true;}
  try {
    auto source=opad::json::parse(option("projectionSource").toStdString());
    if(source.value("sketch","")==m_id && !m_id.empty())throw opad::Error("a sketch cannot project itself");
    auto doc=std::make_shared<opad::Document>(m_doc->doc);auto scene=std::make_shared<opad::Scene>(m_doc->scene);
    const auto frame=m_frame;const bool linked=option("projectionLinked","1")=="1";
    const std::string mode=m_tool=="intersect_body"?"intersect":m_tool=="include3d"?"include":m_tool.toStdString();
    runSketchEdit(tr("Projecting geometry"),[doc,scene,frame,source,linked,mode](Sketch& sk)mutable{
      if(source.contains("body"))source=make_ref(*doc,*scene,opad::Ref::from_json(source));
      const auto generated=derive_sketch(*doc,*scene,frame,source,mode);append_reference(sk,generated,source,mode,linked);
    });
    // Done with that source: the tool asks for the next one (it stayed "ready", and Apply again projected it twice).
    if(!m_previewRequested){m_options.remove("projectionSource");emit workflowChanged();}
  }catch(const std::exception& e){emit status(QString::fromUtf8(e.what()));}
  return true;
}
bool SketchEditor::projectSources(const std::vector<opad::json>& sources,bool linked) {
  if(!m_active||m_editJob||sources.empty())return false;
  QPointer<SketchEditor> guard(this);const auto frame=m_frame;const int session=m_session;
  return m_doc->captureSnapshot(m_jobs,[this,guard,frame,session,sources,linked](std::shared_ptr<opad::Document> copy,const QString& error){
    if(!guard||!m_active||m_session!=session)return;
    if(!copy)return emit status(error);
    runSketchEdit(tr("Projecting geometry"),[copy,frame,sources,linked](Sketch& sk){
      const opad::Scene scene=opad::resolve(*copy);
      for(const auto& source:sources)append_reference(sk,derive_sketch(*copy,scene,frame,source,"project"),source,"project",linked);
    });
  });
}
