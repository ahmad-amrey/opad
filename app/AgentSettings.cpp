#include "AgentBridge.hpp"
#include "Panels.hpp"
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
#include <QDesktopServices>
#include <QUrl>
void AgentBridge::settings(){
  auto* dialog=new QDialog(m_window);dialog->setAttribute(Qt::WA_DeleteOnClose);dialog->setWindowTitle(tr("AI integration"));dialog->resize(640,620);
  auto* layout=new QVBoxLayout(dialog);auto* connection=new QGroupBox(tr("Connection"));auto* form=new QVBoxLayout(connection);layout->addWidget(connection);
  auto* enabled=new QCheckBox(tr("Enable local agent access"));enabled->setChecked(m_enabled);form->addWidget(enabled);
  auto* status=new QLabel;status->setWordWrap(true);form->addWidget(status);
  auto update=[this,status]{status->setText(stateText()+"\n"+m_doc->title());};connect(this,&AgentBridge::statusChanged,status,update);update();
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
  auto* guidance=new QLabel(tr("In a desktop MCP client that supports local STDIO servers, add this command and its arguments, then restart the server. Enabling OPAD does not register it in the client. Ask the agent to list OPAD windows and choose this document explicitly."));guidance->setWordWrap(true);form->addWidget(guidance);
  if(!QFileInfo::exists(executable)){auto* missing=new QLabel(tr("CLI executable not found beside OPAD. Install the matching CLI before configuring the client."));missing->setWordWrap(true);form->addWidget(missing);}
  auto* buttons=new QHBoxLayout;form->addLayout(buttons);auto* copy=new QPushButton(tr("Copy configuration"));buttons->addWidget(copy);connect(copy,&QPushButton::clicked,dialog,[config]{QApplication::clipboard()->setText(config->toPlainText());});
  auto* docs=new QPushButton(tr("Client setup guide"));buttons->addWidget(docs);connect(docs,&QPushButton::clicked,dialog,[]{QDesktopServices::openUrl(QUrl("https://learn.chatgpt.com/docs/extend/mcp"));});
  auto* test=new QPushButton(tr("Test local connection"));buttons->addWidget(test);
  auto* diagnostics=new QLabel;diagnostics->setWordWrap(true);form->addWidget(diagnostics);
  connect(test,&QPushButton::clicked,dialog,[this,dialog,diagnostics]{
    if(!m_enabled){diagnostics->setText(tr("Enable local agent access before testing."));return;}
    auto* socket=new QLocalSocket(dialog);auto* timer=new QTimer(socket);timer->setSingleShot(true);
    connect(socket,&QLocalSocket::connected,dialog,[socket,diagnostics]{diagnostics->setText(tr("Local connection passed. An AI-client round trip is confirmed only when a client binds and calls a tool."));socket->disconnectFromServer();socket->deleteLater();});
    connect(timer,&QTimer::timeout,dialog,[socket,diagnostics]{diagnostics->setText(tr("Local connection failed: %1").arg(socket->errorString()));socket->abort();socket->deleteLater();});timer->start(3000);socket->connectToServer(m_endpoint);
  });
  auto* access=new QGroupBox(tr("Access"));auto* accessLayout=new QVBoxLayout(access);layout->addWidget(access);
  auto* edit=new QCheckBox(tr("Allow design edits and exports"));edit->setChecked(m_edit);accessLayout->addWidget(edit);
  auto* help=new QLabel(tr("Unchecked means view-only. Disabling access or editing cancels unfinished work and removes previews. Completed edits stay in the document and can be undone."));help->setWordWrap(true);accessLayout->addWidget(help);
  connect(enabled,&QCheckBox::toggled,dialog,[this,edit](bool on){setAccess(on,edit->isChecked());});connect(edit,&QCheckBox::toggled,dialog,[this,enabled](bool on){setAccess(enabled->isChecked(),on);});
  auto* activityGroup=new QGroupBox(tr("Activity"));auto* activityLayout=new QVBoxLayout(activityGroup);layout->addWidget(activityGroup);
  auto* activityButton=new QPushButton(tr("Show agent activity"));activityLayout->addWidget(activityButton);connect(activityButton,&QPushButton::clicked,this,&AgentBridge::showActivity);
  auto* stopButton=new QPushButton(tr("Stop agent work"));activityLayout->addWidget(stopButton);connect(stopButton,&QPushButton::clicked,this,&AgentBridge::stop);
  auto* close=new QDialogButtonBox(QDialogButtonBox::Close);layout->addWidget(close);connect(close,&QDialogButtonBox::rejected,dialog,&QDialog::close);dialog->show();
}
