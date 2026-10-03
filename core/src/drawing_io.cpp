#include "opad/drawing_io.hpp"
#include "opad/drawing/display.hpp"
#include "opad/drawing/sheet.hpp"
#include <functional>
#include <set>
#include "opad/design/sketch_geom.hpp"
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopExp.hxx>
#include "opad/geometry.hpp"
#include "import_common.hpp"
#include "drawing_common.hpp"
#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <gp_Pln.hxx>
#ifdef OPAD_HAVE_FONT
#include <StdPrs_BRepTextBuilder.hxx>
#include <StdPrs_BRepFont.hxx>
#endif
#include <BRepBuilderAPI_MakeVertex.hxx>
#include <BRepBuilderAPI_Transform.hxx>
#include <BRepBndLib.hxx>
#include <Bnd_Box.hxx>
#include <BRepBuilderAPI_GTransform.hxx>
#include <Geom_BezierCurve.hxx>
#include <Geom_BSplineCurve.hxx>
#include <TColStd_Array1OfReal.hxx>
#include <TColStd_Array1OfInteger.hxx>
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
#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

namespace opad {
namespace detail {
void Drawing::add(const std::string& layer, const TopoDS_Shape& s, uint32_t color) {
  if (layers.size() >= 10000 && !layers.count(layer)) throw Error("drawing exceeds 10000 layers");
  auto& c = layers[layer][color]; if (c.IsNull()) builder.MakeCompound(c);
  if(transform.is_identity()) builder.Add(c,s);
  else if(mat_is_rigid(transform)) builder.Add(c,BRepBuilderAPI_Transform(s,trsf_from_mat(transform),true).Shape());
  else {
    gp_GTrsf t; for(int r=0;r<3;++r)for(int col=0;col<4;++col)t.SetValue(r+1,col+1,transform.at(r,col));
    builder.Add(c,BRepBuilderAPI_GTransform(s,t,true).Shape());
  }
}
void Drawing::line(const std::string& layer, double x, double y, double u, double v) {
  if (std::hypot(x-u, y-v) > 1e-9) add(layer, BRepBuilderAPI_MakeEdge(gp_Pnt(x,y,0), gp_Pnt(u,v,0)).Edge());
}
void Drawing::circle(const std::string& layer, double x, double y, double r) {
  if (!(r > 1e-9)) throw Error("drawing circle radius must be positive");
  add(layer, BRepBuilderAPI_MakeEdge(gp_Circ(gp_Ax2(gp_Pnt(x,y,0), gp::DZ()), r)).Edge());
}
}  // namespace detail
using detail::Drawing;
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
// Runs a converter and waits for it (two minutes at most); its exit status, or -1 when it did not start. Arguments go as
// wide strings on Windows, so a drawing named in Arabic reaches the converter intact.
int run_program(const std::filesystem::path& program, const std::vector<std::filesystem::path>& args, const std::filesystem::path& cwd = {}) {
  int status = -1;
#ifdef _WIN32
  std::wstring command;
  auto quote = [&](const std::wstring& a) {
    command += L"\""; unsigned slashes=0;
    for(wchar_t c:a) {
      if(c==L'\\') { ++slashes; continue; }
      command.append(c==L'\"'?slashes*2+1:slashes,L'\\'); slashes=0; command+=c;
    }
    command.append(slashes*2,L'\\'); command+=L"\" ";
  };
  quote(program.wstring());
  for (const auto& a : args) quote(a.wstring());
  STARTUPINFOW startup{}; startup.cb=sizeof(startup); startup.dwFlags=STARTF_USESTDHANDLES|STARTF_USESHOWWINDOW;
  startup.wShowWindow=0;  // SW_HIDE (the OCCT headers leave winuser.h out): converters with a window (ODA) stay out of sight
  // Converters report progress on stdout and stderr, which nobody reads: both go to NUL.
  SECURITY_ATTRIBUTES inherit{sizeof(inherit),nullptr,TRUE};
  HANDLE nul=CreateFileW(L"NUL",GENERIC_READ|GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE,&inherit,OPEN_EXISTING,0,nullptr);
  startup.hStdInput=startup.hStdOutput=startup.hStdError=nul;
  PROCESS_INFORMATION process{};
  if(CreateProcessW(nullptr,command.data(),nullptr,nullptr,TRUE,CREATE_NO_WINDOW,nullptr,cwd.empty()?nullptr:cwd.c_str(),&startup,&process)) {
    if(WaitForSingleObject(process.hProcess,120000)==WAIT_OBJECT_0) { DWORD code; if(GetExitCodeProcess(process.hProcess,&code)) status=int(code); }
    else { TerminateProcess(process.hProcess,1); WaitForSingleObject(process.hProcess,5000); }
    CloseHandle(process.hThread); CloseHandle(process.hProcess);
  }
  if(nul!=INVALID_HANDLE_VALUE) CloseHandle(nul);
#else
  (void)cwd;  // callers pass absolute paths here
  std::vector<std::string> text = {program.string()};
  for (const auto& a : args) text.push_back(a.string());
  std::vector<char*> ptrs; for (auto& a : text) ptrs.push_back(a.data()); ptrs.push_back(nullptr);
  pid_t pid;
  posix_spawn_file_actions_t actions; posix_spawn_file_actions_init(&actions);
  posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, "/dev/null", O_WRONLY, 0);  // converter chatter: nobody reads it
  posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
  const int error = posix_spawnp(&pid, text[0].c_str(), &actions, nullptr, ptrs.data(), environ);
  posix_spawn_file_actions_destroy(&actions);
  if (!error) { int code=0; while(waitpid(pid,&code,0)<0 && errno==EINTR) {} if(WIFEXITED(code)) status=WEXITSTATUS(code); }
#endif
  return status;
}

