#include "MainWindow.hpp"
#include "opad/geometry.hpp"
#include "opad/recognize.hpp"
#include <QStatusBar>
#include <algorithm>
#include <cmath>
#include <set>
#include <tuple>

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
  // A body's rules (Recognizer::body_rules), in world coordinates.
  if (r.rule == "top") return MainWindow::tr("Top perimeter");
  if (r.rule == "bottom") return MainWindow::tr("Bottom perimeter");
  if (r.rule == "x") return MainWindow::tr("Edges parallel to X");
  if (r.rule == "y") return MainWindow::tr("Edges parallel to Y");
  if (r.rule == "z") return MainWindow::tr("Edges parallel to Z");
  if (r.rule == "circle") return MainWindow::tr("Circular edges");
  if (r.rule == "up") return MainWindow::tr("Upward planar faces");
  if (r.rule == "holes") return MainWindow::tr("All holes (%1)").arg(p.value("count", 0));
  if (r.rule == "fillets") return MainWindow::tr("All fillets");
  if (r.rule == "chamfers") return MainWindow::tr("All chamfers");
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
  // Again on what the last rule selected: the next rule of the same pick.
  if(!m_similar.rules.empty() && m_similar.revision==m_doc->revision && m_similar.generation==m_doc->generation && sameRefs(picks,m_similar.selected))
    return applySimilar((m_similar.current+1)%m_similar.rules.size());
  // A face or an edge: the ones like it. A body picked whole (here or in the browser): its edges and faces by rule, top
  // perimeter first (what the modal Select by geometry offered, without the dialog and the cap of 100).
  opad::Ref seed;
  if(!picks.empty() && (picks.front().kind==opad::Ref::Kind::Face || picks.front().kind==opad::Ref::Kind::Edge))seed=picks.front();
  else if(const auto ids=currentNodeIds(); ids.size()==1 && (picks.empty() || picks.front().kind==opad::Ref::Kind::Body) && m_doc->node(ids[0]) && m_doc->node(ids[0])->kind==opad::Node::Kind::Body)seed.body=ids[0];
  else throw opad::Error("Select a body, a face or an edge to find the ones like it.");
  const opad::Node* node=m_doc->node(seed.body);
  if(!node || node->body_missing)throw opad::Error("Select a body, a face or an edge to find the ones like it.");
  if(seed.kind!=opad::Ref::Kind::Body && node->representation!="solid")throw opad::Error("Select similar works on the faces and edges of solids.");
  const auto revision=m_doc->revision,generation=m_doc->generation;
  if(!m_doc->captureSnapshot(m_jobs,[this,seed,revision,generation](std::shared_ptr<opad::Document> document,const QString& error){
    if(!document){statusBar()->showMessage(error,6000);return;}
    auto rules=std::make_shared<std::vector<opad::Recognized>>();
    m_jobs->async(tr("Select similar"),[document,rules,seed](Progress progress){
      const auto scene=opad::resolve(*document);
      opad::Recognizer recognizer(opad::node_world_shape(*document,scene,seed.body),[progress]{return progress.cancelled();});
      const bool whole=seed.kind==opad::Ref::Kind::Body;
      std::set<std::vector<int>> seen;
      for(auto& r:whole?recognizer.body_rules():seed.kind==opad::Ref::Kind::Face?recognizer.similar_faces(seed.index):recognizer.similar_edges(seed.index)) {
        const auto& members=r.faces.empty()?r.edges:r.faces;
        if(members.size()>(whole?0u:1u) && seen.insert(members).second)rules->push_back(std::move(r));
      }
    },[this,rules,seed,revision,generation](bool ok,const QString& error){
      if(!ok){statusBar()->showMessage(error,6000);return;}
      if(m_doc->revision!=revision || m_doc->generation!=generation || m_design->ownsSelection())return;
      if(rules->empty()){statusBar()->showMessage(seed.kind==opad::Ref::Kind::Body?tr("No edges or faces of this body follow a rule."):tr("Nothing else on this body is like it."),6000);return;}
      m_similar={*rules,seed.body,0,{},revision,generation,m_similar.token};
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
  const auto token=++m_similar.token;  // a rule still waiting for its filter is dropped
  const QString next=m_similar.rules.size()>1?tr(" · again: %1").arg(similarText(m_similar.rules[(rule+1)%m_similar.rules.size()])):QString();
  const QString text=tr("%1 · %2 selected").arg(similarText(r)).arg(refs.size())+next;
  const QString log=QString("select similar: %1 %2 (%3 rules)").arg(QString::fromStdString(r.rule)).arg(refs.size()).arg(m_similar.rules.size());
  auto show=[this,refs,text,log]{m_viewport->selectRefs(refs);statusBar()->showMessage(text,8000);trace::log(log);};
  const auto filter=edges?Viewport::SelFilter::Edge:Viewport::SelFilter::Face;
  if(m_viewport->selectionFilter()==filter)return show();
  // A body's rules pick edges or faces: the view picks those from now on (the filter chips follow).
  connect(m_viewport,&Viewport::filterApplied,this,[this,show,filter,token,revision=m_similar.revision,generation=m_similar.generation]{
    if(token==m_similar.token && m_viewport->selectionFilter()==filter && m_doc->revision==revision && m_doc->generation==generation && !m_design->ownsSelection())show();
  },Qt::SingleShotConnection);
  m_viewport->setSelectionFilter(filter);
}
