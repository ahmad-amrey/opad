// Settings > AI assistants: Connect (choose your AI app, choose what it may do, try it), Access (what assistants may do,
// while it works, who is connected now) and Other AI apps (the setup to copy into any other app). The tab order, the
// objectNames and the two switches are what benches and agents' tests find; the words are for people.
#include "AgentBridge.hpp"
#include "Panels.hpp"
#include "AgentClients.hpp"
#include "Icons.hpp"
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
#include <QSettings>
#include <QSignalBlocker>
#include <QTabWidget>
#include <QTimer>
#include <QToolButton>

bool AgentBridge::clientConnected()const{
  for(const auto& s:m_sessions)if(s->bound && s->socket && s->socket->state()==QLocalSocket::ConnectedState)return true;
  return false;
}

namespace {
// A step's heading: its number in a ring, then the words.
QWidget* stepHeading(int n,const QString& text){
  auto* w=new QWidget;auto* row=new QHBoxLayout(w);row->setContentsMargins(0,0,0,0);row->setSpacing(8);
  const auto& t=theme::current();
  auto* ring=new QLabel(QString::number(n));ring->setFixedSize(18,18);ring->setAlignment(Qt::AlignCenter);
  ring->setStyleSheet(QStringLiteral("border:1.5px solid %1;border-radius:9px;color:%1;font-size:11px;font-weight:500").arg(t.sel.name()));
  auto* label=new QLabel(text);label->setStyleSheet(QStringLiteral("font-weight:500"));
  row->addWidget(ring);row->addWidget(label,1);
  return w;
}
QLabel* note(const QString& text){
  auto* l=new QLabel(text);l->setWordWrap(true);l->setStyleSheet(QStringLiteral("font-size:12px;color:%1").arg(theme::current().fg2.name()));return l;
}
}