std::filesystem::path executable_dir() {
#ifdef _WIN32
  std::wstring buffer(32768, L'\0');
  const DWORD n = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
  if (n == 0 || n >= buffer.size()) return {};
  buffer.resize(n);
  return std::filesystem::path(buffer).parent_path();
#else
  std::error_code error;
  const auto self = std::filesystem::read_symlink("/proc/self/exe", error);
  return error ? std::filesystem::path() : self.parent_path();
#endif
}

// The free ODA File Converter, where its installers put it (the newest version when several are installed).
std::filesystem::path oda_converter() {
  std::vector<std::filesystem::path> found;
  std::error_code error;
#ifdef _WIN32
  for (const char* variable : {"ProgramFiles", "ProgramFiles(x86)", "ProgramW6432"}) {
    const char* root = std::getenv(variable);
    if (!root || !*root) continue;
    const auto oda = std::filesystem::path(root) / "ODA";
    if (!std::filesystem::is_directory(oda, error)) continue;
    for (const auto& entry : std::filesystem::directory_iterator(oda, error))
      if (std::filesystem::exists(entry.path() / "ODAFileConverter.exe", error)) found.push_back(entry.path() / "ODAFileConverter.exe");
  }
#else
  for (const char* candidate : {"/usr/bin/ODAFileConverter", "/usr/local/bin/ODAFileConverter", "/opt/ODAFileConverter/ODAFileConverter",
                                "/Applications/ODAFileConverter.app/Contents/MacOS/ODAFileConverter"})
    if (std::filesystem::exists(candidate, error)) found.push_back(candidate);
#endif
  std::sort(found.begin(), found.end());
  return found.empty() ? std::filesystem::path() : found.back();
}

