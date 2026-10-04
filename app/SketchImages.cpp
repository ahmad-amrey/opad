#include "SketchEditor.hpp"
#include "SketchBackdrop.hpp"
#include "Jobs.hpp"
#include "opad/design/sketch_reference.hpp"
#include "opad/design/feature.hpp"
#include "opad/design/sketch_trace.hpp"
#include "opad/design/sketch_modify.hpp"
#include "opad/design/drawing_sketch.hpp"
#include "opad/drawing_io.hpp"
#include "opad/geometry.hpp"
#include "I18n.hpp"
#include "Units.hpp"
#include <AIS_TexturedShape.hxx>
#include <gp_Trsf.hxx>
#include <gp_Vec.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_MakePolygon.hxx>
#include <BRepMesh_IncrementalMesh.hxx>
#include <BRep_Builder.hxx>
#include <Image_PixMap.hxx>
#include <Prs3d_Drawer.hxx>
#include <TopoDS_Compound.hxx>
#include <QBuffer>
#include <QFile>
#include <QImage>
#include <QImageReader>
#include <QPointer>
#include <cmath>
#include <cstring>

using namespace opad::design;
namespace {
QByteArray pictureBytes(const QString& file) {
  QFile f(file);if(!f.open(QIODevice::ReadOnly))throw opad::Error("the image could not be read");return f.readAll();
}
// A JPEG or PNG (which every build decodes) is kept as the file has it: re-encoded as PNG a JPEG grew 4.5 times (UI-71).
// Its size as shown (EXIF turns included), else invalid: the picture is converted.
QSize keptPicture(const QString& file) {
  QImageReader reader(file);const QByteArray format=reader.format();QSize size=reader.size();
  if((format!="jpeg"&&format!="png")||!size.isValid()||size.isEmpty())return {};
  if(reader.transformation()&QImageIOHandler::TransformationRotate90)size.transpose();
  return size;
}
opad::json& backdrop(Sketch& sk,int id) {
  for(auto& image:sk.images)if(image.at("id").get<int>()==id)return image;
  throw opad::Error("choose a backdrop image first");
}
std::pair<double,double> imagePoint(const opad::json& image,double x,double y) {
  const double a=image.value("angle",0.0);return {image.at("position")[0].get<double>()+x*std::cos(a)-y*std::sin(a),image.at("position")[1].get<double>()+x*std::sin(a)+y*std::cos(a)};
}
}
bool SketchEditor::imageClick(double u,double v) {
  if(m_tool!="image_insert"&&m_tool!="image_calibrate")return false;
  const size_t count=m_tool=="image_insert"?1:2;if(m_clicks.size()>=count)m_clicks.clear();m_clicks.push_back({u,v});
  // Calibrate: the two points' distance now, in the Known distance box, to type the real one over (TODO 11 wave 3, P4: as
  // the guide shows, and as Fusion's calibrate does); one typed before the clicks stays.
  if(m_tool=="image_calibrate" && m_clicks.size()==2) {
    const QString measured=units::editable(units::Kind::Length,std::hypot(m_clicks[1].u-m_clicks[0].u,m_clicks[1].v-m_clicks[0].v));
    if(!m_options.contains("knownDistance") || m_options.value("knownDistance")==m_calibrateShown){m_options["knownDistance"]=measured;m_calibrateShown=measured;}
  }
  toolPrompt();rebuild();emit changed();return true;
}
void SketchEditor::imageFrame(int id,double du,double dv,std::vector<std::pair<double,double>>& corners) const {
  corners.clear();
  for(const auto& image:m_sk.images)if(image.at("id").get<int>()==id) {
    const double w=image.at("width").get<double>(),h=image.at("height").get<double>();
    for(const auto& [x,y]:std::vector<std::pair<double,double>>{{0,0},{w,0},{w,h},{0,h}}){const auto at=imagePoint(image,x,y);corners.push_back({at.first+du,at.second+dv});}
  }
}

