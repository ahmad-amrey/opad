#include "opad/live.hpp"
#ifdef OPAD_LIVE_MCP
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QLocalSocket>
#include <QLockFile>
#include <QElapsedTimer>
#endif
#include <iostream>
using opad::json;
int opad_live_mcp(int argc,char** argv) {
#ifndef OPAD_LIVE_MCP
  std::cerr<<"This build has no local bridge support. Install the desktop distribution; no headless fallback was used.\n";return 1;
#else
  QCoreApplication app(argc,argv);QString discovery;
  for(int i=3;i<argc;++i)if(std::string(argv[i])=="--discovery" && i+1<argc)discovery=QString::fromLocal8Bit(argv[++i]);
    else {std::cerr<<"usage: opad-cli mcp --live --discovery <directory>\n";return 1;}
  if(discovery.isEmpty()){std::cerr<<"Copy the live configuration from OPAD Settings > AI integration. --discovery is required.\n";return 1;}
  QLocalSocket socket;json client=json::object();bool initialized=false;std::string line;QByteArray input;
  auto instances=[&]{json list=json::array();for(const auto& file:QDir(discovery).entryInfoList({"*.json"},QDir::Files)){
      QLockFile lock(file.absoluteFilePath()+".lock");if(lock.tryLock(0)){lock.unlock();continue;}
      QFile f(file.absoluteFilePath());if(!f.open(QIODevice::ReadOnly) || f.size()>65536)continue;
      auto item=json::parse(f.readAll().toStdString(),nullptr,false);if(item.is_object())list.push_back(item);
    }return list;};
  auto exchange=[&](const json& request){
    if(socket.state()!=QLocalSocket::ConnectedState)throw opad::Error("disconnected: call live_instances and live_bind; no headless fallback");
    socket.write(QByteArray::fromStdString(request.dump())+'\n');socket.flush();QElapsedTimer timer;timer.start();
    while(!input.contains('\n')){
      if(timer.elapsed()>300000)throw opad::Error("timeout: operation status is unknown; reconnect and request_status before retrying");
      if(!socket.waitForReadyRead(1000) && socket.state()!=QLocalSocket::ConnectedState)throw opad::Error("disconnected: check request_status after reconnect before retrying");
      input+=socket.readAll();if(input.size()>16*1024*1024)throw opad::Error("response exceeds 16 MB");
    }
    const auto end=input.indexOf('\n');const auto answer=json::parse(input.left(end).toStdString());input.remove(0,end+1);return answer;
  };
  while(std::getline(std::cin,line)){
    json id=nullptr;bool hasId=false;
    try{
      if(line.size()>8*1024*1024)throw opad::Error("request exceeds 8 MB");
      auto request=json::parse(line);hasId=request.contains("id");id=request.value("id",json());
      if(!hasId)continue;const auto method=request.at("method").get<std::string>();json result;
      if(method=="initialize"){
        initialized=true;client=request.value("params",json::object()).value("clientInfo",json::object());
        result={{"protocolVersion","2025-11-25"},{"capabilities",{{"tools",json::object()}}},{"serverInfo",{{"name","opad-live"},{"version",opad::version_string()}}},
          {"instructions","LIVE OPAD: call live_instances, explicitly choose the intended window/document, then live_bind. live_state supplies revision, camera and selection. Use context and feature_schema before edits. Every write needs expected_revision and a unique request_id; query request_status after reconnect before retrying. Face/edge/vertex inputs need entity_details reference tokens in references. Transactions stage dependent edits for one Undo step; preview=true stages one edit. Stop discards unfinished work. Never silently switch targets. Read-only access cannot edit or export. Geometry calculations run in OPAD and each committed edit appears live; file saves remain with the user."}};
      } else if(method=="ping")result=json::object();
      else if(!initialized)throw opad::Error("initialize first");
      else if(method=="tools/list")result={{"tools",opad::agent::live_tools()}};
      else if(method=="tools/call"){
        try{
          auto params=request.at("params");auto name=params.at("name").get<std::string>();auto args=params.value("arguments",json::object());
          opad::agent::validate_input(opad::agent::live_schema(name),args);
          if(name=="live_instances")result=opad::agent::live_result({{"instances",instances()}});
          else if(name=="live_bind"){
            json chosen;for(const auto& item:instances())if(item.value("instance","")==args.at("instance").get<std::string>() && item.value("target","")==args.at("target").get<std::string>())chosen=item;
            if(chosen.is_null())throw opad::Error("target_unavailable: refresh live_instances and choose explicitly");
            socket.abort();input.clear();socket.connectToServer(QString::fromStdString(chosen.at("endpoint").get<std::string>()));
            if(!socket.waitForConnected(3000))throw opad::Error("connection_failed: enable AI integration in the intended OPAD window");
            result=exchange({{"name","live_bind"},{"arguments",args},{"client",client},{"version",opad::version_string()}});
          }else result=exchange({{"name",name},{"arguments",args}});
        }catch(const std::exception& e){result=opad::agent::live_error("live_request_failed",e.what());}
      }else {std::cout<<json{{"jsonrpc","2.0"},{"id",id},{"error",{{"code",-32601},{"message","method not found"}}}}.dump()<<'\n'<<std::flush;continue;}
      if(result.contains("structuredContent") && result["content"].empty())result["content"].push_back({{"type","text"},{"text",result["structuredContent"].dump()}});
      std::cout<<json{{"jsonrpc","2.0"},{"id",id},{"result",result}}.dump()<<'\n'<<std::flush;
    }catch(const std::exception& e){if(hasId)std::cout<<json{{"jsonrpc","2.0"},{"id",id},{"error",{{"code",-32600},{"message",e.what()}}}}.dump()<<'\n'<<std::flush;}
  }return 0;
#endif
}
