// Settings > AI assistants, one screen: the AI apps found on this computer, each with one Connect / Disconnect button
// (AgentClients.cpp), "Using another AI app?" with one message to copy into any AI app's chat, and what assistants may do as
// one choice (Nothing / Look only / Look and edit, as the chip's menu). After Connect a status line waits for the app and
// turns green by itself when it binds. Details (a page of its own) has what experts need: program paths with Browse, the
// raw settings in every format, Check OPAD is listening, the while-it-works preferences and who is connected now.
// The objectNames are what benches and agents' tests find; the words are for people.
#include "AgentBridge.hpp"
#include "Panels.hpp"
#include "AgentClients.hpp"
#include "Icons.hpp"
#include <QDialog>
#include <QVBoxLayout>
#include <QButtonGroup>
#include <QCheckBox>
#include <QPainter>
#include <QPushButton>
#include <QPlainTextEdit>
#include <QFileInfo>
#include <QDir>
#include <QApplication>
#include <QClipboard>
#include <QLocalSocket>
#include <QComboBox>
#include <QPointer>
#include <QScrollArea>
#include <QSettings>
#include <QSignalBlocker>
#include <QStackedWidget>
#include <QTimer>
#include <QToolButton>
#include <memory>

bool AgentBridge::clientConnected()const{
  for(const auto& s:m_sessions)if(s->bound && s->socket && s->socket->state()==QLocalSocket::ConnectedState)return true;
  return false;
}

namespace {
QLabel* note(const QString& text){
  auto* l=new QLabel(text);l->setWordWrap(true);l->setStyleSheet(QStringLiteral("font-size:12px;color:%1").arg(theme::css(theme::current().fg2)));return l;
}
QLabel* heading(const QString& text){
  auto* l=new QLabel(text);l->setWordWrap(true);l->setStyleSheet(QStringLiteral("font-size:20px;font-weight:600;color:%1").arg(theme::css(theme::current().fg)));return l;
}
QLabel* lead(const QString& text){
  auto* l=new QLabel(text);l->setWordWrap(true);l->setStyleSheet(QStringLiteral("font-size:13px;color:%1").arg(theme::css(theme::current().fg2)));return l;
}
// A section's caption in small capitals.
QLabel* section(const QString& text){
  auto* l=new QLabel(text);QFont f=l->font();f.setCapitalization(QFont::AllUppercase);f.setLetterSpacing(QFont::AbsoluteSpacing,0.6);f.setPointSizeF(f.pointSizeF()*0.85);f.setWeight(QFont::DemiBold);l->setFont(f);
  l->setStyleSheet(QStringLiteral("color:%1").arg(theme::css(theme::current().fg2)));return l;
}
QToolButton* link(const QString& text,const char* name){
  auto* b=new QToolButton;b->setObjectName("toastAction");b->setAutoRaise(true);b->setCursor(Qt::PointingHandCursor);b->setText(text);b->setProperty("role",name);return b;
}
QFrame* card(const char* name,bool sunken=false){
  const auto& t=theme::current();auto* f=new QFrame;f->setObjectName(name);
  f->setStyleSheet(QStringLiteral("QFrame#%1{background:%2;border:1px solid %3;border-radius:10px}").arg(QString::fromLatin1(name),theme::css(sunken?t.bg:t.bg3),theme::css(t.line)));
  return f;
}
// A page that scrolls when the window is small.
QWidget* scrolled(QStackedWidget* pages,const char* name,QVBoxLayout*& layout){
  auto* scroll=new QScrollArea;scroll->setObjectName(name);scroll->setWidgetResizable(true);scroll->setFrameShape(QFrame::NoFrame);
  auto* page=new QWidget;scroll->setWidget(page);layout=new QVBoxLayout(page);layout->setContentsMargins(32,24,32,16);layout->setSpacing(10);
  pages->addWidget(scroll);return scroll;
}
}