int SketchEditor::imageAt(double u,double v) const {
  if(m_sk.images.empty())return 0;
  auto inside=[&](const opad::json& image) {
    const double a=image.value("angle",0.0),dx=u-image.at("position")[0].get<double>(),dy=v-image.at("position")[1].get<double>();
    const double x=dx*std::cos(a)+dy*std::sin(a),y=-dx*std::sin(a)+dy*std::cos(a);
    return x>=0 && y>=0 && x<=image.at("width").get<double>() && y<=image.at("height").get<double>();
  };
  const int shown=option("imageId",QString::number(m_sk.images.back().at("id").get<int>())).toInt();
  for(const auto& image:m_sk.images)if(image.at("id").get<int>()==shown && inside(image))return shown;
  for(auto it=m_sk.images.rbegin();it!=m_sk.images.rend();++it)if(inside(*it))return it->at("id").get<int>();  // the one drawn last
  return 0;
}

bool SketchEditor::imagePress(double u,double v) {
  const int id=imageAt(u,v);
  if(!id)return false;
  size_t index=0;while(index<m_sk.images.size() && m_sk.images[index].at("id").get<int>()!=id)++index;
  const auto& image=m_sk.images[index];
  if(option("imageId").toInt()!=id){m_options["imageId"]=QString::number(id);m_panelFieldsDirty=true;emit workflowChanged();}  // the panel shows the one taken
  m_imageDrag=ImageDrag{id,index,u,v,image.at("position")[0].get<double>(),image.at("position")[1].get<double>()};
  return true;
}

void SketchEditor::imageDragTo(double u,double v) {
  if(!m_imageDrag)return;
  ImageDrag& d=*m_imageDrag;
  d.du=u-d.u;d.dv=v-d.v;
  d.moved=d.moved || std::hypot(d.du,d.dv)>0.5*tol();
  if(!d.moved)return;
  // The picture itself follows (its prepared texture moved, nothing built again), when the prepared ones are the images.
  if(m_imagePrs.size()==m_sk.images.size() && d.index<m_imagePrs.size() && !m_imagePrs[d.index].IsNull()) {
    const opad::Vec3 a=m_frame.to_world(0,0),b=m_frame.to_world(d.du,d.dv);
    gp_Trsf move;move.SetTranslation(gp_Vec(b[0]-a[0],b[1]-a[1],b[2]-a[2]));
    m_imagePrs[d.index]->SetLocalTransformation(move);
  }
  updateTransient();  // its frame at the new place, which redraws the view
}

void SketchEditor::imageRelease() {
  if(!m_imageDrag)return;
  const ImageDrag d=*m_imageDrag;
  m_imageDrag.reset();
  if(!d.moved){updateTransient();return;}
  const double x=d.x+d.du,y=d.y+d.dv;
  const int id=d.id;
  if(!m_editJob) {
    m_options["imageX"]=units::editable(units::Kind::Length,x);m_options["imageY"]=units::editable(units::Kind::Length,y);
    runSketchEdit(tr("Transform image"),[id,x,y](Sketch& sk){backdrop(sk,id)["position"]={x,y};});  // its picture prepared again there
  } else if(d.index<m_imagePrs.size() && !m_imagePrs[d.index].IsNull()) {  // the sketch is busy: the picture goes back
    m_imagePrs[d.index]->SetLocalTransformation(gp_Trsf());
    emit status(tr("The sketch is busy; try again"));
  }
  updateTransient();
  emit changed();
}