// DWG <-> DXF through an external converter (DWG is a closed format): OPAD_DWG2DXF / OPAD_DXF2DWG when set, else the
// ODA File Converter when installed (it reads every DWG version faithfully), else LibreDWG's dwg2dxf / dxf2dwg, which
// the build puts beside the program (third_party/libredwg), else on PATH.
void convert_dwg(const std::filesystem::path& in, const std::filesystem::path& out, bool toDwg) {
  const std::string name = toDwg ? "dxf2dwg" : "dwg2dxf";
  const char* override = std::getenv(toDwg ? "OPAD_DXF2DWG" : "OPAD_DWG2DXF");
  auto libre = [&](const std::filesystem::path& program) {
    // LibreDWG opens files by their ANSI names on Windows, so it works on plain names in a scratch folder it runs in.
    Conversion work;
    const std::filesystem::path source = toDwg ? "in.dxf" : "in.dwg", target = toDwg ? "out.dwg" : "out.dxf";
    std::filesystem::copy_file(in, work.directory / source);
#ifdef _WIN32
    const bool ok = run_program(program, {"-v0", "-y", "-o", target, source}, work.directory) == 0;
#else
    const bool ok = run_program(program, {"-v0", "-y", "-o", work.directory / target, work.directory / source}) == 0;
#endif
    std::error_code error;
    if (!ok || !std::filesystem::exists(work.directory / target, error)) return false;
    std::filesystem::copy_file(work.directory / target, out, std::filesystem::copy_options::overwrite_existing);
    return true;
  };
  if (override && *override && libre(path_from_utf8(override))) return;
  if (const auto oda = oda_converter(); !oda.empty() && !(override && *override)) {
    // ODA converts folders: the drawing alone in one, the result in another.
    Conversion work;
    const auto from = work.directory / "in", to = work.directory / "out";
    std::filesystem::create_directories(from);
    std::filesystem::create_directories(to);
    std::filesystem::copy_file(in, from / in.filename());
    run_program(oda, {from, to, "ACAD2018", toDwg ? "DWG" : "DXF", "0", "1", in.filename()});
    const auto produced = to / (in.stem().wstring() + (toDwg ? L".dwg" : L".dxf"));
    std::error_code error;
    if (std::filesystem::exists(produced, error)) {
      std::filesystem::copy_file(produced, out, std::filesystem::copy_options::overwrite_existing);
      return;
    }
  }
  bool tried = (override && *override) || !oda_converter().empty();
  if (!(override && *override)) {
    std::error_code error;
#ifdef _WIN32
    const auto beside = executable_dir() / (name + ".exe");
#else
    const auto beside = executable_dir() / name;
#endif
    if (!executable_dir().empty() && std::filesystem::exists(beside, error)) {
      tried = true;
      if (libre(beside)) return;
    }
    if (libre(name)) return;  // on PATH
  }
  if (tried)
    throw Error(std::string(toDwg ? "Writing DWG failed" : "Reading DWG failed") +
                ": the converter could not handle this drawing (it may be damaged, or saved by a newer AutoCAD)");
  throw Error(std::string(toDwg ? "Writing DWG" : "Reading DWG") + " needs a converter: put LibreDWG's " + name +
              " beside OPAD or on PATH (or set " + (toDwg ? "OPAD_DXF2DWG" : "OPAD_DWG2DXF") +
              " to it), or install the free ODA File Converter. Saving the drawing as DXF works without one.");
}
std::string extension(const std::filesystem::path& file) {
  std::string e = file.extension().string();
  std::transform(e.begin(), e.end(), e.begin(), [](unsigned char c) { return std::tolower(c); });
  return e;
}
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
#ifdef OPAD_HAVE_FONT
        StdPrs_BRepFont font;
        const double size=length(property("font-size","16"));
        const auto family=property("font-family","sans-serif");
        if(size>0&&font.FindAndInit(family.c_str(),Font_FA_Regular,size)) {
          const auto align=property("text-anchor","");
          const auto h=align=="middle"?Graphic3d_HTA_CENTER:align=="end"?Graphic3d_HTA_RIGHT:Graphic3d_HTA_LEFT;
          const auto shape=StdPrs_BRepTextBuilder().Perform(font,NCollection_String(value.c_str()),gp_Ax3(gp_Pnt(num("x"),-num("y"),0),gp::DZ()),h,Graphic3d_VTA_BOTTOM);
          out.add(layer,shape);
        } else out.warnings.push_back("SVG text font unavailable; text retained in source");
#else
        out.warnings.push_back("SVG text outlines need OCCT font support; text retained in source");
#endif
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
  for(auto& [name,groups]:out.layers) for(auto& [color,shape]:groups) shape=TopoDS::Compound(BRepBuilderAPI_Transform(shape,scaling,true).Shape());
  for(auto& [name,image]:out.images) for(auto& point:image["corners"]) for(auto& v:point) v=v.get<double>()*scale;
  if(out.layers.empty()) throw Error("SVG contains no drawable geometry");
  return out;
}

}

const std::vector<std::string>& importable_extensions() {
  static const std::vector<std::string> list = {".step", ".stp", ".iges", ".igs", ".brep", ".brp", ".stl", ".obj", ".3mf", ".ply",
                                                ".gltf", ".glb", ".wrl", ".vrml", ".dxf", ".dwg", ".svg"};
  return list;
}

