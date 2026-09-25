#pragma once
#include <QWidget>
#include <QStringList>
#include <functional>
#include "opad/core.hpp"
class QLabel;
class QLineEdit;
class QPushButton;
// Client registration changes only the named Codex server. It never binds a document
// or grants OPAD editing access. All CLI invocations are asynchronous, without a shell.
class AgentRegistration : public QWidget {
 public:
  AgentRegistration(QString executable,QString discovery,QWidget* parent=nullptr);
  void refresh();
  void registerClient();
  void unregisterClient();
  QString report() const;
 private:
  void run(const QStringList&,std::function<void(bool,QString)>);
  void inspect(std::function<void(bool,opad::json)>);
  bool matches(const opad::json&) const;
  void message(const QString&);
  QString m_executable;QStringList m_arguments;
  QLineEdit* m_cli;QLabel* m_status;QPushButton *m_connect,*m_remove,*m_refresh;
  bool m_running=false;
};