QSizeF SketchEditor::insertPicture() {
  const QString file=option("imageFile");
  if(file==m_insertFile)return m_insertPicture;
  m_insertFile=file;m_insertPicture={};
  if(file.isEmpty())return m_insertPicture;
  QImageReader reader(file);QSize size=reader.size();  // the header only
  if(!size.isValid()||size.isEmpty())return m_insertPicture;
  if(reader.transformation()&QImageIOHandler::TransformationRotate90)size.transpose();  // as it is placed (keptPicture, decodePicture)
  return m_insertPicture=QSizeF(size);
}
bool SketchEditor::applyImageTool() {
  if(!m_tool.startsWith("image_")&&m_tool!="vector_import"&&m_tool!="vector_export"&&m_tool!="simplify")return false;
  try {
    std::vector<ParamDef> defs;for(const auto& p:m_doc->scene.params)defs.push_back({p.id,p.name,p.expr,p.comment});const ParamTable params(defs,m_doc->scene.units);
    auto length=[&](const char* key,const char* fallback){return params.length(option(key,fallback).toStdString());};
    const int id=option("imageId",m_sk.images.empty()?"0":QString::number(m_sk.images.back().at("id").get<int>())).toInt();
    const QString file=option("imageFile");
    if(m_tool=="image_insert") {
      if(file.isEmpty()||m_clicks.empty())throw opad::Error("choose an image and pick its insertion point");
      const auto at=m_clicks.front();const double width=length("imageWidth","100 mm");if(width<=0)throw opad::Error("image width must be positive");
      runSketchEdit(tr("Loading sketch image"),[file,at,width](Sketch& sk){
        QByteArray bytes=pictureBytes(file);QSize size=keptPicture(file);
        if(!size.isValid()) {  // a format not every build reads: a PNG of it, at most 4096 px
          const QImage decoded=decodePicture(bytes,4096);if(decoded.isNull())throw opad::Error("the image could not be read");
          bytes.clear();QBuffer buffer(&bytes);buffer.open(QIODevice::WriteOnly);if(!decoded.save(&buffer,"PNG"))throw opad::Error("image encoding failed");size=decoded.size();
        }
        sk.images.push_back({{"id",sk.next_id()},{"name",file.toStdString()},{"data",bytes.toBase64().toStdString()},{"position",{at.u,at.v}},{"width",width},{"height",width*size.height()/size.width()},{"angle",0},{"opacity",.5}});
      });
    } else if(m_tool=="image_calibrate") {
      if(m_clicks.size()!=2)throw opad::Error("pick two calibration points");const auto a=m_clicks[0],b=m_clicks[1];const double known=length("knownDistance","10 mm"),distance=std::hypot(b.u-a.u,b.v-a.v);
      if(known<=0||distance<1e-9)throw opad::Error("calibration distances must be positive");const double factor=known/distance;
      if(std::fabs(factor-1)<1e-9){emit status(tr("Type the real distance between the two points, then apply."));return true;}  // the measured one: nothing to scale
      runSketchEdit(tr("Calibrate image"),[id,a,factor](Sketch& sk){auto& image=backdrop(sk,id);image["width"]=image.at("width").get<double>()*factor;image["height"]=image.at("height").get<double>()*factor;image["position"]={a.u+(image.at("position")[0].get<double>()-a.u)*factor,a.v+(image.at("position")[1].get<double>()-a.v)*factor};});
    } else if(m_tool=="image_edit") {
      const double x=length("imageX","0 mm"),y=length("imageY","0 mm"),width=length("imageWidth","100 mm"),angle=params.angle(option("imageAngle","0 deg").toStdString()),opacity=params.number(option("imageOpacity","0.5").toStdString());
      if(width<=0||opacity<0||opacity>1)throw opad::Error("use a positive width and opacity between zero and one");
      runSketchEdit(tr("Transform image"),[id,x,y,width,angle,opacity](Sketch& sk){auto& image=backdrop(sk,id);image["height"]=image.at("height").get<double>()*width/image.at("width").get<double>();image["width"]=width;image["position"]={x,y};image["angle"]=angle;image["opacity"]=opacity;});
    } else if(m_tool=="image_remove") {
      runSketchEdit(tr("Remove image"),[id](Sketch& sk){sk.id_watermark=sk.next_id()-1;for(auto it=sk.images.begin();it!=sk.images.end();)if(it->at("id").get<int>()==id)it=sk.images.erase(it);else ++it;});
    } else if(m_tool=="image_trace") {
      TraceOptions options;options.threshold=params.count(option("threshold","128").toStdString());options.smoothing=params.count(option("smoothing","1").toStdString());options.noise=params.count(option("noise","8").toStdString());options.tolerance=params.number(option("traceTolerance","0.75").toStdString());options.corner_angle=params.number(option("cornerAngle","60").toStdString());options.invert=option("invert","0")=="1";
      runSketchEdit(tr("Tracing image"),[id,options](Sketch& sk){
        const auto imageData=backdrop(sk,id);QImage image=decodePicture(QByteArray::fromBase64(QByteArray::fromStdString(imageData.at("data").get<std::string>())),4096).convertToFormat(QImage::Format_ARGB32);
        if(image.isNull())throw opad::Error("backdrop image could not be decoded");std::vector<unsigned char> grey(size_t(image.width())*image.height());
        for(int y=0;y<image.height();++y)for(int x=0;x<image.width();++x){const auto pixel=image.pixel(x,y);grey[size_t(y)*image.width()+x]=static_cast<unsigned char>((qGray(pixel)*qAlpha(pixel)+255*(255-qAlpha(pixel)))/255);}
        auto traced=trace_bitmap(grey,image.width(),image.height(),options);const double sx=imageData.at("width").get<double>()/image.width(),sy=imageData.at("height").get<double>()/image.height();
        for(auto& p:traced.points){const auto at=imagePoint(imageData,p.x*sx,p.y*sy);p.x=at.first;p.y=at.second;}
        simplify_sketch(traced,std::max(1e-6,options.tolerance*std::min(sx,sy)));append_reference(sk,traced,{},"project",false);
      });
    } else if(m_tool=="simplify") {
      const double tolerance=length("curveTolerance","0.05 mm");runSketchEdit(tr("Simplifying sketch"),[tolerance](Sketch& sk){simplify_sketch(sk,tolerance);});
    } else if(m_tool=="vector_import") {
      const auto path=option("vectorFile");if(path.isEmpty())throw opad::Error("choose an SVG or DXF file");const double tolerance=length("curveTolerance","0.01 mm");
      runSketchEdit(tr("Importing sketch vectors"),[path,tolerance](Sketch& sk){opad::Document doc=opad::Document::create();opad::import_file(doc,std::filesystem::path(path.toStdWString()));const auto scene=opad::resolve(doc);std::vector<DrawingLayer> layers;for(const auto& id:scene.all_bodies())if(const auto* n=scene.node(id);n&&n->raster.is_null())layers.push_back({id,false});append_reference(sk,drawing_sketch(doc,scene,layers,{},tolerance),{},"project",false);});
    } else if(m_tool=="vector_export") {
      const auto path=option("vectorFile");if(path.isEmpty())throw opad::Error("choose an SVG or DXF destination");const auto sketch=std::make_shared<Sketch>(m_sk);
      m_editJob=m_jobs->async(tr("Exporting sketch vectors"),[path,sketch](Progress progress){
        if(progress.cancelled())return;
        opad::Document doc=opad::Document::create();doc.append(make_sketch_op("Sketch",{{"base","xy"},{"frame",opad::Frame{}.to_json()}},sketch->to_json()));opad::ExportOptions options;options.format=path.endsWith(".dxf",Qt::CaseInsensitive)?"dxf":"svg";opad::export_drawing(doc,opad::resolve(doc),std::filesystem::path(path.toStdWString()),options);
      },[this,session=m_session](bool ok,const QString& error){if(!m_active||session!=m_session)return;m_editJob=nullptr;emit status(ok?tr("Sketch exported."):error);});
    }
  }catch(const std::exception& e){emit status(i18n::t(QString::fromUtf8(e.what())));}
  return true;
}