ImportResult import_file(Document& doc, const std::filesystem::path& file, const ImportOptions& options) {
  const auto ext=extension(file);
  if(!std::filesystem::exists(file)) throw Error("file not found: "+file.filename().string());
  try {
    if(ext==".step" || ext==".stp") return import_step(doc,file,options);
    if(ext==".iges" || ext==".igs") return detail::import_iges(doc,file,options);
    if(ext==".brep" || ext==".brp") return detail::import_brep_file(doc,file,options);
    if(ext==".stl") return detail::import_stl(doc,file,options);
    if(ext==".ply") return detail::import_ply(doc,file,options);
    if(ext==".3mf") return detail::import_3mf(doc,file,options);
    if(ext==".obj" || ext==".gltf" || ext==".glb" || ext==".wrl" || ext==".vrml") return detail::import_mesh_scene(doc,file,options);
  } catch(const Standard_Failure& e) { throw Error("cannot read "+file.filename().string()+": "+e.GetMessageString()); }
  if(ext==".dwg") {
    Conversion work; auto name=file.stem(); name+=".dxf";  // keeps the drawing's own name
    convert_dwg(file,work.directory/name,false);
    return import_file(doc,work.directory/name,options);
  }
  try {
    Drawing drawing;
    if(ext==".dxf") drawing=detail::read_dxf(file,options);
    else if(ext==".svg") drawing=read_svg(file);
    else throw Error("unsupported file format: " + ext);
    ImportResult result; result.warnings=drawing.warnings; json children=json::array();
    // Parse fully before touching the document. Stage stores and op so cancellation is atomic.
    Document staged=doc;
    for(const auto& [name, groups]:drawing.layers) {
      if(options.progress && !options.progress(double(children.size())/drawing.layers.size(),"building")) throw Error("cancelled");
      json bodies=json::array();
      for(const auto& [color, shape]:groups) {  // one body per colour the layer's entities are drawn in
        json meta={{"representation","drawing2d"},{"layer",name},{"source",file.filename().string()}};
        json body={{"type","body"},{"id",new_uuid()},{"name",name},{"representation","drawing2d"}};
        if(color!=Drawing::kNoColor) meta["color"]=body["color"]={((color>>16)&255)/255.0,((color>>8)&255)/255.0,(color&255)/255.0};
        body["key"]=detail::store_body(staged,shape,meta,options,false);
        if(bodies.empty() && drawing.images.count(name)) body["raster"]=drawing.images.at(name);
        bodies.push_back(std::move(body));
        ++result.bodies;
      }
      children.push_back({{"type","component"},{"id",new_uuid()},{"name",name},{"visible",!drawing.visible.count(name)||drawing.visible.at(name)},{"children",bodies}});
    }
    json root={{"type","component"},{"id",new_uuid()},{"name",file.stem().string()},{"children",children}};
    Mat4 placement=options.placement;
    if(options.center_drawing) {
      Bnd_Box box;for(const auto& [name,groups]:drawing.layers)for(const auto& [color,shape]:groups)BRepBndLib::Add(shape,box);
      if(!box.IsVoid()){double x0,y0,z0,x1,y1,z1;box.Get(x0,y0,z0,x1,y1,z1);placement=placement*Mat4::translation(-(x0+x1)/2,-(y0+y1)/2,0);}
    } else if(drawing.origin.Modulus()>0) {
      placement=placement*Mat4::translation(drawing.origin.X(),drawing.origin.Y(),drawing.origin.Z());  // read near (0,0), back in place
    }
    if(!placement.is_identity())root["transform"]=placement.to_json();
    json op={{"op","import"},{"source",file.filename().string()},{"nodes",json::array({root})}};
    // The source keeps what the drawing could not show; a viewer never writes it back, so it skips the copy.
    if(ext==".svg" && !drawing.warnings.empty() && !options.viewer) { op["svg_source"]=read_text_file(file); op["warnings"]=drawing.warnings; }
    if(!options.parent.empty()) op["parent"]=options.parent;
    result.op_id=staged.append(op,options.author).id; result.components=int(children.size())+1;
    result.new_entries=int(staged.body_count()-doc.body_count()); doc=std::move(staged); return result;
  } catch(const Standard_Failure& e) { throw Error(std::string("cannot import geometry: ")+e.GetMessageString()); }
}

