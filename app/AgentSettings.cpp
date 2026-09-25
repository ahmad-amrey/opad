#include "AgentBridge.hpp"
#include "Panels.hpp"
#include "AgentRegistration.hpp"
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
#include <QScrollArea>
#include <QTabWidget>
void AgentBridge::settings(){
  auto* dialog=new QDialog(m_window);dialog->setAttribute(Qt::WA_DeleteOnClose);dialog->setWindowTitle(tr("AI integration"));dialog->resize(640,620);
  auto* outer=new QVBoxLayout(dialog);auto* tabs=new QTabWidget;outer->addWidget(tabs);
  auto* scroll=new QScrollArea;scroll->setWidgetResizable(true);tabs->addTab(scroll,tr("Access and activity"));
  auto* page=new QWidget;scroll->setWidget(page);auto* layout=new QVBoxLayout(page);auto* connection=new QGroupBox(tr("Connection"));auto* form=new QVBoxLayout(connection);layout->addWidget(connection);
  auto* enabled=new QCheckBox(tr("Enable local agent access"));enabled->setChecked(m_enabled);form->addWidget(enabled);
  auto* status=new QLabel;status->setWordWrap(true);form->addWidget(status);
  auto update=[this,status]{status->setText(stateText()+"\n"+m_doc->title());};connect(this,&AgentBridge::statusChanged,status,update);update();
  auto* disconnectButton=new QPushButton(tr("Disconnect clients"));form->addWidget(disconnectButton);connect(disconnectButton,&QPushButton::clicked,this,&AgentBridge::disconnectClients);
  auto* setupScroll=new QScrollArea;setupScroll->setWidgetResizable(true);tabs->addTab(setupScroll,tr("Client setup"));
  auto* setupPage=new QWidget;setupScroll->setWidget(setupPage);auto* setupLayout=new QVBoxLayout(setupPage);
  auto* manualSetup=new QGroupBox(tr("Other MCP clients"));form=new QVBoxLayout(manualSetup);
  const auto directory=QCoreApplication::applicationDirPath();QString executable=directory+"/opad-cli";
#ifdef Q_OS_WIN
  executable+=".exe";
#endif
  if(!QFileInfo::exists(executable)){
    const auto candidates=QDir(directory).entryList({"opad-cli-*.exe","opad-cli-*"},QDir::Files);if(!candidates.empty())executable=directory+"/"+candidates.front();
  }
  auto* config=new QPlainTextEdit;config->setReadOnly(true);config->setMaximumHeight(145);
  opad::json configuration;
  configuration["mcpServers"]["opad"]={{"command",executable.toStdString()},{"args",{"mcp","--live","--discovery",m_directory.toStdString()}}};
  config->setPlainText(QString::fromStdString(configuration.dump(2)));form->addWidget(config);
  auto* adapter=new QComboBox;adapter->addItems({tr("Generic MCP JSON"),tr("VS Code MCP configuration"),tr("Local command and arguments")});form->insertWidget(form->indexOf(config),adapter);
  connect(adapter,&QComboBox::currentIndexChanged,dialog,[=,this](int index){
    auto data=configuration;
    if(index==1){data={{"servers",configuration["mcpServers"]}};data["servers"]["opad"]["type"]="stdio";}
    if(index==2)config->setPlainText(tr("Command: %1\nArguments: %2\nIn Settings > MCP servers, add a local STDIO server, save, then restart it. Available only in clients supporting local MCP.").arg(executable,QString::fromStdString(configuration["mcpServers"]["opad"]["args"].dump())));
    else config->setPlainText(QString::fromStdString(data.dump(2)));
  });
  auto* guidance=new QLabel(tr("In a desktop MCP client that supports local STDIO servers, add this command and its arguments, then restart the server. Enabling OPAD does not register it in the client. Ask the agent to list OPAD windows and choose this document explicitly."));guidance->setWordWrap(true);form->addWidget(guidance);
  if(!QFileInfo::exists(executable)){auto* missing=new QLabel(tr("CLI executable not found beside OPAD. Install the matching CLI before configuring the client."));missing->setWordWrap(true);form->addWidget(missing);}
  auto* buttons=new QHBoxLayout;form->addLayout(buttons);auto* copy=new QPushButton(tr("Copy configuration"));buttons->addWidget(copy);connect(copy,&QPushButton::clicked,dialog,[config]{QApplication::clipboard()->setText(config->toPlainText());});
  auto* docs=new QPushButton(tr("Client setup guide"));buttons->addWidget(docs);connect(docs,&QPushButton::clicked,dialog,[dialog]{
    QDialog guide(dialog);guide.setWindowTitle(tr("Connect an AI client"));guide.resize(560,380);
    auto* layout=new QVBoxLayout(&guide);auto* steps=new QLabel(tr("1. Open a document in OPAD and enable local agent access on the Access and activity tab.\n\n2. In your AI client, add a local MCP server using STDIO. Copy the configuration, or enter the command and arguments shown here. Keep the matching CLI executable beside OPAD.\n\n3. Restart that server in the client. Use Test local connection here to check OPAD, then ask the agent to list OPAD windows and bind to this document.\n\n4. Ask the agent to inspect the scene before editing. Enable design edits when ready; changes appear live and completed edits support Undo.\n\n5. Watch Agent activity to follow progress or stop work. Disable local agent access to disconnect. If the client supports only remote MCP, use a client with local STDIO support for this setup."));
    steps->setWordWrap(true);layout->addWidget(steps);auto* close=new QDialogButtonBox(QDialogButtonBox::Close);layout->addWidget(close);connect(close,&QDialogButtonBox::rejected,&guide,&QDialog::reject);guide.exec();
  });
  auto* test=new QPushButton(tr("Test local connection"));buttons->addWidget(test);
  auto* diagnostics=new QLabel;diagnostics->setWordWrap(true);form->addWidget(diagnostics);
  connect(test,&QPushButton::clicked,dialog,[this,dialog,diagnostics]{
    if(!m_enabled){diagnostics->setText(tr("Enable local agent access before testing."));return;}
    auto* socket=new QLocalSocket(dialog);auto* timer=new QTimer(socket);timer->setSingleShot(true);
    connect(socket,&QLocalSocket::connected,dialog,[socket,diagnostics]{diagnostics->setText(tr("Local connection passed. An AI-client round trip is confirmed only when a client binds and calls a tool."));socket->disconnectFromServer();socket->deleteLater();});
    connect(timer,&QTimer::timeout,dialog,[socket,diagnostics]{diagnostics->setText(tr("Local connection failed: %1").arg(socket->errorString()));socket->abort();socket->deleteLater();});timer->start(3000);socket->connectToServer(m_endpoint);
  });
  auto* registration=new QGroupBox(tr("Codex registration"));auto* registrationLayout=new QVBoxLayout(registration);
  registrationLayout->addWidget(new AgentRegistration(executable,m_directory,registration));setupLayout->addWidget(registration);setupLayout->addWidget(manualSetup);setupLayout->addStretch();
  auto* access=new QGroupBox(tr("Access"));auto* accessLayout=new QVBoxLayout(access);layout->addWidget(access);
  auto* edit=new QCheckBox(tr("Allow design edits and exports"));edit->setChecked(m_edit);accessLayout->addWidget(edit);
  auto* help=new QLabel(tr("Unchecked means view-only. Disabling access or editing cancels unfinished work and removes previews. Completed edits stay in the document and can be undone."));help->setWordWrap(true);accessLayout->addWidget(help);
  connect(enabled,&QCheckBox::toggled,dialog,[this,edit](bool on){setAccess(on,edit->isChecked());});connect(edit,&QCheckBox::toggled,dialog,[this,enabled](bool on){setAccess(enabled->isChecked(),on);});
  auto* activityGroup=new QGroupBox(tr("Activity"));auto* activityLayout=new QVBoxLayout(activityGroup);layout->addWidget(activityGroup);
  auto* activityButton=new QPushButton(tr("Show agent activity"));activityLayout->addWidget(activityButton);connect(activityButton,&QPushButton::clicked,this,&AgentBridge::showActivity);
  auto* stopButton=new QPushButton(tr("Stop agent work"));activityLayout->addWidget(stopButton);connect(stopButton,&QPushButton::clicked,this,&AgentBridge::stop);
  layout->addStretch();
  auto* close=new QDialogButtonBox(QDialogButtonBox::Close);outer->addWidget(close);connect(close,&QDialogButtonBox::rejected,dialog,&QDialog::close);dialog->show();
}
