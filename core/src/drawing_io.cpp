#include "opad/drawing_io.hpp"
#include "opad/geometry.hpp"
#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <gp_Pln.hxx>
#include <StdPrs_BRepTextBuilder.hxx>
#include <StdPrs_BRepFont.hxx>
#include <BRepBuilderAPI_MakeVertex.hxx>
#include <BRepBuilderAPI_Transform.hxx>
#include <BRepBuilderAPI_GTransform.hxx>
#include <Geom_BezierCurve.hxx>
#include <gp_Elips.hxx>
#include <TColgp_Array1OfPnt.hxx>
#include <BRepAdaptor_Curve.hxx>
#include <BRepTools.hxx>
#include <BRep_Builder.hxx>
#include <LDOMParser.hxx>
#include <LDOM_Element.hxx>
#include <Poly_Triangulation.hxx>
#include <TopTools_FormatVersion.hxx>
#include <TopoDS_Compound.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS.hxx>
#include <TopExp_Explorer.hxx>
#include <Standard_Failure.hxx>
#include <gp_Circ.hxx>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <cstdlib>
#include <cerrno>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

namespace opad {
namespace {
std::string xml_text(const LDOMString& value) {
  Standard_Integer integer;
  return value.Type() == LDOMBasicString::LDOM_Integer && value.GetInteger(integer) ? std::to_string(integer) : value.GetString();
}
struct Conversion {
  std::filesystem::path directory = std::filesystem::temp_directory_path() / ("opad-convert-" + new_uuid());
  Conversion() { std::filesystem::create_directory(directory); }
  ~Conversion() { std::error_code error; std::filesystem::remove_all(directory, error); }
};
void convert_dwg(const std::filesystem::path& in, const std::filesystem::path& out, bool toDwg) {
  const char* override = std::getenv(toDwg ? "OPAD_DXF2DWG" : "OPAD_DWG2DXF");
  const std::string program = override && *override ? override : toDwg ? "dxf2dwg" : "dwg2dxf";
  std::vector<std::string> args = {program, "-y", "-o", out.string(), in.string()};
  int status = -1;
#ifdef _WIN32
  std::wstring command;
  for (const auto& a : args) {
    command += L"\""; unsigned slashes=0;
    for(wchar_t c:std::filesystem::path(a).wstring()) {
      if(c==L'\\') { ++slashes; continue; }
      command.append(c==L'\"'?slashes*2+1:slashes,L'\\'); slashes=0; command+=c;
    }
    command.append(slashes*2,L'\\'); command+=L"\" ";
  }
  STARTUPINFOW startup{}; startup.cb=sizeof(startup); startup.dwFlags=STARTF_USESTDHANDLES;
  startup.hStdInput=GetStdHandle(STD_INPUT_HANDLE);
  startup.hStdOutput=startup.hStdError=GetStdHandle(STD_ERROR_HANDLE);
  PROCESS_INFORMATION process{};
  if(CreateProcessW(nullptr,command.data(),nullptr,nullptr,TRUE,CREATE_NO_WINDOW,nullptr,nullptr,&startup,&process)) {
    if(WaitForSingleObject(process.hProcess,120000)==WAIT_OBJECT_0) { DWORD code; if(GetExitCodeProcess(process.hProcess,&code)) status=int(code); }
    else { TerminateProcess(process.hProcess,1); WaitForSingleObject(process.hProcess,5000); }
    CloseHandle(process.hThread); CloseHandle(process.hProcess);
  }
#else
  std::vector<char*> ptrs; for (auto& a : args) ptrs.push_back(a.data()); ptrs.push_back(nullptr);
  pid_t pid;
  posix_spawn_file_actions_t actions; posix_spawn_file_actions_init(&actions);
  posix_spawn_file_actions_adddup2(&actions, STDERR_FILENO, STDOUT_FILENO);
  const int error = posix_spawnp(&pid, program.c_str(), &actions, nullptr, ptrs.data(), environ);
  posix_spawn_file_actions_destroy(&actions);
  if (!error) { int code=0; while(waitpid(pid,&code,0)<0 && errno==EINTR) {} if(WIFEXITED(code)) status=WEXITSTATUS(code); }
#endif
  if (status != 0 || !std::filesystem::exists(out))
    throw Error("DWG conversion failed or converter unavailable. Install LibreDWG and set " + std::string(toDwg ? "OPAD_DXF2DWG" : "OPAD_DWG2DXF") + " to its executable path. Alternatively convert the file to DXF manually.");
}
std::string extension(const std::filesystem::path& file) {
  std::string e = file.extension().string();
  std::transform(e.begin(), e.end(), e.begin(), [](unsigned char c) { return std::tolower(c); });
  return e;
}
double number(const std::string& s) {
  size_t used = 0; double v;
  try { v = std::stod(s, &used); } catch (...) { throw Error("invalid drawing coordinate: " + s.substr(0, 60)); }
  if (!std::isfinite(v) || std::abs(v) > 1e12 || s.find_first_not_of(" \r\t", used) != std::string::npos)
    throw Error("invalid drawing coordinate: " + s.substr(0, 60));
  return v;
}
struct Drawing {
  std::map<std::string, TopoDS_Compound> layers;
  std::map<std::string, bool> visible;
  std::map<std::string, json> images;
  std::vector<std::string> warnings;
  BRep_Builder builder;
  Mat4 transform;
  void add(const std::string& layer, const TopoDS_Shape& s) {
    if (layers.size() > 10000) throw Error("drawing exceeds 10000 layers");
    auto& c = layers[layer]; if (c.IsNull()) builder.MakeCompound(c);
    if(transform.is_identity()) builder.Add(c,s);
    else if(mat_is_rigid(transform)) builder.Add(c,BRepBuilderAPI_Transform(s,trsf_from_mat(transform),true).Shape());
    else {
      gp_GTrsf t; for(int r=0;r<3;++r)for(int col=0;col<4;++col)t.SetValue(r+1,col+1,transform.at(r,col));
      builder.Add(c,BRepBuilderAPI_GTransform(s,t,true).Shape());
    }
  }
  void line(const std::string& layer, double x, double y, double u, double v) {
    if (std::hypot(x-u, y-v) > 1e-9) add(layer, BRepBuilderAPI_MakeEdge(gp_Pnt(x,y,0), gp_Pnt(u,v,0)).Edge());
  }
  void circle(const std::string& layer, double x, double y, double r, double a = 0, double b = 2*M_PI) {
    if (!(r > 1e-9)) throw Error("drawing circle radius must be positive");
    gp_Circ c(gp_Ax2(gp_Pnt(x,y,0), gp::DZ()), r);
    add(layer, BRepBuilderAPI_MakeEdge(c, a, b).Edge());
  }
};

// SVG numbers allow comma/space separators and adjacent signs ("10-5").
struct SvgNumbers {
  std::string text; size_t at=0;
  void skip() { while(at<text.size() && (std::isspace(static_cast<unsigned char>(text[at])) || text[at]==',')) ++at; }
  bool end() { skip(); return at==text.size(); }
  double next() {
    skip(); const char* first=text.c_str()+at; char* last=nullptr;
    double value=std::strtod(first,&last);
    if(last==first || !std::isfinite(value) || std::abs(value)>1e12) throw Error("invalid SVG path number");
    at=size_t(last-text.c_str()); return value;
  }
};
Mat4 svg_transform(const std::string& text) {
  Mat4 result; size_t at=0;
  while(at<text.size()) {
    while(at<text.size() && (std::isspace(static_cast<unsigned char>(text[at]))||text[at]==','))++at;
    if(at==text.size())break;
    size_t open=text.find('(',at),close=text.find(')',open);
    if(open==std::string::npos||close==std::string::npos)throw Error("invalid SVG transform");
    std::string name=text.substr(at,open-at); while(!name.empty()&&name.back()==' ')name.pop_back();
    SvgNumbers values{text.substr(open+1,close-open-1)};std::vector<double> v;
    while(!values.end())v.push_back(values.next());
    Mat4 m;
    if(name=="matrix"&&v.size()==6) {m.at(0,0)=v[0];m.at(1,0)=-v[1];m.at(0,1)=-v[2];m.at(1,1)=v[3];m.at(0,3)=v[4];m.at(1,3)=-v[5];}
    else if(name=="translate"&&(v.size()==1||v.size()==2))m=Mat4::translation(v[0],v.size()==2?-v[1]:0,0);
    else if(name=="scale"&&(v.size()==1||v.size()==2)) {m.at(0,0)=v[0];m.at(1,1)=v.size()==2?v[1]:v[0];}
    else if(name=="rotate"&&(v.size()==1||v.size()==3)) {
      const double a=-v[0]*M_PI/180;m.at(0,0)=m.at(1,1)=std::cos(a);m.at(0,1)=-std::sin(a);m.at(1,0)=std::sin(a);
      if(v.size()==3)m=Mat4::translation(v[1],-v[2],0)*m*Mat4::translation(-v[1],v[2],0);
    } else if(name=="skewX"&&v.size()==1)m.at(0,1)=-std::tan(v[0]*M_PI/180);
    else if(name=="skewY"&&v.size()==1)m.at(1,0)=-std::tan(v[0]*M_PI/180);
    else throw Error("unsupported SVG transform: "+name);
    if(std::abs(m.at(0,0)*m.at(1,1)-m.at(0,1)*m.at(1,0))<1e-12)throw Error("degenerate SVG transform");
    result=result*m;at=close+1;
  }
  return result;
}
void svg_path(Drawing& out,const std::string& layer,const std::string& data) {
  SvgNumbers s{data}; char cmd=0, previous=0; double x=0,y=0,sx=0,sy=0,cx=0,cy=0;
  bool moved=false;
  while(!s.end()) {
    if(std::isalpha(static_cast<unsigned char>(s.text[s.at]))) cmd=s.text[s.at++];
    if(!cmd) throw Error("SVG path needs a command");
    const char op=char(std::toupper(static_cast<unsigned char>(cmd))); const bool relative=std::islower(static_cast<unsigned char>(cmd));
    if(!moved && op!='M') throw Error("SVG path must start with moveto");
    auto point=[&] { double u=s.next(),v=s.next(); return std::array<double,2>{u+(relative?x:0),v+(relative?y:0)}; };
    if(op=='Z') { out.line(layer,x,-y,sx,-sy); x=sx;y=sy;cmd=0; }
    else if(op=='M' || op=='L') { auto p=point(); if(op=='L')out.line(layer,x,-y,p[0],-p[1]); else { sx=p[0];sy=p[1];moved=true;cmd=relative?'l':'L'; } x=p[0];y=p[1]; }
    else if(op=='H') { double u=s.next()+(relative?x:0);out.line(layer,x,-y,u,-y);x=u; }
    else if(op=='V') { double v=s.next()+(relative?y:0);out.line(layer,x,-y,x,-v);y=v; }
    else if(op=='C' || op=='S' || op=='Q' || op=='T') {
      const bool cubic=op=='C'||op=='S';
      std::array<double,2> a,b,p;
      if(op=='S'||op=='T') { bool reflect=cubic?(previous=='C'||previous=='S'):(previous=='Q'||previous=='T'); a={reflect?2*x-cx:x,reflect?2*y-cy:y}; } else a=point();
      if(cubic) b=point();
      p=point(); TColgp_Array1OfPnt poles(1,cubic?4:3);
      poles(1)=gp_Pnt(x,-y,0);poles(2)=gp_Pnt(a[0],-a[1],0);
      if(cubic)poles(3)=gp_Pnt(b[0],-b[1],0);
      poles(poles.Upper())=gp_Pnt(p[0],-p[1],0);
      out.add(layer,BRepBuilderAPI_MakeEdge(new Geom_BezierCurve(poles)).Edge());
      cx=cubic?b[0]:a[0];cy=cubic?b[1]:a[1];x=p[0];y=p[1];
    } else if(op=='A') {
      double rx=std::abs(s.next()),ry=std::abs(s.next()),phi=s.next()*M_PI/180;
      double large=s.next(),sweep=s.next(); auto p=point();
      if((large!=0&&large!=1)||(sweep!=0&&sweep!=1))throw Error("SVG arc flags must be 0 or 1");
      if(std::hypot(p[0]-x,p[1]-y)<1e-9) {x=p[0];y=p[1];previous=op;continue;}
      if(rx<1e-9||ry<1e-9)out.line(layer,x,-y,p[0],-p[1]);
      else {
        // W3C SVG endpoint-to-center conversion, including radii correction.
        const double cs=std::cos(phi),sn=std::sin(phi),xp=cs*(x-p[0])/2+sn*(y-p[1])/2,yp=-sn*(x-p[0])/2+cs*(y-p[1])/2;
        double lambda=xp*xp/(rx*rx)+yp*yp/(ry*ry); if(lambda>1){rx*=std::sqrt(lambda);ry*=std::sqrt(lambda);}
        const double f=(large==sweep?-1:1)*std::sqrt(std::max(0.0,(rx*rx*ry*ry-rx*rx*yp*yp-ry*ry*xp*xp)/(rx*rx*yp*yp+ry*ry*xp*xp)));
        const double ax=f*rx*yp/ry,ay=-f*ry*xp/rx,cx=cs*ax-sn*ay+(x+p[0])/2,cy=sn*ax+cs*ay+(y+p[1])/2;
        const double ux=(xp-ax)/rx,uy=(yp-ay)/ry,vx=(-xp-ax)/rx,vy=(-yp-ay)/ry;
        double theta=std::atan2(uy,ux),delta=std::atan2(ux*vy-uy*vx,ux*vx+uy*vy);
        if(!sweep&&delta>0)delta-=2*M_PI;
        if(sweep&&delta<0)delta+=2*M_PI;
        if(rx<ry) {std::swap(rx,ry);phi+=M_PI/2;theta-=M_PI/2;}
        gp_Elips ellipse(gp_Ax2(gp_Pnt(cx,-cy,0),gp_Dir(0,0,-1),gp_Dir(std::cos(phi),-std::sin(phi),0)),rx,ry);
        double a=theta,b=theta+delta;if(b<a)std::swap(a,b);
        out.add(layer,BRepBuilderAPI_MakeEdge(ellipse,a,b).Edge());
      }
      x=p[0];y=p[1];
    } else throw Error("Unsupported SVG path command: " + std::string(1,cmd));
    previous=op;
  }
}

Drawing read_dxf(const std::filesystem::path& file) {
  std::ifstream in(file); if (!in) throw Error("cannot open DXF");
  struct Pair { int code; std::string value; };
  std::vector<Pair> pairs;
  std::string a, b;
  while (std::getline(in,a)) {
    if (!std::getline(in,b)) throw Error("truncated DXF group");
    if (!b.empty() && b.back() == '\r') b.pop_back();
    double code = number(a); if (code != std::floor(code) || code < 0 || code > 1071) throw Error("invalid DXF group code");
    pairs.push_back({int(code),b});
  }
  if (pairs.empty() || pairs.back().code != 0 || pairs.back().value != "EOF") throw Error("DXF missing EOF marker");
  double unitScale=1.0;
  for(size_t i=0;i+1<pairs.size();++i) if(pairs[i].code==9 && pairs[i].value=="$INSUNITS") {
    const int units=int(number(pairs[i+1].value));
    switch(units) {
      case 0: case 4: unitScale=1;break;
      case 1: unitScale=25.4;break;
      case 2: unitScale=304.8;break;
      case 5: unitScale=10;break;
      case 6: unitScale=1000;break;
      case 7: unitScale=1000000;break;
      case 13: unitScale=0.001;break;
      default: throw Error("DXF insertion units are unsupported; convert the drawing to millimetres");
    }
  }
  Drawing out; bool entities = false;
  for (size_t i=0; i<pairs.size();) {
    if (pairs[i].code != 0) { ++i; continue; }
    const std::string type = pairs[i].value;
    size_t end=i+1; while(end<pairs.size() && pairs[end].code != 0) ++end;
    auto str = [&](int code, const std::string& fallback = "") { for(size_t j=i+1;j<end;++j) if(pairs[j].code==code) return pairs[j].value; return fallback; };
    auto num = [&](int code, double fallback=0) { auto s=str(code); return s.empty()?fallback:number(s); };
    if (type=="SECTION") entities = str(2)=="ENTITIES";
    else if (type=="ENDSEC") entities=false;
    else if(type=="LAYER") out.visible[str(2,"0")] = num(62,7)>=0 && !(int(num(70)) & 1);
    else if(entities) {
      const auto layer=str(8,"0");
      if (num(30)!=0 || num(31)!=0 || num(38)!=0 || num(210)!=0 || num(220)!=0 || num(230,1)!=1)
        throw Error("DXF: only planar XY entities are supported; project to XY before import");
      if(type=="LINE") out.line(layer,num(10),num(20),num(11),num(21));
      else if(type=="CIRCLE") out.circle(layer,num(10),num(20),num(40));
      else if(type=="ARC") { double start=num(50)*M_PI/180, stop=num(51)*M_PI/180; if(stop<=start)stop+=2*M_PI; out.circle(layer,num(10),num(20),num(40),start,stop); }
      else if(type=="POINT") out.add(layer,BRepBuilderAPI_MakeVertex(gp_Pnt(num(10),num(20),0)).Vertex());
      else if(type=="LWPOLYLINE") {
        std::vector<std::array<double,3>> points;
        for(size_t j=i+1;j<end;++j) {
          if(pairs[j].code==10) points.push_back({number(pairs[j].value),0,0});
          if(pairs[j].code==20 && !points.empty()) points.back()[1]=number(pairs[j].value);
          if(pairs[j].code==42 && !points.empty()) points.back()[2]=number(pairs[j].value);
        }
        if(points.size()<2) throw Error("DXF polyline needs two points");
        size_t segments=points.size()-1+(int(num(70))&1);
        for(size_t k=0;k<segments;++k) {
          auto p=points[k], q=points[(k+1)%points.size()];
          if(std::abs(p[2])<1e-12) out.line(layer,p[0],p[1],q[0],q[1]);
          else {
            const double dx=q[0]-p[0], dy=q[1]-p[1], chord=std::hypot(dx,dy), bulge=p[2];
            if(chord<1e-9) throw Error("degenerate DXF bulge");
            const double cx=(p[0]+q[0])/2-dy*(1-bulge*bulge)/(4*bulge), cy=(p[1]+q[1])/2+dx*(1-bulge*bulge)/(4*bulge);
            double start=std::atan2(p[1]-cy,p[0]-cx), stop=start+4*std::atan(bulge);
            if(bulge<0) std::swap(start,stop);
            out.circle(layer,cx,cy,chord*(1+bulge*bulge)/(4*std::abs(bulge)),start,stop);
          }
        }
      } else throw Error("Unsupported DXF entity: " + type + ". Explode blocks/text/hatches to supported curves before importing.");
    }
    i=end;
  }
  if(out.layers.empty()) throw Error("DXF contains no supported geometry");
  if(unitScale!=1) {
    gp_Trsf scale;scale.SetScale(gp_Pnt(0,0,0),unitScale);
    for(auto& [name,shape]:out.layers)shape=TopoDS::Compound(BRepBuilderAPI_Transform(shape,scale,true).Shape());
  }
  return out;
}

std::string base64(const std::string& bytes) {
  static constexpr char alphabet[]="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  for(size_t i=0;i<bytes.size();i+=3) {
    unsigned bits=unsigned((unsigned char)bytes[i])<<16;
    if(i+1<bytes.size()) bits|=unsigned((unsigned char)bytes[i+1])<<8;
    if(i+2<bytes.size()) bits|=unsigned((unsigned char)bytes[i+2]);
    out+=alphabet[(bits>>18)&63]; out+=alphabet[(bits>>12)&63];
    out+=i+1<bytes.size()?alphabet[(bits>>6)&63]:'='; out+=i+2<bytes.size()?alphabet[bits&63]:'=';
  }
  return out;
}

Drawing read_svg(const std::filesystem::path& file) {
  const std::string text=read_text_file(file);
  if(text.find("<!DOCTYPE")!=std::string::npos || text.find("<!ENTITY")!=std::string::npos) throw Error("SVG external entities/DOCTYPE are not supported");
  LDOMParser parser; std::istringstream stream(text);
  if(parser.parse(stream)) throw Error("invalid SVG XML");
  auto root=parser.getDocument().getDocumentElement();
  auto local=[](std::string tag) { const auto colon=tag.find(':'); return colon==std::string::npos?tag:tag.substr(colon+1); };
  if(local(root.getTagName().GetString())!="svg") throw Error("expected SVG root");
  Drawing out;
  std::map<std::string,LDOM_Element> ids;
  std::function<void(const LDOM_Element&,int)> index;
  index=[&](const LDOM_Element& e,int depth) {
    if(depth>64) throw Error("SVG hierarchy too deep");
    const auto id=xml_text(e.getAttribute("id")); if(!id.empty()) ids.insert_or_assign(id,e);
    for(auto child=e.getFirstChild();!child.isNull();child=child.getNextSibling())
      if(child.getNodeType()==LDOM_Node::ELEMENT_NODE) index(static_cast<const LDOM_Element&>(child),depth+1);
  };
  index(root,0);
  auto length=[](const std::string& value) {
    size_t used=0; double n=std::stod(value,&used); const auto unit=value.substr(used);
    const double factor=unit.empty()||unit=="px"?1:unit=="mm"?96/25.4:unit=="cm"?960/25.4:unit=="in"?96:unit=="pt"?96/72.0:unit=="pc"?16:0;
    if(!factor||!std::isfinite(n)||std::abs(n)>1e12) throw Error("unsupported SVG length: "+value);
    return n*factor;
  };
  std::function<void(const LDOM_Element&,std::string,int)> walk;
  walk=[&](const LDOM_Element& e,std::string layer,int depth) {
    if(depth>64) throw Error("SVG reference cycle or hierarchy too deep");
    auto attr=[&](const char* key) { return xml_text(e.getAttribute(key)); };
    auto property=[&](const char* key,const std::string& fallback) {
      auto value=attr(key); if(!value.empty()) return value;
      std::istringstream css(attr("style")); std::string declaration;
      while(std::getline(css,declaration,';')) {
        const auto colon=declaration.find(':'); if(colon==std::string::npos) continue;
        auto trim=[](std::string v) { auto a=v.find_first_not_of(" \t\r\n"); auto b=v.find_last_not_of(" \t\r\n"); return a==std::string::npos?std::string():v.substr(a,b-a+1); };
        if(trim(declaration.substr(0,colon))==key) return trim(declaration.substr(colon+1));
      }
      return fallback;
    };
    auto num=[&](const char* key,double fallback=0) { const auto value=attr(key); return value.empty()?fallback:length(value); };
    const std::string full=e.getTagName().GetString(),tag=local(full);
    // Editor metadata and non-rendering resources must never reject a valid drawing.
    if(tag=="defs"||tag=="metadata"||tag=="title"||tag=="desc"||tag=="style"||tag=="namedview"
       || (full.find(':')!=std::string::npos && full.substr(0,full.find(':'))!="svg")) return;
    if(property("display","")=="none"||property("visibility","")=="hidden") return;
    const Mat4 parent=out.transform;
    if(!attr("transform").empty()) out.transform=parent*svg_transform(attr("transform"));
    if(tag=="g"&&!attr("id").empty()) layer=attr("inkscape:label").empty()?attr("id"):attr("inkscape:label");
    if(!attr("clip-path").empty()||!attr("mask").empty()) out.warnings.push_back("SVG clipping/masking retained in source; imported curves are unclipped");
    if(tag=="use") {
      auto href=attr("href"); if(href.empty()) href=attr("xlink:href");
      if(href.size()>1 && href[0]=='#' && ids.count(href.substr(1))) {
        out.transform=out.transform*Mat4::translation(num("x"),-num("y"),0);
        walk(ids.at(href.substr(1)),layer,depth+1);
      } else out.warnings.push_back("Unresolved SVG use reference: "+href);
    } else if(tag=="line") out.line(layer,num("x1"),-num("y1"),num("x2"),-num("y2"));
    else if(tag=="path") { if(!attr("d").empty()) svg_path(out,layer,attr("d")); }
    else if(tag=="circle") { if(num("r")>0) out.circle(layer,num("cx"),-num("cy"),num("r")); }
    else if(tag=="ellipse") {
      double rx=num("rx"),ry=num("ry"); if(rx>0&&ry>0) {
        gp_Ax2 axis(gp_Pnt(num("cx"),-num("cy"),0),gp::DZ(),rx>=ry?gp::DX():gp::DY());
        out.add(layer,BRepBuilderAPI_MakeEdge(gp_Elips(axis,std::max(rx,ry),std::min(rx,ry))).Edge());
      }
    } else if(tag=="rect") {
      const double x=num("x"),y=num("y"),w=num("width"),h=num("height");
      if(w<0||h<0) throw Error("invalid SVG rectangle size");
      if(w>0&&h>0) {
        const double rx=std::clamp(num("rx",num("ry")),0.0,w/2),ry=std::clamp(num("ry",num("rx")),0.0,h/2);
        if(rx>0&&ry>0) {
          std::ostringstream path; path.precision(17);
          path<<"M"<<x+rx<<' '<<y<<" H"<<x+w-rx<<" A"<<rx<<' '<<ry<<" 0 0 1 "<<x+w<<' '<<y+ry
              <<" V"<<y+h-ry<<" A"<<rx<<' '<<ry<<" 0 0 1 "<<x+w-rx<<' '<<y+h
              <<" H"<<x+rx<<" A"<<rx<<' '<<ry<<" 0 0 1 "<<x<<' '<<y+h-ry
              <<" V"<<y+ry<<" A"<<rx<<' '<<ry<<" 0 0 1 "<<x+rx<<' '<<y<<" Z";
          svg_path(out,layer,path.str());
        } else { out.line(layer,x,-y,x+w,-y); out.line(layer,x+w,-y,x+w,-y-h); out.line(layer,x+w,-y-h,x,-y-h); out.line(layer,x,-y-h,x,-y); }
      }
    } else if(tag=="polyline"||tag=="polygon") {
      SvgNumbers values{attr("points")}; std::vector<std::array<double,2>> points;
      while(!values.end()) { double x=values.next(),y=values.next(); points.push_back({x,-y}); }
      for(size_t i=1;i<points.size();++i) out.line(layer,points[i-1][0],points[i-1][1],points[i][0],points[i][1]);
      if(tag=="polygon"&&points.size()>2) out.line(layer,points.back()[0],points.back()[1],points.front()[0],points.front()[1]);
    } else if(tag=="image") {
      const double x=num("x"),y=num("y"),w=num("width"),h=num("height");
      if(w>0&&h>0) {
        auto href=attr("href"); if(href.empty()) href=attr("xlink:href");
        if(!href.empty()&&href.rfind("data:",0)!=0&&href.find("://")==std::string::npos) {
          const auto path=file.parent_path()/std::filesystem::u8path(href);
          if(std::filesystem::exists(path)) {
            const auto ext=extension(path); const auto mime=ext==".jpg"||ext==".jpeg"?"image/jpeg":ext==".png"?"image/png":ext==".bmp"?"image/bmp":"application/octet-stream";
            href="data:"+std::string(mime)+";base64,"+base64(read_text_file(path));
          }
        }
        const std::string imageLayer=layer+" / Image "+std::to_string(out.images.size()+1);
        out.add(imageLayer,BRepBuilderAPI_MakeFace(gp_Pln(gp::XOY()),x,x+w,-y-h,-y).Face());
        json corners=json::array();
        for(auto p:std::array<Vec3,3>{{{x,-y,0},{x+w,-y,0},{x,-y-h,0}}}) {
          Vec3 q{}; for(int r=0;r<3;++r) {q[r]=out.transform.at(r,3);for(int c=0;c<3;++c)q[r]+=out.transform.at(r,c)*p[c];} corners.push_back(q);
        }
        out.images[imageLayer]={{"href",href},{"corners",corners},{"preserveAspectRatio",attr("preserveAspectRatio")}};
        if(href.rfind("data:image/",0)!=0) out.warnings.push_back("Image reference unavailable; its frame and source reference were retained: "+href.substr(0,160));
      }
    } else if(tag=="text") {
      std::string value;
      std::function<void(const LDOM_Node&)> content=[&](const LDOM_Node& node) {
        if(node.getNodeType()==LDOM_Node::TEXT_NODE||node.getNodeType()==LDOM_Node::CDATA_SECTION_NODE) value+=xml_text(node.getNodeValue());
        for(auto child=node.getFirstChild();!child.isNull();child=child.getNextSibling()) content(child);
      };
      content(e);
      if(!value.empty()) {
        StdPrs_BRepFont font;
        const double size=length(property("font-size","16"));
        const auto family=property("font-family","sans-serif");
        if(size>0&&font.FindAndInit(family.c_str(),Font_FA_Regular,size)) {
          const auto align=property("text-anchor","");
          const auto h=align=="middle"?Graphic3d_HTA_CENTER:align=="end"?Graphic3d_HTA_RIGHT:Graphic3d_HTA_LEFT;
          const auto shape=StdPrs_BRepTextBuilder().Perform(font,NCollection_String(value.c_str()),gp_Ax3(gp_Pnt(num("x"),-num("y"),0),gp::DZ()),h,Graphic3d_VTA_BOTTOM);
          out.add(layer,shape);
        } else out.warnings.push_back("SVG text font unavailable; text retained in source");
      }
    } else if(tag!="svg"&&tag!="g"&&tag!="symbol"&&tag!="a"&&tag!="switch") {
      out.warnings.push_back("SVG element retained in source: "+full);
    }
    if(tag!="use"&&tag!="text")
      for(auto child=e.getFirstChild();!child.isNull();child=child.getNextSibling())
        if(child.getNodeType()==LDOM_Node::ELEMENT_NODE) walk(static_cast<const LDOM_Element&>(child),layer,depth+1);
    out.transform=parent;
  };
  walk(root,"0",0);
  double scale=25.4/96.0;
  const auto width=xml_text(root.getAttribute("width")),viewbox=xml_text(root.getAttribute("viewBox"));
  if(!width.empty()&&width.back()!='%'&&!viewbox.empty()) {
    SvgNumbers vb{viewbox}; vb.next();vb.next();const double w=vb.next(),h=vb.next();
    if(w<=0||h<=0||!vb.end()) throw Error("invalid SVG viewBox");
    scale=length(width)*25.4/96/w;
  }
  gp_Trsf scaling; scaling.SetScale(gp_Pnt(0,0,0),scale);
  for(auto& [name,shape]:out.layers) shape=TopoDS::Compound(BRepBuilderAPI_Transform(shape,scaling,true).Shape());
  for(auto& [name,image]:out.images) for(auto& point:image["corners"]) for(auto& v:point) v=v.get<double>()*scale;
  if(out.layers.empty()) throw Error("SVG contains no drawable geometry");
  return out;
}

TopoDS_Shape read_mesh(const std::filesystem::path& file) {
  std::vector<gp_Pnt> vertices; std::vector<std::array<int,3>> triangles;
  const std::string text=read_text_file(file);
  auto add=[&](double x,double y,double z) { if(!std::isfinite(x)||!std::isfinite(y)||!std::isfinite(z)||std::max({std::abs(x),std::abs(y),std::abs(z)})>1e12) throw Error("invalid mesh vertex"); vertices.emplace_back(x,y,z); };
  if(extension(file)==".stl") {
    uint32_t count=0; if(text.size()>=84) { for(int i=0;i<4;++i) count |= uint32_t(static_cast<unsigned char>(text[80+i]))<<(8*i); }
    if(count>0 && count <= (text.size()-std::min<size_t>(84,text.size()))/50 && text.size()==84+size_t(count)*50) {
      for(uint32_t i=0;i<count;++i) {
        for(int j=0;j<3;++j) {
          float v[3]; for(int k=0;k<3;++k) { uint32_t bits=0; size_t at=84+size_t(i)*50+12+j*12+k*4; for(int q=0;q<4;++q) bits|=uint32_t(static_cast<unsigned char>(text[at+q]))<<(8*q); std::memcpy(&v[k],&bits,4); }
          add(v[0],v[1],v[2]);
        }
        const int n=int(vertices.size()); triangles.push_back({n-2,n-1,n});
      }
    } else {
      std::istringstream in(text); std::string word,x,y,z;
      while(in>>word) if(word=="vertex") { if(!(in>>x>>y>>z)) throw Error("truncated STL vertex"); add(number(x),number(y),number(z)); }
      if(vertices.empty() || vertices.size()%3 || text.find("endsolid")==std::string::npos) throw Error("invalid or truncated STL");
      for(int i=1;i<=int(vertices.size());i+=3) triangles.push_back({i,i+1,i+2});
    }
  } else {
    std::istringstream in(text); std::string line;
    while(std::getline(in,line)) {
      std::istringstream ss(line); std::string tag,x,y,z; ss>>tag;
      if(tag=="v") { if(!(ss>>x>>y>>z)) throw Error("invalid OBJ vertex"); add(number(x),number(y),number(z)); }
      else if(tag=="f") {
        std::vector<int> f;
        while(ss>>x) { auto slash=x.find('/'); double raw=number(x.substr(0,slash)); if(raw!=std::floor(raw)||raw==0||std::abs(raw)>vertices.size()) throw Error("OBJ vertex index out of range"); int i=int(raw); f.push_back(i<0?int(vertices.size())+i+1:i); }
        if(f.size()!=3) throw Error("OBJ faces must be triangulated before import");
        triangles.push_back({f[0],f[1],f[2]});
      }
    }
  }
  if(vertices.empty() || triangles.empty() || vertices.size()>10000000 || triangles.size()>10000000) throw Error("empty or oversized mesh");
  Handle(Poly_Triangulation) mesh=new Poly_Triangulation(int(vertices.size()),int(triangles.size()),false);
  for(size_t i=0;i<vertices.size();++i) mesh->SetNode(int(i+1),vertices[i]);
  for(size_t i=0;i<triangles.size();++i) { auto t=triangles[i]; mesh->SetTriangle(int(i+1),Poly_Triangle(t[0],t[1],t[2])); }
  TopoDS_Face face; BRep_Builder().MakeFace(face,mesh); return face;
}
std::string xml(const std::string& in) { std::string out; for(char c:in) { if(c=='&')out+="&amp;"; else if(c=='<')out+="&lt;"; else if(c=='\"')out+="&quot;"; else out+=c; } return out; }
}

