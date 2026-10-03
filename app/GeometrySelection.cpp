#include "MainWindow.hpp"
#include "opad/agent.hpp"
#include "opad/geometry.hpp"
#include "opad/inspect.hpp"
#include <QInputDialog>
#include <QStatusBar>
#include <algorithm>
#include <cmath>
#include <set>
#include <tuple>

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

namespace {
QString amount(double v) { return QString::number(std::round(v * 1000) / 1000, 'g', 10); }
// What a rule selected, in the user's language (the core's labels are English data).
QString similarText(const opad::Recognized& r) {
  const auto& p = r.params;
  auto value = [&](const char* key) { return p.contains(key) && p[key].is_number() ? amount(p[key].get<double>()) : QString(); };
  if (r.rule == "hole") return p.value("through", false) ? MainWindow::tr("Holes Ø%1 through").arg(value("diameter")) : MainWindow::tr("Holes Ø%1").arg(value("diameter"));
  if (r.rule == "fillet") return MainWindow::tr("Fillets R%1").arg(value("radius"));
  if (r.rule == "chamfer") return MainWindow::tr("Chamfers %1 mm").arg(value("distance"));
  if (r.rule == "wall") return MainWindow::tr("Walls %1 mm").arg(value("thickness"));
  if (r.rule == "radius") return r.faces.empty() ? MainWindow::tr("Circular edges R%1").arg(value("radius")) : MainWindow::tr("Round faces R%1").arg(value("radius"));
  if (r.rule == "angle") return MainWindow::tr("Conical faces %1°").arg(value("angle"));
  if (r.rule == "normal") return MainWindow::tr("Faces facing the same way");
  if (r.rule == "area") return MainWindow::tr("Faces of the same area");
  if (r.rule == "direction") return MainWindow::tr("Parallel edges");
  return MainWindow::tr("Edges of the same length");
}
bool sameRefs(std::vector<opad::Ref> a, std::vector<opad::Ref> b) {
  auto key = [](std::vector<opad::Ref>& v) { std::sort(v.begin(), v.end(), [](const opad::Ref& x, const opad::Ref& y) { return std::tie(x.body, x.kind, x.index) < std::tie(y.body, y.kind, y.index); }); };
  key(a);
  key(b);
  return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](const opad::Ref& x, const opad::Ref& y) { return x.body == y.body && x.kind == y.kind && x.index == y.index; });
}
}  // namespace

void MainWindow::selectSimilar() {
  if(m_doc->loading || m_doc->designBusy || m_design->sketchActive() || m_design->featureActive() || m_design->pickingPlane() || !m_tool.id.isEmpty())
    throw opad::Error("Finish the current operation before selecting geometry.");
  const auto picks=m_viewport->selection();
  if(picks.empty() || (picks.front().kind!=opad::Ref::Kind::Face && picks.front().kind!=opad::Ref::Kind::Edge))
    throw opad::Error("Select a face or an edge to find the ones like it.");
  // Again on what the last rule selected: the next rule of the same pick.
  if(!m_similar.rules.empty() && m_similar.revision==m_doc->revision && m_similar.generation==m_doc->generation && sameRefs(picks,m_similar.selected))
    return applySimilar((m_similar.current+1)%m_similar.rules.size());
  const opad::Ref seed=picks.front();
  const opad::Node* node=m_doc->node(seed.body);
  if(!node || node->representation!="solid")throw opad::Error("Select similar works on the faces and edges of solids.");
  const auto revision=m_doc->revision,generation=m_doc->generation;
  if(!m_doc->captureSnapshot(m_jobs,[this,seed,revision,generation](std::shared_ptr<opad::Document> document,const QString& error){
    if(!document){statusBar()->showMessage(error,6000);return;}
    auto rules=std::make_shared<std::vector<opad::Recognized>>();
    m_jobs->async(tr("Select similar"),[document,rules,seed](Progress progress){
      const auto scene=opad::resolve(*document);
      opad::Recognizer recognizer(opad::node_world_shape(*document,scene,seed.body),[progress]{return progress.cancelled();});
      std::set<std::vector<int>> seen;
      for(auto& r:seed.kind==opad::Ref::Kind::Face?recognizer.similar_faces(seed.index):recognizer.similar_edges(seed.index)) {
        const auto& members=r.faces.empty()?r.edges:r.faces;
        if(members.size()>1 && seen.insert(members).second)rules->push_back(std::move(r));
      }
    },[this,rules,seed,revision,generation](bool ok,const QString& error){
      if(!ok){statusBar()->showMessage(error,6000);return;}
      if(m_doc->revision!=revision || m_doc->generation!=generation || m_design->ownsSelection())return;
      if(rules->empty()){statusBar()->showMessage(tr("Nothing else on this body is like it."),6000);return;}
      m_similar={*rules,seed.body,0,{},revision,generation};
      applySimilar(0);
    });
  }))throw opad::Error("Document is busy; try again shortly.");
}

void MainWindow::applySimilar(size_t rule) {
  const opad::Recognized& r=m_similar.rules.at(rule);
  const bool edges=r.faces.empty();
  std::vector<opad::Ref> refs;
  for(int i:edges?r.edges:r.faces){opad::Ref ref;ref.body=m_similar.body;ref.kind=edges?opad::Ref::Kind::Edge:opad::Ref::Kind::Face;ref.index=i;refs.push_back(ref);}
  m_similar.current=rule;m_similar.selected=refs;
  m_viewport->selectRefs(refs);
  const QString next=m_similar.rules.size()>1?tr(" · again: %1").arg(similarText(m_similar.rules[(rule+1)%m_similar.rules.size()])):QString();
  statusBar()->showMessage(tr("%1 · %2 selected").arg(similarText(r)).arg(refs.size())+next,8000);
  trace::log(QString("select similar: %1 %2 (%3 rules)").arg(QString::fromStdString(r.rule)).arg(refs.size()).arg(m_similar.rules.size()));
}