void SketchEditor::refreshImages() {
  if(!m_active)return;
  opad::json stamp=opad::json::array();for(const auto& image:m_sk.images)stamp.push_back({image.at("id"),image.at("position"),image.at("width"),image.at("height"),image.value("angle",0.0),image.value("opacity",.5)});
  const auto key=stamp.dump()+m_frame.to_json().dump();if(key==m_imagesStamp)return;m_imagesStamp=key;
  const int revision=++m_imageRevision;if(m_imageJob)m_imageJob->cancel();
  for(const auto& prs:m_imagePrs)m_viewport->removeOverlay(prs);m_imagePrs.clear();
  if(m_sk.images.empty()){m_imageJob=nullptr;return;}
  auto images=std::make_shared<opad::json>(m_sk.images);auto made=std::make_shared<std::vector<Handle(AIS_InteractiveObject)>>();const auto frame=m_frame;QPointer<SketchEditor> guard(this);
  m_imageJob=m_jobs->async(tr("Preparing image backdrop"),[images,made,frame](Progress progress){
    *made=prepareSketchBackdrops(*images,frame,progress);
  },[this,guard,made,revision](bool ok,const QString& error){if(!guard||!m_active||m_imageRevision!=revision)return;m_imageJob=nullptr;if(!ok){emit status(error);return;}m_imagePrs=*made;if(m_visible)for(const auto& prs:m_imagePrs)m_viewport->showBackdrop(prs);});
}

