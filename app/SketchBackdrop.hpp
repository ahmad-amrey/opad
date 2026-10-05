#pragma once
#include <AIS_InteractiveObject.hxx>
#include <Image_PixMap.hxx>
#include <QByteArray>
#include <QImage>
#include "Jobs.hpp"
#include "opad/scene.hpp"
// Worker-only texture preparation shared by the active editor and saved sketches.
std::vector<Handle(AIS_InteractiveObject)> prepareSketchBackdrops(const opad::json& images,const opad::Frame& frame,Progress progress);
// A picture's bytes decoded (worker threads: 80 ms for a 12 MP JPEG), turned as its EXIF says, no larger than `maxSide`
// on either side (JPEG decodes at the smaller size directly). Null when it cannot be read.
QImage decodePicture(const QByteArray& bytes,int maxSide);
// A decoded picture as a texture: rows as OCCT samples them on a face whose parameters run like the picture's corners
// (top left at v = height), the way canvases and SVG rasters are drawn.
Handle(Image_PixMap) texturePixels(const QImage& image);
