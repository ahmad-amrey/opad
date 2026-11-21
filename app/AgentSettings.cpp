#include "AgentBridge.hpp"
#include "Panels.hpp"
#include "AgentClients.hpp"
#include <QDialog>
#include <QVBoxLayout>
#include <QFormLayout>
#include <QGroupBox>
#include <QPushButton>
#include <QPlainTextEdit>
#include <QDialogButtonBox>
#include <QFileInfo>
#include <QDir>
#include <QApplication>
#include <QClipboard>
#include <QLocalSocket>
#include <QComboBox>
#include <QLineEdit>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QTabWidget>
#include <QTimer>

bool AgentBridge::clientConnected()const{
  for(const auto& s:m_sessions)if(s->bound && s->socket && s->socket->state()==QLocalSocket::ConnectedState)return true;
  return false;
}

void AgentBridge::settings(int pageWanted){
  for(auto* open:m_window->findChildren<QDialog*>("agentSettings"))open->close();  // one at a time: the chip opens it at the page that fits
  auto* dialog=new QDialog(m_window);dialog->setObjectName("agentSettings");dialog->setAttribute(Qt::WA_DeleteOnClose);dialog->setWindowTitle(tr("AI integration"));dialog->resize(680,680);
  auto* outer=new QVBoxLayout(dialog);auto* tabs=new QTabWidget;outer->addWidget(tabs);

  // The copy of opad-cli an AI client starts: the one beside this OPAD.
  const auto directory=QCoreApplication::applicationDirPath();QString executable=directory+"/opad-cli";
#ifdef Q_OS_WIN
  executable+=".exe";
#endif
  if(!QFileInfo::exists(executable)){
    const auto candidates=QDir(directory).entryList({"opad-cli-*.exe","opad-cli-*"},QDir::Files);if(!candidates.empty())executable=directory+"/"+candidates.front();
  }

  // ---- Connect an AI client: the four steps, each client's state at a glance.
  auto* connectScroll=new QScrollArea;connectScroll->setObjectName("agentConnectPage");connectScroll->setWidgetResizable(true);tabs->addTab(connectScroll,tr("Connect an AI client"));
  auto* connectPage=new QWidget;connectScroll->setWidget(connectPage);auto* steps=new QVBoxLayout(connectPage);
  auto* intro=new QLabel(tr("OPAD works with AI clients that start local MCP servers: Claude Code, Claude Desktop, Codex and others. Four steps:"));intro->setWordWrap(true);steps->addWidget(intro);
  auto step=[&](int n,const QString& text){auto* l=new QLabel(QStringLiteral("<b>%1.</b> %2").arg(n).arg(text.toHtmlEscaped()));l->setWordWrap(true);steps->addSpacing(6);steps->addWidget(l);return l;};
  step(1,tr("Connect your client. One click registers OPAD as its MCP server \"opad\"; nothing else in the client changes."));
  auto* clients=new QGroupBox;auto* clientsLayout=new QVBoxLayout(clients);steps->addWidget(clients);
  QList<AgentClientRow*> rows;
  for(const auto kind:{AgentClientRow::Client::ClaudeCode,AgentClientRow::Client::ClaudeDesktop,AgentClientRow::Client::Codex}){
    auto* row=new AgentClientRow(kind,executable,m_directory,clients);clientsLayout->addWidget(row);rows<<row;
  }
  auto* otherRow=new QHBoxLayout;clientsLayout->addLayout(otherRow);
  auto* otherLabel=new QLabel(tr("<b>Other MCP clients</b>"));otherRow->addWidget(otherLabel);
  auto* other=new QPushButton(tr("Show the configuration to copy"));otherRow->addWidget(other);otherRow->addStretch();
  if(!QFileInfo::exists(executable)){auto* missing=new QLabel(tr("opad-cli was not found beside OPAD: install the matching opad-cli before connecting a client."));missing->setWordWrap(true);clientsLayout->addWidget(missing);}
  step(2,tr("Restart the client, so it starts OPAD's server (each row says how once it is connected)."));
  step(3,tr("Allow access for this document:"));
  auto* allow=new QCheckBox(tr("Enable local agent access"));allow->setObjectName("agentConnectEnable");steps->addWidget(allow);
  auto* allowEdit=new QCheckBox(tr("Allow design edits and exports"));allowEdit->setObjectName("agentConnectEdit");steps->addWidget(allowEdit);
  step(4,tr("Ask the AI, for example:"));
  auto* askRow=new QHBoxLayout;steps->addLayout(askRow);
  auto* ask=new QLineEdit;ask->setObjectName("agentConnectPrompt");ask->setReadOnly(true);askRow->addWidget(ask,1);
  auto* copyAsk=new QPushButton(tr("Copy"));askRow->addWidget(copyAsk);
  connect(copyAsk,&QPushButton::clicked,dialog,[ask]{QApplication::clipboard()->setText(ask->text());});
  auto* connectState=new QLabel;connectState->setObjectName("agentConnectState");connectState->setWordWrap(true);steps->addSpacing(6);steps->addWidget(connectState);
  steps->addStretch();

  // ---- Access and activity.
  auto* scroll=new QScrollArea;scroll->setObjectName("agentAccessPage");scroll->setWidgetResizable(true);tabs->addTab(scroll,tr("Access and activity"));
  auto* page=new QWidget;scroll->setWidget(page);auto* layout=new QVBoxLayout(page);auto* connection=new QGroupBox(tr("Connection"));auto* form=new QVBoxLayout(connection);layout->addWidget(connection);
  auto* enabled=new QCheckBox(tr("Enable local agent access"));enabled->setChecked(m_enabled);form->addWidget(enabled);
  auto* status=new QLabel;status->setWordWrap(true);form->addWidget(status);
  auto* disconnectButton=new QPushButton(tr("Disconnect clients"));form->addWidget(disconnectButton);connect(disconnectButton,&QPushButton::clicked,this,&AgentBridge::disconnectClients);

  // ---- Client setup: any other MCP client, by its configuration.
  auto* setupScroll=new QScrollArea;setupScroll->setObjectName("agentSetupPage");setupScroll->setWidgetResizable(true);tabs->addTab(setupScroll,tr("Client setup"));
  connect(other,&QPushButton::clicked,dialog,[tabs,setupScroll]{tabs->setCurrentWidget(setupScroll);});
  auto* setupPage=new QWidget;setupScroll->setWidget(setupPage);auto* setupLayout=new QVBoxLayout(setupPage);
  auto* manualSetup=new QGroupBox(tr("Other MCP clients"));form=new QVBoxLayout(manualSetup);
  auto* config=new QPlainTextEdit;config->setReadOnly(true);config->setMaximumHeight(145);
  opad::json configuration;
  configuration["mcpServers"]["opad"]={{"command",QDir::toNativeSeparators(executable).toStdString()},{"args",{"mcp","--live","--discovery",QDir::toNativeSeparators(m_directory).toStdString()}}};
  config->setPlainText(QString::fromStdString(configuration.dump(2)));form->addWidget(config);
  auto* adapter=new QComboBox;adapter->addItems({tr("Generic MCP JSON"),tr("VS Code MCP configuration"),tr("Local command and arguments")});form->insertWidget(form->indexOf(config),adapter);
  connect(adapter,&QComboBox::currentIndexChanged,dialog,[=,this](int index){
    auto data=configuration;
    if(index==1){data={{"servers",configuration["mcpServers"]}};data["servers"]["opad"]["type"]="stdio";}
    if(index==2)config->setPlainText(tr("Command: %1\nArguments: %2\nIn Settings > MCP servers, add a local STDIO server, save, then restart it. Available only in clients supporting local MCP.").arg(executable,QString::fromStdString(configuration["mcpServers"]["opad"]["args"].dump())));
    else config->setPlainText(QString::fromStdString(data.dump(2)));
  });
  auto* guidance=new QLabel(tr("In a desktop MCP client that supports local STDIO servers, add this command and its arguments, then restart the server. Enabling OPAD does not register it in the client. Ask the agent to list OPAD windows and choose this document explicitly."));guidance->setWordWrap(true);form->addWidget(guidance);
  auto* buttons=new QHBoxLayout;form->addLayout(buttons);auto* copy=new QPushButton(tr("Copy configuration"));buttons->addWidget(copy);connect(copy,&QPushButton::clicked,dialog,[config]{QApplication::clipboard()->setText(config->toPlainText());});
  auto* test=new QPushButton(tr("Test local connection"));buttons->addWidget(test);
  auto* diagnostics=new QLabel;diagnostics->setWordWrap(true);form->addWidget(diagnostics);
  connect(test,&QPushButton::clicked,dialog,[this,dialog,diagnostics]{
    if(!m_enabled){diagnostics->setText(tr("Enable local agent access before testing."));return;}
    auto* socket=new QLocalSocket(dialog);auto* timer=new QTimer(socket);timer->setSingleShot(true);
    connect(socket,&QLocalSocket::connected,dialog,[socket,diagnostics]{diagnostics->setText(tr("Local connection passed. An AI-client round trip is confirmed only when a client binds and calls a tool."));socket->disconnectFromServer();socket->deleteLater();});
    connect(timer,&QTimer::timeout,dialog,[socket,diagnostics]{diagnostics->setText(tr("Local connection failed: %1").arg(socket->errorString()));socket->abort();socket->deleteLater();});timer->start(3000);socket->connectToServer(m_endpoint);
  });
  setupLayout->addWidget(manualSetup);setupLayout->addStretch();

  auto* access=new QGroupBox(tr("Access"));auto* accessLayout=new QVBoxLayout(access);layout->addWidget(access);
  auto* edit=new QCheckBox(tr("Allow design edits and exports"));edit->setChecked(m_edit);accessLayout->addWidget(edit);
  auto* help=new QLabel(tr("Unchecked means view-only. Disabling access or editing cancels unfinished work and removes previews. Completed edits stay in the document and can be undone."));help->setWordWrap(true);accessLayout->addWidget(help);
  // The two pages' switches are one setting: either changes it, both show it.
  connect(enabled,&QCheckBox::toggled,dialog,[this,edit](bool on){setAccess(on,edit->isChecked());});connect(edit,&QCheckBox::toggled,dialog,[this,enabled](bool on){setAccess(enabled->isChecked(),on);});
  connect(allow,&QCheckBox::toggled,dialog,[this](bool on){setAccess(on,m_edit);});connect(allowEdit,&QCheckBox::toggled,dialog,[this](bool on){setAccess(m_enabled || on,on);});
  auto update=[this,status,connectState,enabled,edit,allow,allowEdit,ask]{
    status->setText(stateText()+"\n"+m_doc->title());
    connectState->setText(tr("Now: %1").arg(stateText()));
    for(auto* box:{enabled,allow}){QSignalBlocker block(box);box->setChecked(m_enabled);}
    for(auto* box:{edit,allowEdit}){QSignalBlocker block(box);box->setChecked(m_edit);}
    // The document by its file's name, as the windows list names it (the title adds " - OPAD" and the unsaved mark).
    const QString name=m_doc->browse?QFileInfo(m_doc->viewing).fileName():m_doc->doc.path.empty()?tr("Untitled"):QString::fromStdU16String(m_doc->doc.path.filename().u16string());
    ask->setText(tr("List the OPAD windows and use \"%1\".").arg(name));
  };
  connect(this,&AgentBridge::statusChanged,dialog,update);update();
  auto* activityGroup=new QGroupBox(tr("Activity"));auto* activityLayout=new QVBoxLayout(activityGroup);layout->addWidget(activityGroup);
  auto* activityButton=new QPushButton(tr("Show agent activity"));activityLayout->addWidget(activityButton);connect(activityButton,&QPushButton::clicked,this,&AgentBridge::showActivity);
  auto* stopButton=new QPushButton(tr("Stop agent work"));activityLayout->addWidget(stopButton);connect(stopButton,&QPushButton::clicked,this,&AgentBridge::stop);
  layout->addStretch();

  tabs->setCurrentIndex(pageWanted>=0 && pageWanted<tabs->count()?pageWanted:clientConnected()?1:0);
  auto* close=new QDialogButtonBox(QDialogButtonBox::Close);outer->addWidget(close);connect(close,&QDialogButtonBox::rejected,dialog,&QDialog::close);dialog->show();
  for(auto* row:rows)row->refresh();  // each client's state at a glance (Codex asks its CLI: asynchronous)
}
