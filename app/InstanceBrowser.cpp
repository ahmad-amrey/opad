#include "MainWindow.hpp"
#include <QCheckBox>
#include <QPushButton>
#include <QVBoxLayout>
#include <QPointer>

void MainWindow::browseInstances(const std::string& id) {
  const auto* node=m_doc->scene.node(id);if(!node)return;
  if(auto* old=findChild<ToolPanel*>("instanceBrowser")){old->setObjectName({});old->hide();old->deleteLater();}
  const auto key=node->body_key;
  auto instances=std::make_shared<std::vector<std::string>>();
  for(const auto& candidate:m_doc->scene.all_bodies())if(m_doc->scene.node(candidate)->body_key==key)instances->push_back(candidate);
  if(instances->size()<2)return;
  auto index=std::make_shared<size_t>(std::find(instances->begin(),instances->end(),id)-instances->begin());
  auto* body=new QWidget;auto* layout=new QVBoxLayout(body);auto* label=new QLabel(body);label->setWordWrap(true);layout->addWidget(label);
  auto* isolate=new QCheckBox(tr("Isolate instance"),body);isolate->setChecked(true);layout->addWidget(isolate);
  auto* row=new QHBoxLayout;auto* previous=new QPushButton(tr("Previous instance"),body);auto* next=new QPushButton(tr("Next instance"),body);row->addWidget(previous);row->addWidget(next);layout->addLayout(row);
  previous->setObjectName("previousInstance");next->setObjectName("nextInstance");
  auto* panel=new ToolPanel("instances","component",&Tokens::sel,tr("Linked instances"),body,180,this);panel->setObjectName("instanceBrowser");m_panels<<panel;
  const auto previousIsolation=m_viewport->isolatedNodes();const auto generation=m_doc->generation;
  auto update=[=,this] {
    if(generation!=m_doc->generation){panel->hide();return;}
    std::erase_if(*instances,[this](const auto& candidate){return !m_doc->scene.node(candidate);});
    if(instances->empty()){panel->hide();return;}*index%=instances->size();const auto& current=instances->at(*index);
    panel->setProperty("instanceCurrent",QString::fromStdString(current));
    label->setText(tr("Instance %1 of %2\n%3").arg(*index+1).arg(instances->size()).arg(m_doc->nodeName(current)));
    m_viewport->isolate(isolate->isChecked()?std::vector<std::string>{current}:previousIsolation);
    // Fit once the instance is on screen: isolating it re-displays it through a sliced job, so fitting now would
    // find nothing displayed and fall back to everything.
    m_browser->setSelectedIds({current});m_browser->scrollToSelected();m_viewport->selectNodes({current});m_viewport->fitNodesWhenReady({current});
  };
  connect(previous,&QPushButton::clicked,body,[=]{if(instances->empty())return;*index=(*index+instances->size()-1)%instances->size();update();});
  connect(next,&QPushButton::clicked,body,[=]{if(instances->empty())return;*index=(*index+1)%instances->size();update();});
  connect(isolate,&QCheckBox::toggled,body,update);
  connect(m_doc,&AppDocument::changed,panel,&ToolPanel::hide);
  // Like isolation: looking around (fit, home, projection, style, grid, panels) keeps browsing; commands that act on
  // something else, or isolate/unisolate, end it.
  for(auto* action:m_actions){
    const QString id=action->objectName();
    const bool looking=(id.startsWith("view.") && id!="view.isolate" && id!="view.unisolate") || id.startsWith("nav.") || id.startsWith("panel.") || id.startsWith("help.");
    if(!looking)connect(action,&QAction::triggered,panel,&ToolPanel::hide);
  }
  connect(m_doc,&AppDocument::aboutToReplace,panel,&ToolPanel::hide);
  connect(panel,&ToolPanel::visibilityChanged,this,[=,this](bool shown){if(!shown){if(generation==m_doc->generation && m_viewport->isolatedNodes()==std::vector<std::string>{panel->property("instanceCurrent").toString().toStdString()})m_viewport->isolate(previousIsolation);panel->deleteLater();}});
  connect(panel,&QObject::destroyed,this,[this,panel]{m_panels.removeAll(panel);});openPanel(panel);update();
}