ImportResult import_file(Document& doc, const std::filesystem::path& file, const ImportOptions& options) {
  const auto ext=extension(file);
  if(ext==".step" || ext==".stp") return import_step(doc,file,options);
  if(ext==".dwg") {
    Conversion work; auto converted=work.directory/"drawing.dxf";
    convert_dwg(file,converted,false);
    return import_file(doc,converted,options);
  }
  try {
    Drawing drawing; bool mesh=ext==".stl" || ext==".obj";
    if(mesh) drawing.add("Mesh",read_mesh(file));
    else if(ext==".dxf") drawing=read_dxf(file);
    else if(ext==".svg") drawing=read_svg(file);
    else throw Error("unsupported import format: " + ext);
    ImportResult result; result.warnings=drawing.warnings; json children=json::array();
    // Parse fully before touching the document. Stage stores and op so cancellation is atomic.
    Document staged=doc;
    for(const auto& [name, shape]:drawing.layers) {
      if(options.progress && !options.progress(double(children.size())/drawing.layers.size(),"building")) throw Error("cancelled");
      std::string brep;
      if(mesh) { std::ostringstream ss; ss.precision(17); BRepTools::Write(shape,ss,true,false,TopTools_FormatVersion_VERSION_1); brep=ss.str(); }
      else brep=brep_from_shape(shape);
      if(brep.empty() || brep.back()!='\n') brep+='\n';
      const auto key=staged.add_body(brep,{{"representation",mesh?"mesh":"drawing2d"},{"layer",name},{"source",file.filename().string()}});
      cache_shape(staged,key,shape);
      json body={{"type","body"},{"id",new_uuid()},{"name",name},{"key",key},{"representation",mesh?"mesh":"drawing2d"}};
      if(drawing.images.count(name)) body["raster"]=drawing.images.at(name);
      if(!mesh) children.push_back({{"type","component"},{"id",new_uuid()},{"name",name},{"visible",!drawing.visible.count(name)||drawing.visible.at(name)},{"children",json::array({body})}});
      else children.push_back(body);
      ++result.bodies;
    }
    json root={{"type","component"},{"id",new_uuid()},{"name",file.stem().string()},{"children",children}};
    json op={{"op","import"},{"source",file.filename().string()},{"nodes",json::array({root})}};
    if(ext==".svg" && !drawing.warnings.empty()) { op["svg_source"]=read_text_file(file); op["warnings"]=drawing.warnings; }
    if(!options.parent.empty()) op["parent"]=options.parent;
    result.op_id=staged.append(op,options.author).id; result.components=mesh?1:int(children.size())+1;
    result.new_entries=int(staged.body_count()-doc.body_count()); doc=std::move(staged); return result;
  } catch(const Standard_Failure& e) { throw Error(std::string("cannot import geometry: ")+e.GetMessageString()); }
}