namespace {
// The selection (or the document) as drawn: solids and meshes as a view, drawings and sketches as they lie.
drawing::Display objects_display(const Document& doc,const Scene& scene,const ExportOptions& options,const std::string& title,json& details,int& bodies) {
  const bool painted=options.format=="pdf" || options.format=="png";
  std::vector<std::string> nodes, sketches;
  for(const auto& id:options.select) { if(scene.sketch(id)) sketches.push_back(id); else nodes.push_back(id); }
  std::vector<std::string> objects;
  if(options.select.empty() || !nodes.empty()) objects=select_bodies(scene,nodes);
  if(options.select.empty()) for(const auto& sk:scene.sketches) if(sk.visible) sketches.push_back(sk.id);
  std::vector<std::string> drawn, modelled;  // as drawn (drawings, sketches, images) / seen in a view (solids, meshes)
  std::set<std::string> seen;
  for(const auto& id:objects) {
    if(!seen.insert(id).second) continue;
    const Node* n=scene.node(id);
    if(n->body_missing || (options.select.empty() && !scene.effectively_visible(id))) continue;
    (n->representation=="drawing2d" || !n->raster.is_null()?drawn:modelled).push_back(id);
  }
  for(const auto& id:sketches) if(seen.insert(id).second) drawn.push_back(id);
  drawing::Display d; d.title=title;
  // Solids and meshes: a hidden-line view; solids without one asked for as seen from the top, in XY with the drawings.
  if(options.view.is_null())
    for(const auto& id:modelled) if(scene.node(id)->representation=="mesh") throw Error("mesh reference objects require STL, OBJ or GLB export, or a 2D view");
  if(!modelled.empty()) {
    json spec=options.view.is_object()?options.view:json{{"view","top"}};
    if(!spec.contains("hidden")) spec["hidden"]=false;
    spec["nodes"]=modelled;
    const auto view=drawing::ViewSpec::from_json(spec);
    const auto g=drawing::project(doc,scene,view,options.progress);
    d=drawing::view_display(*g,title);
    details["view"]={{"dir",view.dir},{"up",view.up},{"hidden",view.hidden},{"tier",drawing::quality_name(g->tier)},{"ms",g->stats.value("ms",0)}};
    bodies+=int(g->bodies.size());
  }
  if(!options.view.is_null()) {
    if(modelled.empty()) throw Error("Nothing to project: a 2D view shows solids and meshes");
    if(!drawn.empty()) details["skipped"]=drawn.size();  // drawings and sketches lie in their own planes, not in the view
    drawn.clear();
  }
  for(const auto& id:drawn) {
    const auto* sketch=scene.sketch(id);
    const Node* n=sketch?nullptr:scene.node(id);
    std::string name=sketch?sketch->name:n->name;
    std::replace(name.begin(),name.end(),'\n','_'); std::replace(name.begin(),name.end(),'\r','_');
    const uint32_t rgb=n && n->has_color?uint32_t(std::lround(std::clamp(n->color[0],0.0,1.0)*255))<<16|uint32_t(std::lround(std::clamp(n->color[1],0.0,1.0)*255))<<8|uint32_t(std::lround(std::clamp(n->color[2],0.0,1.0)*255)):drawing::kInk;
    drawing::Layer pen{name,rgb,drawing::LineType::Continuous,0.25};
    const int layer=d.layer(pen);
    const uint32_t own=d.layers[size_t(layer)].rgb==rgb?drawing::kByLayer:rgb;
    if(n && !n->raster.is_null()) {
      if(options.format!="svg" && !painted) throw Error("Raster images require SVG, PDF or PNG export; DXF raster references are not supported");
      const auto world=scene.world(id);
      drawing::Prim image; image.kind=drawing::Prim::Kind::Image; image.layer=layer;
      for(size_t i=0;i<3;++i) { const auto p=world.apply(n->raster.at("corners").at(i).get<Vec3>()); image.corners[i]={p[0],p[1]}; }
      image.text=n->raster.value("href",""); image.fit=n->raster.value("preserveAspectRatio","");
      d.prims.push_back(std::move(image)); ++bodies; continue;
    }
    if(sketch) {
      // A sketch exports in its own 2D coordinates, independent of its world plane.
      TopoDS_Compound local; BRep_Builder builder; builder.MakeCompound(local);
      for(const auto& edge:design::sketch_edges(design::Sketch::from_json(sketch->geometry),Frame{},true)) builder.Add(local,edge);
      drawing::add_shape(d,layer,local,0.01,own);
    } else drawing::add_shape(d,layer,node_world_shape(doc,scene,id),0.01,own);
    ++bodies;
  }
  if(!bodies) throw Error("No drawing objects selected for export");
  return d;
}
}