std::vector<Handle(AIS_InteractiveObject)> prepareSketchBackdrops(const opad::json& images,const opad::Frame& frame,Progress progress) {
  std::vector<Handle(AIS_InteractiveObject)> made;
    for(const auto& data:images){if(progress.cancelled())return made;
      QImage image=decodePicture(QByteArray::fromBase64(QByteArray::fromStdString(data.at("data").get<std::string>())),4096).convertToFormat(QImage::Format_RGBA8888);if(image.isNull())continue;
      const double width=data.at("width").get<double>(),height=data.at("height").get<double>(),angle=data.value("angle",0.0);
      opad::Frame placed=frame;const auto origin=imagePoint(data,0,0);placed.origin=frame.to_world(origin.first,origin.second);
      for(int i=0;i<3;++i){placed.x[i]=frame.x[i]*std::cos(angle)+frame.y[i]*std::sin(angle);placed.y[i]=-frame.x[i]*std::sin(angle)+frame.y[i]*std::cos(angle);}
      auto shape=BRepBuilderAPI_MakeFace(frame_plane(placed),0,width,0,height).Face();BRepMesh_IncrementalMesh mesh(shape,.1);
      Handle(Image_PixMap) pixels=new Image_PixMap();pixels->InitTrash(Image_Format_RGBA,image.width(),image.height());pixels->SetTopDown(false);
      for(int row=0;row<image.height();++row){auto* target=pixels->ChangeRow(row);std::memcpy(target,image.constScanLine(image.height()-1-row),image.width()*4);for(int x=0;x<image.width();++x)target[x*4+3]=static_cast<unsigned char>(target[x*4+3]*data.value("opacity",.5));}
      Handle(AIS_TexturedShape) prs=new AIS_TexturedShape(shape);prs->SetTexturePixMap(pixels);prs->SetTextureMapOn();prs->DisableTextureModulate();prs->SetTextureRepeat(false);prs->SetTransparency(float(1-data.value("opacity",.5)));prs->Attributes()->SetAutoTriangulation(false);made.push_back(prs);
    }
  return made;
}

QImage decodePicture(const QByteArray& bytes,int maxSide) {
  QBuffer buffer;buffer.setData(bytes);buffer.open(QIODevice::ReadOnly);QImageReader reader(&buffer);reader.setAutoTransform(true);
  if(const QSize size=reader.size();size.isValid()&&(size.width()>maxSide||size.height()>maxSide))reader.setScaledSize(size.scaled(maxSide,maxSide,Qt::KeepAspectRatio));
  return reader.read();
}
