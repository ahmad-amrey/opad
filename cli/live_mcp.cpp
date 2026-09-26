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
namespace {struct TransportFailure {json result;};}
int opad_live_mcp(int argc,char** argv) {
#ifndef OPAD_LIVE_MCP
  std::cerr<<"This build has no local bridge support. Install the desktop distribution; no headless fallback was used.\n";return 1;
#else
  QCoreApplication app(argc,argv);QString discovery;
  for(int i=3;i<argc;++i)if(std::string(argv[i])=="--discovery" && i+1<argc)discovery=QString::fromLocal8Bit(argv[++i]);
    else {std::cerr<<"usage: opad-cli mcp --live --discovery <directory>\n";return 1;}
  if(discovery.isEmpty()){std::cerr<<"Copy the live configuration from OPAD Settings > AI integration. --discovery is required.\n";return 1;}
  QLocalSocket socket;json client=json::object(),lastError=nullptr;bool initialized=false,bound=false;std::string line;QByteArray input;
  auto instances=[&]{json list=json::array();for(const auto& file:QDir(discovery).entryInfoList({"*.json"},QDir::Files)){
      QLockFile lock(file.absoluteFilePath()+".lock");if(lock.tryLock(0)){lock.unlock();continue;}
      QFile f(file.absoluteFilePath());if(!f.open(QIODevice::ReadOnly) || f.size()>65536)continue;
      auto item=json::parse(f.readAll().toStdString(),nullptr,false);if(item.is_object())list.push_back(item);
    }return list;};
  auto exchange=[&](const json& request){
    auto fail=[&](const char* code,const char* message){
      auto result=opad::agent::live_error(code,message);
      result["structuredContent"]["error"]["transport"]={{"kind","local_socket"},{"qt_error",int(socket.error())},{"message",socket.errorString().toStdString()}};
      result["structuredContent"]["error"]["next"]="Call live_instances, bind the same target and query request_status before retrying an uncertain write. Disconnected transactions cannot resume.";
      // Do not let a late reply from a timed-out request become the next call's reply.
      socket.abort();input.clear();throw TransportFailure{std::move(result)};
    };
    if(socket.state()!=QLocalSocket::ConnectedState && !bound){
      // Nothing was bound yet (or the last bind failed): say what to call, not that a connection dropped.
      auto result=opad::agent::live_error("not_bound","This connection is not bound to an OPAD window yet. Call live_instances, choose the window and document, then live_bind.");
      result["structuredContent"]["error"]["next"]=json{"live_instances","live_bind"};throw TransportFailure{std::move(result)};
    }
    if(socket.state()!=QLocalSocket::ConnectedState)fail("disconnected","The local connection is closed; no headless fallback was used.");
    socket.write(QByteArray::fromStdString(request.dump())+'\n');socket.flush();QElapsedTimer timer;timer.start();
    while(!input.contains('\n')){
      if(timer.elapsed()>300000)fail("operation_timeout","No operation response within 300000 ms; commit status is unknown. The connection was closed.");
      if(!socket.waitForReadyRead(1000) && socket.state()!=QLocalSocket::ConnectedState)fail("disconnected","The local connection closed before the operation response; commit status may be unknown.");
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
          {"instructions","LIVE OPAD: start with live_diagnostics (include_example=true for a short workflow), then explicitly choose a window/document using live_instances and live_bind. Every write needs expected_revision and a unique request_id. Transactions are connection-scoped: keep the MCP process alive; disconnect discards uncommitted work. Keep expected_revision=base_revision while staging and committing. After reconnect query request_status before retrying an uncertain commit; cancelled groups must be replanned. result.feature_id is a history ID, result.body_ids are body IDs, result.sketch_id is a sketch ID; legacy ids are operation IDs. changes.scope identifies command versus cumulative transaction changes. Review context section ai_agent_notes first: AI agent notes are user requests tied to model anchors; fetch annotations by id for full text, comments and drawing strokes, inspect current references, and resolve only after verifying completion. Use context and feature_schema before edits. Face/edge/vertex inputs need current entity_details reference tokens in references. preview=true stages one edit. Stop discards unfinished work. Never silently switch targets. Use save after committing work to persist the live document: supply an absolute .opad path for the first save, then omit path for later saves. Read-only access cannot edit, export or save."}};
      } else if(method=="ping")result=json::object();
      else if(!initialized)throw opad::Error("initialize first");
      else if(method=="tools/list")result={{"tools",opad::agent::live_tools()}};
      else if(method=="tools/call"){
        try{
          auto params=request.at("params");auto name=params.at("name").get<std::string>();auto args=params.value("arguments",json::object());
          opad::agent::validate_input(opad::agent::live_schema(name),args);
          if(name=="live_instances")result=opad::agent::live_result({{"instances",instances()}});
          else if(name=="live_diagnostics" && socket.state()!=QLocalSocket::ConnectedState){
            json info={{"connection","unbound"},{"target",nullptr},{"permissions",{{"confirmed",false}}},{"units",nullptr},
              {"transaction_state",{{"state","none"},{"scope","connection"}}},{"next_calls",{"live_instances","live_bind"}},
              {"discovery_directory",discovery.toStdString()},{"discovery_exists",QDir(discovery).exists()},{"instances",instances()}};
            if(!lastError.is_null())info["last_error"]=lastError;
            if(args.value("include_example",false))info["guide"]=opad::agent::live_guide();result=opad::agent::live_result(info);
          }
          else if(name=="live_bind"){
            json chosen;for(const auto& item:instances())if(item.value("instance","")==args.at("instance").get<std::string>() && item.value("target","")==args.at("target").get<std::string>())chosen=item;
            if(chosen.is_null())result=opad::agent::live_error("target_changed","The requested instance/document is no longer advertised. Refresh live_instances and explicitly choose the current target. The previous binding was not changed.");
            else if(!chosen.value("enabled",false))result=opad::agent::live_error("access_disabled","This window advertises agent access as disabled. Enable it in OPAD Settings > AI integration.");
            else {
              socket.abort();input.clear();bound=false;socket.connectToServer(QString::fromStdString(chosen.at("endpoint").get<std::string>()));
              if(!socket.waitForConnected(3000)) {
                const auto error=socket.error();std::string code="connection_failed",next="Check the endpoint and client execution environment. Sandbox restrictions may prevent access; this is not a confirmed OS diagnosis.";
                if(error==QLocalSocket::SocketAccessError){code="access_denied";next="The local transport reports access denied. Check user identity and sandbox permissions; toggling integration will not resolve an access restriction.";}
                else if(error==QLocalSocket::ServerNotFoundError || error==QLocalSocket::ConnectionRefusedError){code="endpoint_unavailable";next="Refresh live_instances; the window may have closed or its listener may be unavailable. If the endpoint works outside a sandbox, investigate its access restrictions.";}
                else if(error==QLocalSocket::SocketTimeoutError){code="connection_timeout";next="The endpoint did not accept a connection within 3000 ms. Check OPAD responsiveness and retry discovery.";}
                result=opad::agent::live_error(code,"Could not connect to the advertised OPAD endpoint.");auto& detail=result["structuredContent"]["error"];
                detail["next"]=next;detail["transport"]={{"kind","local_socket"},{"qt_error",int(error)},{"message",socket.errorString().toStdString()},{"endpoint",chosen["endpoint"]},{"advertised_enabled",chosen["enabled"]},{"timeout_ms",3000}};
              } else {result=exchange({{"name","live_bind"},{"arguments",args},{"client",client},{"version",opad::version_string()}});bound=!result.value("isError",false);}
            }
            if(result.value("isError",false))lastError=result["structuredContent"]["error"];else lastError=nullptr;
          }else result=exchange({{"name",name},{"arguments",args}});
        }catch(const TransportFailure& e){result=e.result;lastError=result["structuredContent"]["error"];}
        catch(const std::exception& e){result=opad::agent::live_error("live_request_failed",e.what());}
      }else {std::cout<<json{{"jsonrpc","2.0"},{"id",id},{"error",{{"code",-32601},{"message","method not found"}}}}.dump()<<'\n'<<std::flush;continue;}
      if(result.contains("structuredContent") && result["content"].empty())result["content"].push_back({{"type","text"},{"text",result["structuredContent"].dump()}});
      std::cout<<json{{"jsonrpc","2.0"},{"id",id},{"result",result}}.dump()<<'\n'<<std::flush;
    }catch(const std::exception& e){if(hasId)std::cout<<json{{"jsonrpc","2.0"},{"id",id},{"error",{{"code",-32600},{"message",e.what()}}}}.dump()<<'\n'<<std::flush;}
  }return 0;
#endif
}