void AgentBridge::settings(int pageWanted){
  for(auto* open:m_window->findChildren<QDialog*>("agentSettings"))open->close();  // one at a time: the chip opens it at the page that fits
  auto* dialog=new QDialog(m_window);dialog->setObjectName("agentSettings");dialog->setAttribute(Qt::WA_DeleteOnClose);dialog->setWindowTitle(tr("AI assistants"));dialog->resize(720,780);
  const auto& t=theme::current();
  auto* outer=new QVBoxLayout(dialog);outer->setContentsMargins(0,0,0,0);outer->setSpacing(0);
  auto* pages=new QStackedWidget;pages->setObjectName("agentSettingsPages");outer->addWidget(pages,1);

  // The copy of opad-cli an AI client starts: the one beside this OPAD.
  const auto directory=QCoreApplication::applicationDirPath();QString executable=directory+"/opad-cli";
#ifdef Q_OS_WIN
  executable+=".exe";
#endif
  if(!QFileInfo::exists(executable)){
    const auto candidates=QDir(directory).entryList({"opad-cli-*.exe","opad-cli-*"},QDir::Files);if(!candidates.empty())executable=directory+"/"+candidates.front();
  }
  // Which app the status line waits for: the one just connected, "your AI app" after Copy setup message; empty: no line.
  auto waiting=std::make_shared<QString>();

  // ---- The one screen.
  QVBoxLayout* main=nullptr;auto* mainPage=scrolled(pages,"agentConnectPage",main);
  main->addWidget(heading(tr("Work on your designs with an AI assistant")));
  main->addWidget(lead(tr("Connect the AI app you use. It can then see this design and, if you allow it, change it. Every change can be undone.")));
  main->addSpacing(14);
  main->addWidget(section(tr("Found on this computer")));
  QList<AgentClientRow*> rows;
  for(const auto kind:{AgentClientRow::Client::ClaudeDesktop,AgentClientRow::Client::ClaudeCode,AgentClientRow::Client::Codex}){
    auto* row=new AgentClientRow(kind,executable,m_directory);main->addWidget(row);rows<<row;
  }
  if(!QFileInfo::exists(executable))main->addWidget(note(tr("opad-cli was not found beside OPAD: install the matching opad-cli before connecting an AI app.")));
  main->addSpacing(14);
  auto* other=card("agentOtherCard");auto* otherRow=new QHBoxLayout(other);otherRow->setContentsMargins(16,14,16,14);otherRow->setSpacing(16);
  auto* otherWords=new QVBoxLayout;otherWords->setSpacing(4);
  auto* otherTitle=new QLabel(tr("Using another AI app?"));otherTitle->setStyleSheet(QStringLiteral("font-weight:600;font-size:14px"));otherWords->addWidget(otherTitle);
  otherWords->addWidget(note(tr("Copy one message and paste it into your AI app's chat. It adds OPAD by itself.")));
  otherRow->addLayout(otherWords,1);
  auto* copySetup=new QPushButton(tr("Copy setup message"));copySetup->setObjectName("agentCopySetup");copySetup->setMinimumHeight(32);
  copySetup->setStyleSheet(QStringLiteral("QPushButton#agentCopySetup{background:transparent;border:1px solid %1;border-radius:6px;padding:0 16px;font-weight:600;color:%2}QPushButton#agentCopySetup:hover{background:%3}")
                             .arg(theme::css(t.sel.lighter(130)),theme::css(t.fg),theme::css(t.bg4)));
  otherRow->addWidget(copySetup);main->addWidget(other);
  main->addSpacing(14);
  main->addWidget(section(tr("What may it do?")));
  // Nothing / Look only / Look and edit: one setting, the chip's Off / Look only / Look and edit.
  auto* accessBox=new QWidget;accessBox->setObjectName("agentAccess");auto* accessRow=new QHBoxLayout(accessBox);accessRow->setContentsMargins(0,0,0,0);
  accessBox->setStyleSheet(QStringLiteral("QToolButton#segment:checked{background:%1;color:%2;font-weight:600}").arg(theme::css(t.sel),theme::css(t.onsel)));
  auto* segmented=new QWidget;segmented->setObjectName("segmented");auto* segRow=new QHBoxLayout(segmented);segRow->setContentsMargins(1,1,1,1);segRow->setSpacing(0);
  auto* accessGroup=new QButtonGroup(dialog);accessGroup->setExclusive(true);
  const QStringList accessWords{tr("Nothing"),tr("Look only"),tr("Look and edit")};
  for(int i=0;i<3;++i){
    auto* b=new QToolButton(segmented);b->setObjectName("segment");b->setCheckable(true);b->setAutoRaise(true);b->setFixedHeight(30);b->setText(accessWords[i]);
    b->setMinimumWidth(96);b->setProperty("access",i);segRow->addWidget(b);accessGroup->addButton(b,i);
  }
  accessGroup->button(0)->setToolTip(tr("AI assistants cannot connect."));
  accessGroup->button(1)->setToolTip(tr("Read the design, take pictures and measure. Nothing is changed."));
  accessGroup->button(2)->setToolTip(tr("Also change the design. Each change is a normal step you can undo."));
  accessRow->addWidget(segmented);accessRow->addStretch();main->addWidget(accessBox);
  connect(accessGroup,&QButtonGroup::idClicked,dialog,[this](int id){setAccess(id>0,id==2);});
  main->addWidget(note(tr("You can change this any time from the AI button in the status bar. It applies to every design you open in OPAD.")));
  main->addStretch();

  // ---- Another AI app: the message to paste into its chat.
  QVBoxLayout* copyLayout=nullptr;auto* copyPage=scrolled(pages,"agentCopyPage",copyLayout);
  auto* back1=link(tr("← All AI apps"),"back");copyLayout->addWidget(back1,0,Qt::AlignLeft);
  copyLayout->addWidget(heading(tr("Connect any other AI app")));
  copyLayout->addWidget(lead(tr("Paste this message into your AI app's chat. Its assistant adds OPAD to its own settings, then tells you if it needs a restart.")));
  copyLayout->addSpacing(6);
  auto* messageBox=card("agentMessageCard",true);auto* messageLayout=new QVBoxLayout(messageBox);messageLayout->setContentsMargins(16,14,16,14);messageLayout->setSpacing(10);
  auto* message=new QLabel;message->setObjectName("agentSetupMessage");message->setWordWrap(true);message->setTextInteractionFlags(Qt::TextSelectableByMouse);message->setTextFormat(Qt::PlainText);
  {QFont mono(QStringLiteral("Consolas"));mono.setStyleHint(QFont::Monospace);mono.setPointSizeF(message->font().pointSizeF()*0.95);message->setFont(mono);}
  messageLayout->addWidget(message);
  auto* copiedRow=new QHBoxLayout;
  auto* copied=new QLabel(QStringLiteral("✓ ")+tr("Copied"));copied->setObjectName("agentSetupCopied");copied->setStyleSheet(QStringLiteral("color:%1;font-weight:600").arg(theme::css(t.green)));
  auto* copyAgain=new QPushButton(tr("Copy again"));copyAgain->setObjectName("primary");
  copiedRow->addWidget(copied);copiedRow->addStretch();copiedRow->addWidget(copyAgain);messageLayout->addLayout(copiedRow);
  copyLayout->addWidget(messageBox);
  copyLayout->addSpacing(8);
  copyLayout->addWidget(section(tr("In your AI app")));
  auto* chat=card("agentChatCard");auto* chatLayout=new QVBoxLayout(chat);chatLayout->setContentsMargins(16,14,16,14);chatLayout->setSpacing(10);
  auto bubble=[&t](const QString& text,bool user){
    auto* frame=new QFrame;frame->setObjectName("agentChatBubble");frame->setMaximumWidth(440);
    frame->setStyleSheet(QStringLiteral("QFrame#agentChatBubble{background:%1;border:1px solid %2;border-radius:10px}").arg(theme::css(user?t.bg4:t.bg2),theme::css(user?t.bg4:t.line)));
    auto* box=new QVBoxLayout(frame);box->setContentsMargins(12,10,12,10);auto* l=new QLabel(text);l->setWordWrap(true);box->addWidget(l);return frame;
  };
  auto say=[chatLayout](QFrame* b,bool user){auto* r=new QHBoxLayout;if(user)r->addStretch(1);r->addWidget(b,3);if(!user)r->addStretch(1);chatLayout->addLayout(r);};
  say(bubble(tr("Please connect to OPAD, my CAD app, so you can work on my designs. Add a local MCP server…"),true),true);
  say(bubble(tr("I added the \"opad\" server to my settings. Reload me once, then I can open your design."),false),false);
  copyLayout->addWidget(chat);
  auto* jsonNote=new QLabel(tr("Works with assistants that can change their own settings, such as Claude Code, Codex, Cursor and VS Code. If yours cannot, its settings page takes the same lines: <a href=\"json\">copy them as settings (JSON)</a>."));
  jsonNote->setObjectName("agentCopyJson");jsonNote->setWordWrap(true);jsonNote->setTextFormat(Qt::RichText);
  jsonNote->setStyleSheet(QStringLiteral("font-size:12px;color:%1;background:%2;border-radius:8px;padding:10px").arg(theme::css(t.fg2),theme::css(t.bg3)));
  copyLayout->addWidget(jsonNote);
  copyLayout->addStretch();

  // ---- Details: what experts need, nothing lost.
  QVBoxLayout* details=nullptr;auto* detailsPage=scrolled(pages,"agentDetailsPage",details);
  auto* back2=link(tr("← All AI apps"),"back");details->addWidget(back2,0,Qt::AlignLeft);
  details->addWidget(heading(tr("Details")));
  details->addSpacing(6);
  details->addWidget(section(tr("AI apps and their programs")));
  details->addWidget(note(tr("OPAD finds each app by itself. If an app is installed somewhere else, choose its program here.")));
  for(auto* row:rows){auto* box=card("agentDetailsCard");auto* l=new QVBoxLayout(box);l->setContentsMargins(12,10,12,10);l->addWidget(row->details());details->addWidget(box);}
  details->addSpacing(10);
  details->addWidget(section(tr("Settings to copy")));
  details->addWidget(note(tr("For an app you set up by hand: add OPAD as a local tool server in its settings (often called an MCP server), then restart the app.")));
  auto* adapter=new QComboBox;adapter->setObjectName("agentSetupFormat");adapter->addItems({tr("Settings file (JSON)"),tr("VS Code"),tr("Command and arguments")});details->addWidget(adapter,0,Qt::AlignLeft);
  auto* config=new QPlainTextEdit;config->setReadOnly(true);config->setObjectName("mono");config->setMaximumHeight(160);details->addWidget(config);
  opad::json configuration;
  configuration["mcpServers"]["opad"]={{"command",QDir::toNativeSeparators(executable).toStdString()},{"args",{"mcp","--live","--discovery",QDir::toNativeSeparators(m_directory).toStdString()}}};
  config->setPlainText(QString::fromStdString(configuration.dump(2)));
  connect(adapter,&QComboBox::currentIndexChanged,dialog,[=,this](int index){
    auto data=configuration;
    if(index==1){data={{"servers",configuration["mcpServers"]}};data["servers"]["opad"]["type"]="stdio";}
    if(index==2)config->setPlainText(tr("Command: %1\nArguments: %2\nAdd it as a local (STDIO) tool server in the app's settings, then restart the app.").arg(QDir::toNativeSeparators(executable),QString::fromStdString(configuration["mcpServers"]["opad"]["args"].dump())));
    else config->setPlainText(QString::fromStdString(data.dump(2)));
  });
  auto* buttons=new QHBoxLayout;details->addLayout(buttons);auto* copy=new QPushButton(tr("Copy setup"));buttons->addWidget(copy);connect(copy,&QPushButton::clicked,dialog,[config]{QApplication::clipboard()->setText(config->toPlainText());});
  auto* test=new QPushButton(tr("Check OPAD is listening"));test->setObjectName("agentCheckListening");buttons->addWidget(test);buttons->addStretch();
  auto* diagnostics=new QLabel;diagnostics->setObjectName("agentListening");diagnostics->setWordWrap(true);details->addWidget(diagnostics);
  connect(test,&QPushButton::clicked,dialog,[this,dialog,diagnostics]{
    if(!m_enabled){diagnostics->setText(tr("OPAD isn't listening: choose Look only or Look and edit first."));return;}
    auto* socket=new QLocalSocket(dialog);auto* timer=new QTimer(socket);timer->setSingleShot(true);
    connect(socket,&QLocalSocket::connected,dialog,[socket,diagnostics]{diagnostics->setText(tr("OPAD is listening. You'll see the assistant on the AI chip once it connects."));socket->disconnectFromServer();socket->deleteLater();});
    connect(timer,&QTimer::timeout,dialog,[socket,diagnostics]{diagnostics->setText(tr("OPAD can't be reached: %1").arg(socket->errorString()));socket->abort();socket->deleteLater();});timer->start(3000);socket->connectToServer(m_endpoint);
  });
  details->addSpacing(10);
  details->addWidget(section(tr("While it works")));
  auto* follow=new QCheckBox(tr("Zoom to its changes"));follow->setObjectName("agentFollow");follow->setChecked(m_follow);details->addWidget(follow);
  connect(follow,&QCheckBox::toggled,this,[this](bool on){m_follow=on;QSettings().setValue("agent/follow",on);if(m_feed)m_feed->setFollow(on);});
  auto* announce=new QCheckBox(tr("Tell me when an AI assistant connects"));announce->setObjectName("agentAnnounce");announce->setChecked(m_announce);details->addWidget(announce);
  connect(announce,&QCheckBox::toggled,this,[this](bool on){m_announce=on;QSettings().setValue("agent/announce",on);});
  details->addSpacing(10);
  details->addWidget(section(tr("Connected now")));
  auto* status=new QLabel;status->setObjectName("agentConnectedNow");status->setWordWrap(true);details->addWidget(status);
  auto* nowButtons=new QHBoxLayout;details->addLayout(nowButtons);
  auto* activityButton=new QPushButton(tr("Show AI activity"));nowButtons->addWidget(activityButton);connect(activityButton,&QPushButton::clicked,this,&AgentBridge::showActivity);
  auto* stopButton=new QPushButton(tr("Stop all AI work"));nowButtons->addWidget(stopButton);connect(stopButton,&QPushButton::clicked,this,&AgentBridge::userStop);
  auto* disconnectButton=new QPushButton(tr("Disconnect all"));nowButtons->addWidget(disconnectButton);connect(disconnectButton,&QPushButton::clicked,this,&AgentBridge::disconnectClients);
  nowButtons->addStretch();
  details->addWidget(note(tr("Turning access off or down stops unfinished work and removes previews. Finished changes stay in the document and can be undone.")));
  details->addStretch();

  // ---- The status line (waits for the app, green by itself when it binds) and the footer.
  auto* stateBar=new QWidget;stateBar->setObjectName("agentConnectStateBar");
  auto* stateRow=new QHBoxLayout(stateBar);stateRow->setContentsMargins(32,10,32,10);stateRow->setSpacing(8);
  auto* stateIcon=new QLabel;stateIcon->setFixedSize(14,14);stateIcon->setAlignment(Qt::AlignCenter);
  auto* connectState=new QLabel;connectState->setObjectName("agentConnectState");connectState->setWordWrap(true);
  auto* stateHint=new QLabel(tr("This turns green by itself"));stateHint->setObjectName("secondary");
  auto* showActivityLink=link(tr("Show activity"),"activity");connect(showActivityLink,&QToolButton::clicked,this,&AgentBridge::showActivity);
  stateRow->addWidget(stateIcon);stateRow->addWidget(connectState,1);stateRow->addWidget(stateHint);stateRow->addWidget(showActivityLink);
  stateBar->setStyleSheet(QStringLiteral("QWidget#agentConnectStateBar{background:%1;border-top:1px solid %2}").arg(theme::css(t.bg3),theme::css(t.line)));
  outer->addWidget(stateBar);
  auto* footer=new QWidget;footer->setObjectName("agentSettingsFooter");
  footer->setStyleSheet(QStringLiteral("QWidget#agentSettingsFooter{border-top:1px solid %1}").arg(theme::css(t.line)));
  auto* footerRow=new QHBoxLayout(footer);footerRow->setContentsMargins(24,10,32,12);
  auto* detailsLink=link(tr("Details: program paths, raw settings, check the connection"),"details");
  footerRow->addWidget(detailsLink);footerRow->addStretch();
  auto* done=new QPushButton(tr("Done"));done->setObjectName("agentSettingsDone");done->setDefault(true);footerRow->addWidget(done);
  connect(done,&QPushButton::clicked,dialog,&QDialog::close);
  outer->addWidget(footer);

  auto update=[this,rows,status,connectState,stateIcon,stateHint,stateBar,showActivityLink,accessGroup,waiting]{
    const int n=boundCount();const auto name=connectedName();
    QStringList lines;
    for(const auto& s:m_sessions)if(s->bound && s->socket && s->socket->state()==QLocalSocket::ConnectedState)
      lines<<tr("%1 · %2%3").arg(who(s).full,fileLabel(),m_busy && s->socket==m_owner?tr(" · working"):QString());
    status->setText(lines.isEmpty()?tr("No AI assistant is connected."):lines.join("\n"));
    const auto& t=theme::current();const qreal dpr=stateIcon->devicePixelRatioF();
    auto dot=[stateIcon,dpr](const QColor& c){QPixmap p(QSize(14,14)*dpr);p.setDevicePixelRatio(dpr);p.fill(Qt::transparent);QPainter g(&p);g.setRenderHint(QPainter::Antialiasing);g.setPen(Qt::NoPen);g.setBrush(c);g.drawEllipse(QRectF(3,3,8,8));stateIcon->setPixmap(p);};
    stateBar->setVisible(n>0 || !waiting->isEmpty());
    stateHint->setVisible(false);
    if(n){connectState->setText(tr("%1 is connected to %2").arg(name.brief,fileLabel()));stateIcon->setPixmap(icons::pixmap("check",t.green,14,dpr));connectState->setStyleSheet(QStringLiteral("color:%1").arg(theme::css(t.green)));}
    else if(!m_enabled){connectState->setText(tr("%1 can connect once you choose Look only or Look and edit.").arg(*waiting));dot(t.amber);connectState->setStyleSheet({});}
    else {connectState->setText(tr("Waiting for %1 to connect…").arg(*waiting));dot(t.amber);connectState->setStyleSheet({});stateHint->setVisible(true);}
    showActivityLink->setVisible(n>0);
    const int access=!m_enabled?0:m_edit?2:1;
    if(auto* b=accessGroup->button(access)){QSignalBlocker block(accessGroup);b->setChecked(true);}
    for(auto* row:rows)row->setPrompt(tr("Open \"%1\" in OPAD and tell me what you see.").arg(fileLabel()));
  };
  connect(this,&AgentBridge::statusChanged,dialog,update);

  // Copy setup message: copied at once, and the page shows it with what happens next.
  auto document=[this]{return m_doc->hasDocument?fileLabel():QString();};
  auto copyMessage=[=]{QApplication::clipboard()->setText(message->text());copied->setVisible(true);};
  connect(copySetup,&QPushButton::clicked,dialog,[=,this]{
    message->setText(agentclients::setupMessage(executable,m_directory,document()));copyMessage();
    if(waiting->isEmpty())*waiting=tr("your AI app");
    pages->setCurrentWidget(copyPage);update();
  });
  connect(copyAgain,&QPushButton::clicked,dialog,copyMessage);
  connect(jsonNote,&QLabel::linkActivated,dialog,[=,this]{
    QApplication::clipboard()->setText(agentclients::setupJson(executable,m_directory));
    jsonNote->setToolTip(tr("Copied"));copied->setText(QStringLiteral("✓ ")+tr("Settings copied (JSON)"));
    QTimer::singleShot(2000,copied,[copied]{copied->setText(QStringLiteral("✓ ")+tr("Copied"));});
  });
  for(auto* row:rows)connect(row,&AgentClientRow::connected,dialog,[=]{*waiting=row->name();update();});
  for(auto* back:{back1,back2})connect(back,&QToolButton::clicked,dialog,[pages,mainPage]{pages->setCurrentWidget(mainPage);});
  connect(detailsLink,&QToolButton::clicked,dialog,[pages,detailsPage]{pages->setCurrentWidget(detailsPage);});
  connect(pages,&QStackedWidget::currentChanged,dialog,[pages,detailsPage,detailsLink]{detailsLink->setVisible(pages->currentWidget()!=detailsPage);});
  update();

  pages->setCurrentWidget(pageWanted==1?detailsPage:mainPage);detailsLink->setVisible(pageWanted!=1);
  dialog->show();
  for(auto* row:rows)row->refresh();  // each app's state at a glance (Codex asks its CLI: asynchronous)
}
