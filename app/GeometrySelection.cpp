#include "MainWindow.hpp"
#include "opad/agent.hpp"
#include "opad/inspect.hpp"
#include <QInputDialog>
#include <QStatusBar>

void MainWindow::selectGeometry() {
  if(m_doc->loading || m_doc->designBusy || m_design->sketchActive() || m_design->featureActive() || m_design->pickingPlane())
    throw opad::Error("Finish the current operation before selecting geometry.");
  const auto ids=currentNodeIds();
  if(ids.size()!=1 || !m_doc->scene.node(ids[0]) || m_doc->scene.node(ids[0])->kind!=opad::Node::Kind::Body)
    throw opad::Error("Select one body to find matching geometry.");
  const QStringList choices={tr("Top perimeter"),tr("Bottom perimeter"),tr("Edges parallel to X"),tr("Edges parallel to Y"),tr("Edges parallel to Z"),tr("Circular edges"),tr("Upward planar faces")};
  bool accepted=false;
  const auto choice=QInputDialog::getItem(this,tr("Select by geometry"),tr("Match in world coordinates:"),choices,0,false,&accepted);
  if(!accepted)return;
  const int mode=choices.indexOf(choice);const auto body=ids[0];const auto revision=m_doc->revision,generation=m_doc->generation;
  if(!m_doc->captureSnapshot(m_jobs,[this,mode,body,revision,generation](std::shared_ptr<opad::Document> document,const QString& error){
    if(!document){statusBar()->showMessage(error,6000);return;}
    auto matches=std::make_shared<std::vector<opad::Ref>>();
    m_jobs->async(tr("Select by geometry"),[document,matches,body,mode](Progress progress){
      const auto scene=opad::resolve(*document);opad::json filters;
      if(mode<2){
        const auto properties=opad::node_properties(*document,scene,body);
        filters={{"at_plane",{{"axis","z"},{"value",properties.at("bbox").at(mode==0?"max":"min").at(2)}}}};
      }else if(mode<5)filters={{"parallel_to",mode==2?"x":mode==3?"y":"z"}};
      else if(mode==5)filters={{"curve","circle"}};
      else filters={{"normal","+z"}};
      const auto result=opad::agent::query_entities(*document,scene,{{"body",body},{"kind",mode==6?"face":"edge"},{"filters",filters},{"limit",100}},[progress]{return progress.cancelled();});
      if(result.at("total").get<int>()>100)throw opad::Error("More than 100 matches. Select a smaller body or use a narrower geometric query.");
      for(const auto& item:result.at("items"))matches->push_back(opad::Ref::from_json(item.at("reference").at("ref")));
    },[this,matches,revision,generation,mode](bool ok,const QString& error){
      if(!ok){statusBar()->showMessage(error,6000);return;}
      if(m_doc->revision!=revision || m_doc->generation!=generation || m_design->ownsSelection())return;
      if(matches->empty()){statusBar()->showMessage(tr("No matching geometry."),6000);return;}
      connect(m_viewport,&Viewport::filterApplied,this,[this,matches,revision,generation]{
        if(m_doc->revision==revision && m_doc->generation==generation && !m_design->ownsSelection())m_viewport->selectRefs(*matches);
      },Qt::SingleShotConnection);
      m_viewport->setSelectionFilter(mode==6?Viewport::SelFilter::Face:Viewport::SelFilter::Edge);
      statusBar()->showMessage(tr("%1 matching entities selected.").arg(matches->size()),5000);
    });
  }))throw opad::Error("Document is busy; try again shortly.");
}