void AgentBridge::settings(int pageWanted){
  for(auto* open:m_window->findChildren<QDialog*>("agentSettings"))open->close();  // one at a time: the chip opens it at the page that fits
  auto* dialog=new QDialog(m_window);dialog->setObjectName("agentSettings");dialog->setAttribute(Qt::WA_DeleteOnClose);dialog->setWindowTitle(tr("AI assistants"));dialog->resize(680,640);
  auto* outer=new QVBoxLayout(dialog);
  // What this is, in one line, under the AI mark.
  auto* header=new QHBoxLayout;header->setSpacing(14);
  auto* mark=new QLabel;mark->setPixmap(icons::pixmap("aiMark",theme::current().hov,32,dialog->devicePixelRatioF()));mark->setAlignment(Qt::AlignTop);
  auto* headText=new QVBoxLayout;headText->setSpacing(4);
  auto* title=new QLabel(tr("Work on your designs with an AI assistant"));title->setStyleSheet(QStringLiteral("font-size:14px;font-weight:500"));
  headText->addWidget(title);
  headText->addWidget(note(tr("The assistant is an app on this computer. It sees the open design and, if you allow it, changes it. Each change is a normal step you can undo, and you can stop it at any time.")));
  header->addWidget(mark);header->addLayout(headText,1);outer->addLayout(header);
  auto* tabs=new QTabWidget;outer->addWidget(tabs);

  // The copy of opad-cli an AI client starts: the one beside this OPAD.
  const auto directory=QCoreApplication::applicationDirPath();QString executable=directory+"/opad-cli";
#ifdef Q_OS_WIN
  executable+=".exe";
#endif
  if(!QFileInfo::exists(executable)){
    const auto candidates=QDir(directory).entryList({"opad-cli-*.exe","opad-cli-*"},QDir::Files);if(!candidates.empty())executable=directory+"/"+candidates.front();
  }

  // ---- Connect: three steps.
  auto* connectScroll=new QScrollArea;connectScroll->setObjectName("agentConnectPage");connectScroll->setWidgetResizable(true);tabs->addTab(connectScroll,tr("Connect"));
  auto* connectPage=new QWidget;connectScroll->setWidget(connectPage);auto* steps=new QVBoxLayout(connectPage);steps->setSpacing(8);
  steps->addWidget(stepHeading(1,tr("Choose your AI app")));
  QList<AgentClientRow*> rows;
  for(const auto kind:{AgentClientRow::Client::ClaudeCode,AgentClientRow::Client::ClaudeDesktop,AgentClientRow::Client::Codex}){
    auto* row=new AgentClientRow(kind,executable,m_directory,connectPage);steps->addWidget(row);rows<<row;
  }
  auto* other=new QToolButton;other->setObjectName("toastAction");other->setAutoRaise(true);other->setText(tr("Using another AI app? Set it up by hand"));
  steps->addWidget(other,0,Qt::AlignLeft);
  if(!QFileInfo::exists(executable))steps->addWidget(note(tr("opad-cli was not found beside OPAD: install the matching opad-cli before setting up an AI app.")));
  steps->addSpacing(10);
  steps->addWidget(stepHeading(2,tr("Choose what it may do")));
  auto* allow=new QCheckBox(tr("Let AI assistants see your designs"));allow->setObjectName("agentConnectEnable");steps->addWidget(allow);
  auto* allowEdit=new QCheckBox(tr("…and change them (each change can be undone)"));allowEdit->setObjectName("agentConnectEdit");
  auto* editRow=new QHBoxLayout;editRow->addSpacing(24);editRow->addWidget(allowEdit,1);steps->addLayout(editRow);
  steps->addWidget(note(tr("Without changes it can only look: read the design, take pictures and measure. Access applies to every design you open in OPAD.")));
  steps->addSpacing(10);
  steps->addWidget(stepHeading(3,tr("Try it")));
  steps->addWidget(note(tr("Ask your AI app:")));
  auto* askRow=new QHBoxLayout;steps->addLayout(askRow);
  auto* ask=new QLineEdit;ask->setObjectName("agentConnectPrompt");ask->setReadOnly(true);askRow->addWidget(ask,1);
  auto* copyAsk=new QPushButton(tr("Copy"));askRow->addWidget(copyAsk);
  connect(copyAsk,&QPushButton::clicked,dialog,[ask,copyAsk]{QApplication::clipboard()->setText(ask->text());copyAsk->setText(tr("Copied"));QTimer::singleShot(1500,copyAsk,[copyAsk]{copyAsk->setText(tr("Copy"));});});
  auto* stateRow=new QHBoxLayout;stateRow->setSpacing(6);
  auto* stateIcon=new QLabel;stateIcon->setFixedSize(14,14);
  auto* connectState=new QLabel;connectState->setObjectName("agentConnectState");connectState->setWordWrap(true);
  auto* showActivityLink=new QToolButton;showActivityLink->setObjectName("toastAction");showActivityLink->setAutoRaise(true);showActivityLink->setText(tr("Show activity"));
  connect(showActivityLink,&QToolButton::clicked,this,&AgentBridge::showActivity);
  stateRow->addWidget(stateIcon);stateRow->addWidget(connectState,1);stateRow->addWidget(showActivityLink);steps->addLayout(stateRow);
  steps->addStretch();

  // ---- Access.
  auto* scroll=new QScrollArea;scroll->setObjectName("agentAccessPage");scroll->setWidgetResizable(true);tabs->addTab(scroll,tr("Access"));
  auto* page=new QWidget;scroll->setWidget(page);auto* layout=new QVBoxLayout(page);
  auto* access=new QGroupBox(tr("What AI assistants may do"));auto* accessLayout=new QVBoxLayout(access);layout->addWidget(access);
  auto* enabled=new QCheckBox(tr("Let AI assistants see your designs"));accessLayout->addWidget(enabled);
  auto* edit=new QCheckBox(tr("…and change them (each change can be undone)"));
  auto* editRow2=new QHBoxLayout;editRow2->addSpacing(24);editRow2->addWidget(edit,1);accessLayout->addLayout(editRow2);
  accessLayout->addWidget(note(tr("Access applies to every design you open in OPAD.")));
  auto* working=new QGroupBox(tr("While it works"));auto* workingLayout=new QVBoxLayout(working);layout->addWidget(working);
  auto* follow=new QCheckBox(tr("Zoom to its changes"));follow->setChecked(m_follow);workingLayout->addWidget(follow);
  connect(follow,&QCheckBox::toggled,this,[this](bool on){m_follow=on;QSettings().setValue("agent/follow",on);if(m_feed)m_feed->setFollow(on);});
  auto* announce=new QCheckBox(tr("Tell me when an AI assistant connects"));announce->setChecked(m_announce);workingLayout->addWidget(announce);
  connect(announce,&QCheckBox::toggled,this,[this](bool on){m_announce=on;QSettings().setValue("agent/announce",on);});
  auto* now=new QGroupBox(tr("Connected now"));auto* nowLayout=new QVBoxLayout(now);layout->addWidget(now);
  auto* status=new QLabel;status->setWordWrap(true);nowLayout->addWidget(status);
  auto* nowButtons=new QHBoxLayout;nowLayout->addLayout(nowButtons);
  auto* activityButton=new QPushButton(tr("Show AI activity"));nowButtons->addWidget(activityButton);connect(activityButton,&QPushButton::clicked,this,&AgentBridge::showActivity);
  auto* stopButton=new QPushButton(tr("Stop all AI work"));nowButtons->addWidget(stopButton);connect(stopButton,&QPushButton::clicked,this,&AgentBridge::userStop);
  auto* disconnectButton=new QPushButton(tr("Disconnect all"));nowButtons->addWidget(disconnectButton);connect(disconnectButton,&QPushButton::clicked,this,&AgentBridge::disconnectClients);
  nowButtons->addStretch();
  layout->addWidget(note(tr("Turning access off or down stops unfinished work and removes previews. Finished changes stay in the document and can be undone.")));
  layout->addStretch();

  // ---- Other AI apps: the setup to copy.
  auto* setupScroll=new QScrollArea;setupScroll->setObjectName("agentSetupPage");setupScroll->setWidgetResizable(true);tabs->addTab(setupScroll,tr("Other AI apps"));
  connect(other,&QToolButton::clicked,dialog,[tabs,setupScroll]{tabs->setCurrentWidget(setupScroll);});
  auto* setupPage=new QWidget;setupScroll->setWidget(setupPage);auto* setupLayout=new QVBoxLayout(setupPage);
  setupLayout->addWidget(note(tr("For an AI app not listed on Connect: add OPAD as a local tool server in its settings (often called an MCP server), then restart the app.")));
  for(const auto& line:{tr("1. Copy the setup below into the app's settings."),tr("2. Restart the app."),tr("3. Ask it to open this document in OPAD.")}){auto* l=new QLabel(line);setupLayout->addWidget(l);}
  auto* adapter=new QComboBox;adapter->addItems({tr("Settings file (JSON)"),tr("VS Code"),tr("Command and arguments")});setupLayout->addWidget(adapter);
  auto* config=new QPlainTextEdit;config->setReadOnly(true);config->setObjectName("mono");config->setMaximumHeight(160);setupLayout->addWidget(config);
  opad::json configuration;
  configuration["mcpServers"]["opad"]={{"command",QDir::toNativeSeparators(executable).toStdString()},{"args",{"mcp","--live","--discovery",QDir::toNativeSeparators(m_directory).toStdString()}}};
  config->setPlainText(QString::fromStdString(configuration.dump(2)));
  connect(adapter,&QComboBox::currentIndexChanged,dialog,[=,this](int index){
    auto data=configuration;
    if(index==1){data={{"servers",configuration["mcpServers"]}};data["servers"]["opad"]["type"]="stdio";}
    if(index==2)config->setPlainText(tr("Command: %1\nArguments: %2\nAdd it as a local (STDIO) tool server in the app's settings, then restart the app.").arg(executable,QString::fromStdString(configuration["mcpServers"]["opad"]["args"].dump())));
    else config->setPlainText(QString::fromStdString(data.dump(2)));
  });
  auto* buttons=new QHBoxLayout;setupLayout->addLayout(buttons);auto* copy=new QPushButton(tr("Copy setup"));buttons->addWidget(copy);connect(copy,&QPushButton::clicked,dialog,[config]{QApplication::clipboard()->setText(config->toPlainText());});
  auto* test=new QPushButton(tr("Check OPAD is listening"));buttons->addWidget(test);buttons->addStretch();
  auto* diagnostics=new QLabel;diagnostics->setWordWrap(true);setupLayout->addWidget(diagnostics);
  connect(test,&QPushButton::clicked,dialog,[this,dialog,diagnostics]{
    if(!m_enabled){diagnostics->setText(tr("OPAD isn't listening: turn on AI access first."));return;}
    auto* socket=new QLocalSocket(dialog);auto* timer=new QTimer(socket);timer->setSingleShot(true);
    connect(socket,&QLocalSocket::connected,dialog,[socket,diagnostics]{diagnostics->setText(tr("OPAD is listening. You'll see the assistant on the AI chip once it connects."));socket->disconnectFromServer();socket->deleteLater();});
    connect(timer,&QTimer::timeout,dialog,[socket,diagnostics]{diagnostics->setText(tr("OPAD can't be reached: %1").arg(socket->errorString()));socket->abort();socket->deleteLater();});timer->start(3000);socket->connectToServer(m_endpoint);
  });
  setupLayout->addStretch();

  // The two pages' switches are one setting: either changes it, both show it; changes turns seeing on too.
  connect(enabled,&QCheckBox::toggled,dialog,[this,edit](bool on){setAccess(on,on && edit->isChecked());});connect(edit,&QCheckBox::toggled,dialog,[this](bool on){setAccess(m_enabled || on,on);});
  connect(allow,&QCheckBox::toggled,dialog,[this](bool on){setAccess(on,on && m_edit);});connect(allowEdit,&QCheckBox::toggled,dialog,[this](bool on){setAccess(m_enabled || on,on);});
  auto update=[this,status,connectState,stateIcon,showActivityLink,enabled,edit,allow,allowEdit,ask]{
    const int n=boundCount();const auto name=connectedName();
    QStringList lines;
    for(const auto& s:m_sessions)if(s->bound && s->socket && s->socket->state()==QLocalSocket::ConnectedState)
      lines<<tr("%1 · %2%3").arg(who(s).full,fileLabel(),m_busy && s->socket==m_owner?tr(" · working"):QString());
    status->setText(lines.isEmpty()?tr("No AI assistant is connected."):lines.join("\n"));
    const auto& t=theme::current();
    if(!m_enabled){connectState->setText(tr("AI access is off."));stateIcon->setPixmap(icons::pixmap("aiMark",t.fg3,14,stateIcon->devicePixelRatioF()));}
    else if(n){connectState->setText(tr("%1 is connected to %2").arg(name.brief,fileLabel()));stateIcon->setPixmap(icons::pixmap("check",t.green,14,stateIcon->devicePixelRatioF()));}
    else {connectState->setText(tr("Waiting for an assistant to connect…"));stateIcon->setPixmap(icons::pixmap("aiMark",t.hov,14,stateIcon->devicePixelRatioF()));}
    showActivityLink->setVisible(n>0);
    for(auto* box:{enabled,allow}){QSignalBlocker block(box);box->setChecked(m_enabled);}
    for(auto* box:{edit,allowEdit}){QSignalBlocker block(box);box->setChecked(m_enabled && m_edit);box->setEnabled(m_enabled);}
    ask->setText(tr("Open \"%1\" in OPAD and tell me what you see.").arg(fileLabel()));
  };
  connect(this,&AgentBridge::statusChanged,dialog,update);update();

  tabs->setCurrentIndex(pageWanted>=0 && pageWanted<tabs->count()?pageWanted:clientConnected()?1:0);
  auto* close=new QDialogButtonBox(QDialogButtonBox::Close);outer->addWidget(close);connect(close,&QDialogButtonBox::rejected,dialog,&QDialog::close);dialog->show();
  for(auto* row:rows)row->refresh();  // each client's state at a glance (Codex asks its CLI: asynchronous)
}
