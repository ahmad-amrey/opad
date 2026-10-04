#include "SketchEditor.hpp"
#include "Jobs.hpp"
#include "opad/design/sketch_reference.hpp"
#include "opad/design/feature.hpp"
#include "I18n.hpp"
#include <QCoreApplication>
#include <QCursor>
using namespace opad::design;

void SketchEditor::referenceHover() {
  const bool on=sketchkeys::referenceTool(m_tool.toStdString());
  const auto before=m_viewport->selectionFilter();
  m_viewport->setEdgeHover(on);
  if(on) {
    const auto filter=option("projectionPick","edge");
    const auto want=m_tool=="intersect_body"||m_tool=="silhouette"||filter=="body"?Viewport::SelFilter::Body:filter=="face"?Viewport::SelFilter::Face:filter=="vertex"?Viewport::SelFilter::Vertex:Viewport::SelFilter::Edge;
    m_viewport->setSelectionFilter(want);
    // A filter change (setEdgeHover passes through Edges) clears the view's selection: the picks are shown again. The same
    // filter keeps it, and then what is shown must be compared as it is: two reference tools with Edges (Project, then
    // Include) left the first one's pick highlighted after setTool dropped it.
    if(before!=Viewport::SelFilter::Edge || want!=Viewport::SelFilter::Edge)m_sourcesShown.clear();
  }
  showSources();
}
// The picked edges, faces, vertices and bodies stay highlighted as the view's selection until the tool adds them or lets
// them go (TODO 11 wave 3, P4: the guides show them in the selection colour until Enter, as Fusion's Project does); the
// preview is drawn over them (showToolPreview). Origin axes, sketches and features chosen in the panel are listed there.
void SketchEditor::showSources() {
  const QStringList shown=m_active && m_visible && sketchkeys::referenceTool(m_tool.toStdString())?m_sources:QStringList();  // a hidden sketch: none
  // Under the sketch, not X-ray: in Topmost a picked body or face was depth-tested against the sketch drawn there and hid
  // its curves inside the pick's outline until the tool let it go. Back to X-ray as soon as none is shown.
  m_viewport->setSelectionXray(shown.isEmpty());
  if(shown==m_sourcesShown)return;
  m_sourcesShown=shown;
  std::vector<opad::Ref> refs;
  for(const auto& text:shown) {
    const auto j=opad::json::parse(text.toStdString(),nullptr,false);
    if(!j.is_object() || !j.contains("body"))continue;
    try{refs.push_back(opad::Ref::from_json(j));}catch(const std::exception&){}
  }
  m_viewport->selectRefs(refs);
}
void SketchEditor::pickReference() {
  opad::Ref ref;
  // What the pointer rests on, else what is under the press: a click that came before any hover pass picked nothing.
  if(m_replayReference)ref=*m_replayReference;
  else if(!m_viewport->hoveredReference(ref) && !m_viewport->referenceAt(m_viewport->mapFromGlobal(QCursor::pos()),ref))
    return emit status(tr("Pick source geometry in the view, or choose it in the panel."));
  toggleSource(QString::fromStdString(ref.to_json().dump()));
}
// Sources accumulate, as Fusion's Project does (TODO 11 wave 3, P4): a click on one more adds it, on a picked one drops it;
// each change previews them all (a worker job, as Apply computes them), Enter or Apply adds them, Backspace takes the last
// one back and Esc drops them all.
void SketchEditor::toggleSource(const QString& source) {
  if(source.isEmpty() || !sketchkeys::referenceTool(m_tool.toStdString()))return;
  invalidatePreview();
  if(!m_sources.removeOne(source)){m_sources<<source;m_sourceAdded=source;}
  else m_sourceAdded.clear();
  emit status(m_sources.isEmpty()?tr("Pick source geometry in the view, or choose it in the panel.")
                                  :tr("%1: click more to add them, or press Enter or Apply").arg(m_sources.size()==1?tr("1 source"):tr("%1 sources").arg(m_sources.size())));
  rebuild();emit changed();emit workflowChanged();scheduleToolPreview();
}
QString SketchEditor::sourceLabel(const QString& source) const {
  opad::json j;
  try{j=opad::json::parse(source.toStdString());}catch(const std::exception&){return source;}
  if(j.contains("sketch")) {
    for(const auto& sk:m_doc->scene.sketches)if(sk.id==j.value("sketch",""))return QString::fromStdString(sk.name);
    return tr("Sketch");
  }
  if(j.contains("feature"))for(const auto& f:m_doc->scene.features)if(f.id==j.value("feature",""))return QString::fromStdString(f.name);
  if(j.contains("base"))return tr("Origin axis %1").arg(QString::fromStdString(j.value("base","")));
  if(!j.contains("body"))return source;
  const QString body=m_doc->nodeName(j.value("body",""));
  const std::string kind=j.value("kind","body");
  const int index=j.value("index",-1);
  if(kind=="face")return tr("%1 · face %2").arg(body).arg(index);
  if(kind=="edge")return tr("%1 · edge %2").arg(body).arg(index);
  if(kind=="vertex")return tr("%1 · vertex %2").arg(body).arg(index);
  return body;
}
bool SketchEditor::applyReference() {
  if(m_tool=="break_link") {
    std::vector<int> ids;for(int id:m_sel)if(const auto* e=m_sk.entity(id);e && !e->source.is_null())ids.push_back(id);
    if(m_previewRequested)return true;  // nothing to show before: the curves stay where they are
    if(ids.empty()){emit status(tr("Select linked curves first: they are drawn amber."));return true;}
    runSketchEdit(tr("Break projection link"),[ids](Sketch& sk){break_reference(sk,ids);});return true;
  }
  if(!sketchkeys::referenceTool(m_tool.toStdString()))return false;
  // Apply before a source is picked: say what is missing (it showed a JSON parse error).
  if(m_sources.isEmpty()){if(!m_previewRequested)emit status(tr("Pick source geometry in the view, or choose it in the panel."));return true;}
  try {
    std::vector<opad::json> sources;
    for(const auto& text:m_sources) {
      auto source=opad::json::parse(text.toStdString());
      if(source.value("sketch","")==m_id && !m_id.empty())throw opad::Error("a sketch cannot project itself");
      sources.push_back(std::move(source));
    }
    auto doc=std::make_shared<opad::Document>(m_doc->doc);auto scene=std::make_shared<opad::Scene>(m_doc->scene);
    const auto frame=m_frame;const bool linked=option("projectionLinked","1")=="1";
    const std::string mode=m_tool=="intersect_body"?"intersect":m_tool=="include3d"?"include":m_tool.toStdString();
    // The preview tries every source, so that a failure names the ones that gave nothing (an edge square to the plane, a
    // body the plane misses), not the last pick; Apply stops at the first.
    const QStringList texts=m_sources;const bool preview=m_previewRequested;
    auto failed=std::make_shared<QStringList>();m_sourcesFailed=failed;
    runSketchEdit(tr("Projecting geometry"),[doc,scene,frame,sources,linked,mode,texts,preview,failed](Sketch& sk)mutable{
      std::string first;
      for(size_t i=0;i<sources.size();++i) {
        auto& source=sources[i];
        try {
          if(source.contains("body"))source=make_ref(*doc,*scene,opad::Ref::from_json(source));
          const auto generated=derive_sketch(*doc,*scene,frame,source,mode);append_reference(sk,generated,source,mode,linked);
        } catch(const std::exception& e) {
          if(!preview)throw;
          if(first.empty())first=e.what();
          failed->push_back(texts.value(qsizetype(i)));
        }
      }
      if(!first.empty())throw opad::Error(first);
    });
    // Done with those sources: the tool asks for the next ones (they stayed "ready", and Apply again projected them twice).
    if(!m_previewRequested){m_sources.clear();emit workflowChanged();}
  }catch(const std::exception& e){emit status(i18n::t(QString::fromUtf8(e.what())));}
  return true;
}