ExportResult export_drawing(const Document& doc,const Scene& scene,const std::filesystem::path& file,const ExportOptions& options) {
  const bool painted=options.format=="pdf" || options.format=="png";
  if(options.format!="dxf" && options.format!="svg" && options.format!="dwg" && !painted) throw Error("2D formats are dxf, svg, dwg, pdf and png, not "+options.format);
  if(painted && !drawing::can_paint()) throw Error("PDF and PNG drawings are written by the OPAD app and opad-cli, not by this build");
  const auto stem=doc.path.stem().u8string();
  const std::string title=doc.path.empty()?std::string("OPAD drawing"):std::string(stem.begin(),stem.end());
  std::vector<drawing::Display> pages(1); json details=json::object(); int bodies=0;
  if(options.sheet.empty()) pages[0]=objects_display(doc,scene,options,title,details,bodies);
  else {  // drawing sheets as drawn (UI-86): one by id or name, or every sheet of "drawing:<name>" (a PDF page each)
    std::vector<const Sheet*> sheets;
    if(options.sheet.rfind("drawing:",0)==0) {
      for(const auto& s:scene.sheets) if(s.drawing==options.sheet.substr(8)) sheets.push_back(&s);
      if(sheets.empty()) throw Error("drawing "+options.sheet.substr(8)+" has no sheets (sheet_info lists the sheets)");
    } else {
      const Sheet* sheet=scene.sheet(options.sheet);
      for(const auto& s:scene.sheets) if(!sheet && s.name==options.sheet) sheet=&s;
      if(!sheet) throw Error("sheet "+options.sheet+" does not exist (sheet_info lists the sheets)");
      sheets.push_back(sheet);
    }
    if(sheets.size()>1 && options.format!="pdf") throw Error("several sheets go into one PDF (a page each), or one sheet at a time into "+options.format);
    pages.resize(sheets.size());
    json drawn=json::array(), skipped=json::array();
    for(size_t i=0;i<sheets.size();++i) {
      json report;
      const double n=double(sheets.size());
      pages[i]=drawing::sheet_display(doc,scene,*sheets[i],[&](double f,const std::string& phase){ return !options.progress || options.progress(f<0?-1:(double(i)+f)/n,phase); },&report);
      drawn.push_back({{"id",sheets[i]->id},{"name",sheets[i]->name},{"views",report["views"]},{"items",report["items"]}});
      for(const auto& s:report["skipped"]) skipped.push_back(s);
      bodies+=report["bodies"].get<int>();
    }
    if(drawn.size()==1) details["sheet"]=drawn[0]; else details["sheets"]=drawn;
    if(!skipped.empty()) details["skipped"]=skipped;
  }
  const drawing::Display& d=pages[0];
  if(options.format=="dwg") {  // DXF R2000 through the converter; text of several lines as one TEXT a line
    Conversion work; const auto intermediate=work.directory/"drawing.dxf", converted=work.directory/"drawing.dwg";
    write_text_file(intermediate,drawing::dxf_text(d,options.decimals,false));
    convert_dwg(intermediate,converted,true);
    if(file.has_parent_path()) std::filesystem::create_directories(file.parent_path());
    std::filesystem::copy_file(converted,file,std::filesystem::copy_options::overwrite_existing);
  } else {
    std::vector<const drawing::Display*> list;
    for(const auto& p:pages) list.push_back(&p);
    const json wrote=drawing::write_pages(list,file,options.format,options.decimals,{{"dpi",options.dpi}});
    for(const auto& [k,v]:wrote.items()) details[k]=v;
  }
  ExportResult result{{file},bodies,details};
  // What was written, the pages together.
  std::function<void(json&,const json&)> add=[&](json& to,const json& from){
    for(const auto& [k,v]:from.items()) {
      if(!v.is_object()) to[k]=to.value(k,0)+v.get<int>();
      else { if(!to.contains(k)) to[k]=json::object(); add(to[k],v); }
    }
  };
  json counts=json::object();
  for(const auto& p:pages) add(counts,p.counts());
  for(const auto& [k,v]:counts.items()) result.details[k]=v;
  return result;
}
}
