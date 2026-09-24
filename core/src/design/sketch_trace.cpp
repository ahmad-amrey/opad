#include "opad/design/sketch_trace.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <deque>
#include <set>

namespace opad::design {
Sketch trace_bitmap(const std::vector<unsigned char>& grey,int w,int h,const TraceOptions& opt,const std::function<bool()>& cancel) {
  if(w<1||h<1||w>4096||h>4096||grey.size()!=size_t(w)*h)throw Error("trace image must be between 1 and 4096 pixels on each side");
  if(opt.threshold<0||opt.threshold>255||opt.smoothing<0||opt.smoothing>10||opt.noise<0||opt.tolerance<0||!std::isfinite(opt.tolerance)||opt.corner_angle<0||opt.corner_angle>180)throw Error("invalid tracing parameters");
  auto check=[&]{if(cancel&&cancel())throw Error("cancelled");};
  std::vector<unsigned char> mask(grey.size());
  // Integral image: smoothing is linear in pixel count, independent of radius.
  std::vector<double> sum(size_t(w+1)*(h+1));
  for(int y=0;y<h;++y){check();double row=0;for(int x=0;x<w;++x){row+=grey[size_t(y)*w+x];sum[size_t(y+1)*(w+1)+x+1]=sum[size_t(y)*(w+1)+x+1]+row;}}
  for(int y=0;y<h;++y){check();for(int x=0;x<w;++x){int a=std::max(0,x-opt.smoothing),b=std::min(w,x+opt.smoothing+1),c=std::max(0,y-opt.smoothing),d=std::min(h,y+opt.smoothing+1);double value=(sum[size_t(d)*(w+1)+b]-sum[size_t(c)*(w+1)+b]-sum[size_t(d)*(w+1)+a]+sum[size_t(c)*(w+1)+a])/((b-a)*(d-c));mask[size_t(y)*w+x]=(value<opt.threshold)!=opt.invert;}}
  sum.clear();sum.shrink_to_fit();
  // Remove small foreground components before outlining; holes remain holes.
  std::vector<unsigned char> seen(mask.size());std::vector<int> component;
  const std::array<int,4> dx={1,0,-1,0},dy={0,1,0,-1};
  for(int i=0;i<w*h;++i)if(mask[i]&&!seen[i]){check();component.clear();component.push_back(i);seen[i]=1;for(size_t k=0;k<component.size();++k){int p=component[k],x=p%w,y=p/w;for(int d=0;d<4;++d){int nx=x+dx[d],ny=y+dy[d];if(nx<0||nx>=w||ny<0||ny>=h)continue;int n=ny*w+nx;if(mask[n]&&!seen[n]){seen[n]=1;component.push_back(n);}}}if(component.size()<size_t(opt.noise))for(int p:component)mask[p]=0;}
  // Directed grid edges keep material on the right. At a diagonal contact the
  // rightmost continuation keeps independent contours separate.
  const int stride=w+1;std::vector<unsigned char> edges(size_t(stride)*(h+1));
  auto filled=[&](int x,int y){return x>=0&&x<w&&y>=0&&y<h&&mask[size_t(y)*w+x];};
  for(int y=0;y<h;++y){check();for(int x=0;x<w;++x)if(filled(x,y)){
    if(!filled(x,y-1))edges[size_t(y)*stride+x]|=1;
    if(!filled(x+1,y))edges[size_t(y)*stride+x+1]|=2;
    if(!filled(x,y+1))edges[size_t(y+1)*stride+x+1]|=4;
    if(!filled(x-1,y))edges[size_t(y+1)*stride+x]|=8;
  }}
  Sketch out;
  for(size_t seed=0;seed<edges.size();++seed)while(edges[seed]){
    check();std::vector<std::pair<double,double>> points;int at=int(seed),direction=0;while(!(edges[at]&(1<<direction)))++direction;
    do {
      points.emplace_back(at%stride,h-at/stride);edges[at]&=static_cast<unsigned char>(~(1<<direction));at+=dx[direction]+stride*dy[direction];
      if(at==int(seed))break;int next=-1;for(int turn:{1,0,3,2}){int d=(direction+turn)%4;if(edges[at]&(1<<d)){next=d;break;}}
      if(next<0)throw Error("trace contour is open");direction=next;
    }while(points.size()<=grey.size()*4);
    if(points.size()<3)continue;
    // Keep sharp corners and use Douglas-Peucker between them. Opposite points
    // split closed loops so the initial and final chord cannot collapse a loop.
    std::set<size_t> keep{0,points.size()/2,points.size()};
    if(opt.corner_angle<180)for(size_t i=0;i<points.size();++i){size_t step=std::min<size_t>(3,std::max<size_t>(1,points.size()/8));auto a=points[(i+points.size()-step)%points.size()],b=points[i],c=points[(i+step)%points.size()];double ax=b.first-a.first,ay=b.second-a.second,bx=c.first-b.first,by=c.second-b.second;double angle=std::atan2(std::fabs(ax*by-ay*bx),ax*bx+ay*by)*180/3.14159265358979323846;if(angle>=opt.corner_angle)keep.insert(i);}
    points.push_back(points.front());std::vector<std::pair<size_t,size_t>> work;for(auto i=keep.begin(),j=std::next(i);j!=keep.end();++i,++j)work.push_back({*i,*j});
    while(!work.empty()){auto [a,b]=work.back();work.pop_back();const auto p=points[a],q=points[b];double dx=q.first-p.first,dy=q.second-p.second,den=dx*dx+dy*dy,max=opt.tolerance*opt.tolerance;size_t split=0;for(size_t i=a+1;i<b;++i){double t=den?std::clamp(((points[i].first-p.first)*dx+(points[i].second-p.second)*dy)/den,0.0,1.0):0;double ex=points[i].first-p.first-t*dx,ey=points[i].second-p.second-t*dy,d=ex*ex+ey*ey;if(d>max){max=d;split=i;}}if(split){keep.insert(split);work.push_back({a,split});work.push_back({split,b});}}
    std::vector<int> ids;for(size_t i:keep)if(i+1<points.size())ids.push_back(out.add_point(points[i].first,points[i].second));
    if(ids.size()<3)continue;for(size_t i=0;i<ids.size();++i)out.add_line(ids[i],ids[(i+1)%ids.size()]);
    if(out.entities.size()>100000)throw Error("trace exceeds 100,000 segments; increase smoothing, noise removal or tolerance");
  }
  if(out.entities.empty())throw Error("no outlines at this threshold");out.validate();return out;
}
}
