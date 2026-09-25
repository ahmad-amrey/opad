#include "AgentRegistration.hpp"
#include <QCoreApplication>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPointer>
#include <QProcess>
#include <QPushButton>
#include <QSettings>
#include <QStandardPaths>
#include <QTimer>
#include <QVBoxLayout>
AgentRegistration::AgentRegistration(QString executable,QString discovery,QWidget* parent)
 :QWidget(parent),m_executable(std::move(executable)),m_arguments({"mcp","--live","--discovery",discovery}) {
  auto* layout=new QVBoxLayout(this);layout->setContentsMargins(0,0,0,0);
  auto* help=new QLabel(tr("Register this installation as the Codex MCP server 'opad'. Registration does not enable OPAD access or select a document. Restart the client's MCP server after changing registration."));help->setWordWrap(true);layout->addWidget(help);
  auto* row=new QHBoxLayout;layout->addLayout(row);
  m_cli=new QLineEdit(QSettings().value("agent/codexExecutable",QStandardPaths::findExecutable("codex")).toString());m_cli->setPlaceholderText(tr("Codex executable path"));m_cli->setObjectName("codexExecutable");row->addWidget(m_cli);
  auto* browse=new QPushButton(tr("Browse..."));row->addWidget(browse);
  connect(browse,&QPushButton::clicked,this,[this]{if(m_running)return;const auto path=QFileDialog::getOpenFileName(this,tr("Select Codex executable"));if(!path.isEmpty())m_cli->setText(path);});
  m_status=new QLabel;m_status->setWordWrap(true);layout->addWidget(m_status);
  auto* buttons=new QHBoxLayout;layout->addLayout(buttons);
  m_connect=new QPushButton(tr("Connect to Codex"));m_remove=new QPushButton(tr("Remove Codex registration"));m_refresh=new QPushButton(tr("Check registration"));
  buttons->addWidget(m_connect);buttons->addWidget(m_remove);buttons->addWidget(m_refresh);
  connect(m_connect,&QPushButton::clicked,this,&AgentRegistration::registerClient);
  connect(m_remove,&QPushButton::clicked,this,&AgentRegistration::unregisterClient);
  connect(m_refresh,&QPushButton::clicked,this,&AgentRegistration::refresh);
  message(tr("Registration has not been checked. Live connection status is on the Access and activity tab."));
}
QString AgentRegistration::report()const{return m_status->text();}
void AgentRegistration::message(const QString& value){m_status->setText(value);}
void AgentRegistration::run(const QStringList& args,std::function<void(bool,QString)> done){
  if(m_running)return;
  auto path=m_cli->text().trimmed();if(!QFileInfo(path).isAbsolute())path=QStandardPaths::findExecutable(path);
  if(path.isEmpty() || !QFileInfo(path).isExecutable()){message(tr("Codex executable not found. Install Codex or select its executable."));return;}
  m_running=true;m_cli->setEnabled(false);m_connect->setEnabled(false);m_remove->setEnabled(false);m_refresh->setEnabled(false);
  QSettings().setValue("agent/codexExecutable",path);message(tr("Checking Codex configuration..."));
  // Owned by the app so closing Settings cannot destroy a running QProcess and block
  // the UI. Callbacks use a guarded widget; only this child process may be timed out.
  auto* process=new QProcess(QCoreApplication::instance());process->setWorkingDirectory(QDir::homePath());
  auto* timer=new QTimer(process);timer->setSingleShot(true);QPointer<AgentRegistration> self=this;
  auto completed=std::make_shared<bool>(false),timedOut=std::make_shared<bool>(false);
  auto finish=[self,process,timer,done,completed](bool ok,QString text){
    if(*completed)return;
    *completed=true;timer->stop();process->deleteLater();if(!self)return;
    self->m_running=false;self->m_cli->setEnabled(true);self->m_connect->setEnabled(true);self->m_remove->setEnabled(true);self->m_refresh->setEnabled(true);done(ok,text);
  };
  connect(process,&QProcess::finished,process,[process,finish,timedOut](int code,QProcess::ExitStatus status){finish(!*timedOut && code==0 && status==QProcess::NormalExit,*timedOut?tr("Codex command timed out. Check registration before retrying."):QString::fromUtf8(code==0?process->readAllStandardOutput():process->readAllStandardError()).left(65536));});
  connect(process,&QProcess::errorOccurred,process,[process,finish](QProcess::ProcessError error){if(error==QProcess::FailedToStart)finish(false,process->errorString());});
  connect(timer,&QTimer::timeout,process,[process,timedOut]{*timedOut=true;process->kill();});
  process->setProgram(path);process->setArguments(args);process->start();timer->start(15000);
}
bool AgentRegistration::matches(const opad::json& entry)const {
  const auto transport=entry.value("transport",opad::json::object());
  QStringList args;for(const auto& arg:transport.value("args",opad::json::array()))if(arg.is_string())args<<QString::fromStdString(arg.get<std::string>());
  return transport.value("type","")=="stdio" && QFileInfo(QString::fromStdString(transport.value("command",""))).absoluteFilePath()==QFileInfo(m_executable).absoluteFilePath() && args==m_arguments;
}
void AgentRegistration::inspect(std::function<void(bool,opad::json)> done){
  run({"mcp","list","--json"},[this,done](bool ok,QString output){
    if(!ok){message(tr("Codex command failed: %1").arg(output));done(false,{});return;}
    const auto entries=opad::json::parse(output.toStdString(),nullptr,false);
    if(!entries.is_array()){message(tr("Codex returned an unsupported configuration format."));done(false,{});return;}
    for(const auto& entry:entries)if(entry.value("name","")=="opad"){done(true,entry);return;}
    done(true,nullptr);
  });
}
void AgentRegistration::refresh(){inspect([this](bool ok,opad::json entry){if(!ok)return;
  message(entry.is_null()?tr("Not registered with Codex."):matches(entry)?(entry.value("enabled",true)?tr("Registered with Codex. A live connection is confirmed only after the client binds to a document."):tr("Registered with Codex, but disabled in the client.")):tr("The Codex server 'opad' points to a different installation or transport."));
});}
void AgentRegistration::registerClient(){
  if(!QFileInfo::exists(m_executable)){message(tr("CLI executable not found beside OPAD. Install the matching CLI before configuring the client."));return;}
  inspect([this](bool ok,opad::json entry){if(!ok)return;
    if(!entry.is_null() && matches(entry) && entry.value("enabled",true)){message(tr("Already registered. Restart the client's MCP server, then choose an OPAD document."));return;}
    if(!entry.is_null() && QMessageBox::question(this,tr("Replace Codex registration?"),tr("A Codex server named 'opad' already exists. Replace its configuration with this installation? Other servers will be preserved."))!=QMessageBox::Yes)return;
    QStringList args={"mcp","add","opad","--",m_executable};args.append(m_arguments);
    run(args,[this](bool success,QString error){if(success)refresh();else message(tr("Codex command failed: %1").arg(error));});
  });
}
void AgentRegistration::unregisterClient(){inspect([this](bool ok,opad::json entry){if(!ok)return;
  if(entry.is_null()){message(tr("Not registered with Codex."));return;}
  if(!matches(entry)){message(tr("Registration belongs to another installation. Manage it in Codex; it was not removed."));return;}
  run({"mcp","remove","opad"},[this](bool success,QString error){message(success?tr("Codex registration removed. Restart the client's MCP server. Use Disconnect clients to end current OPAD connections immediately."):tr("Codex command failed: %1").arg(error));});
});}