ExportResult export_drawing(const Document& doc,const Scene& scene,const std::filesystem::path& file,const ExportOptions& options) {
  if (options.format == "dwg") {
    Conversion work; auto intermediate=work.directory/"drawing.dxf", converted=work.directory/"drawing.dwg";
    ExportOptions dxf=options; dxf.format="dxf";
    auto result=export_drawing(doc,scene,intermediate,dxf);
    convert_dwg(intermediate,converted,true);
    std::filesystem::copy_file(converted,file,std::filesystem::copy_options::overwrite_existing);
    result.files={file}; return result;
  }
  const bool svg=options.format=="svg"; std::ostringstream body; body.precision(17);
  double xmin=0,ymin=0,xmax=1,ymax=1; int bodies=0;
  auto line=[&](const std::string& layer,const gp_Pnt& a,const gp_Pnt& b) {
    for(const auto& p:{a,b}) { xmin=std::min(xmin,p.X()); xmax=std::max(xmax,p.X()); ymin=std::min(ymin,-p.Y()); ymax=std::max(ymax,-p.Y()); }
    if(svg) body<<"<line x1=\""<<a.X()<<"\" y1=\""<<-a.Y()<<"\" x2=\""<<b.X()<<"\" y2=\""<<-b.Y()<<"\"/>\n";
    else body<<"0\nLINE\n8\n"<<layer<<"\n10\n"<<a.X()<<"\n20\n"<<a.Y()<<"\n11\n"<<b.X()<<"\n21\n"<<b.Y()<<'\n';
  };
  for(const auto& id:select_bodies(scene,options.select)) {
    if(!scene.effectively_visible(id)) continue;
    const Node* n=scene.node(id);
    if (n->representation == "mesh") throw Error("mesh reference objects require STL, OBJ or GLB export");
    std::string layer=n->name;
    std::replace(layer.begin(),layer.end(),'\n','_'); std::replace(layer.begin(),layer.end(),'\r','_');
    if(svg) body<<"<g id=\""<<xml(layer)<<"\">\n";
    if(!n->raster.is_null()) {
      if(!svg) throw Error("Raster images require SVG export; DXF raster references are not supported");
      const auto world=scene.world(id);
      std::array<Vec3,3> p;
      for(int i=0;i<3;++i) p[i]=world.apply(n->raster.at("corners").at(i).get<Vec3>());
      for(int i=0;i<4;++i) {
        Vec3 q=i<3?p[i]:Vec3{p[1][0]+p[2][0]-p[0][0],p[1][1]+p[2][1]-p[0][1],0};
        xmin=std::min(xmin,q[0]);xmax=std::max(xmax,q[0]);ymin=std::min(ymin,-q[1]);ymax=std::max(ymax,-q[1]);
      }
      body<<"<image width=\"1\" height=\"1\" preserveAspectRatio=\""<<xml(n->raster.value("preserveAspectRatio",""))
          <<"\" transform=\"matrix("<<p[1][0]-p[0][0]<<' '<<-(p[1][1]-p[0][1])<<' '
          <<p[2][0]-p[0][0]<<' '<<-(p[2][1]-p[0][1])<<' '<<p[0][0]<<' '<<-p[0][1]
          <<")\" href=\""<<xml(n->raster.value("href",""))<<"\"/>\n</g>\n";
      ++bodies; continue;
    }
    auto shape=node_world_shape(doc,scene,id);
    for(TopExp_Explorer it(shape,TopAbs_EDGE);it.More();it.Next()) {
      BRepAdaptor_Curve c(TopoDS::Edge(it.Current()));
      if(c.GetType()==GeomAbs_Circle && std::abs(c.Circle().Axis().Direction().Z())>1-1e-9) {
        const auto circle=c.Circle(); const auto center=circle.Location(); const double r=circle.Radius();
        const bool full=std::abs(c.LastParameter()-c.FirstParameter())>=2*M_PI-1e-8;
        xmin=std::min(xmin,center.X()-r); xmax=std::max(xmax,center.X()+r); ymin=std::min(ymin,-center.Y()-r); ymax=std::max(ymax,-center.Y()+r);
        if(svg && full) { body<<"<circle cx=\""<<center.X()<<"\" cy=\""<<-center.Y()<<"\" r=\""<<r<<"\"/>\n"; continue; }
        if(!svg) {
          body<<"0\n"<<(full?"CIRCLE":"ARC")<<"\n8\n"<<layer<<"\n10\n"<<center.X()<<"\n20\n"<<center.Y()<<"\n40\n"<<r<<'\n';
          if(!full) {
            auto a=c.Value(c.FirstParameter()),b=c.Value(c.LastParameter()); if(circle.Axis().Direction().Z()<0)std::swap(a,b);
            auto angle=[&](const gp_Pnt& p){double value=std::atan2(p.Y()-center.Y(),p.X()-center.X())*180/M_PI;return value<0?value+360:value;};
            body<<"50\n"<<angle(a)<<"\n51\n"<<angle(b)<<'\n';
          }
          continue;
        }
      }
      const int segments=c.GetType()==GeomAbs_Line?1:128;
      for(int i=0;i<segments;++i) line(layer,c.Value(c.FirstParameter()+(c.LastParameter()-c.FirstParameter())*i/segments),c.Value(c.FirstParameter()+(c.LastParameter()-c.FirstParameter())*(i+1)/segments));
    }
    if(svg) body<<"</g>\n";
    ++bodies;
  }
  std::ostringstream out; out.precision(17);
  if(svg) out<<"<svg xmlns=\"http://www.w3.org/2000/svg\" width=\""<<xmax-xmin+2<<"mm\" height=\""<<ymax-ymin+2<<"mm\" viewBox=\""<<xmin-1<<' '<<ymin-1<<' '<<xmax-xmin+2<<' '<<ymax-ymin+2<<"\" fill=\"none\" stroke=\"black\" stroke-width=\"0.2\">\n"<<body.str()<<"</svg>\n";
  else out<<"0\nSECTION\n2\nHEADER\n9\n$INSUNITS\n70\n4\n0\nENDSEC\n0\nSECTION\n2\nENTITIES\n"<<body.str()<<"0\nENDSEC\n0\nEOF\n";
  write_text_file(file,out.str()); return {{file},bodies};
}
}
