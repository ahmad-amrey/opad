#pragma once
// The 3D viewport: OCCT AIS/V3d rendering inside a native Qt widget, driven by AIS_ViewController
// (navigation gestures, hover pre-highlight, click/rubber-band selection, view-cube animation).
#include <AIS_InteractiveContext.hxx>
#include <AIS_Shape.hxx>
#include <AIS_TextLabel.hxx>
#include <AIS_ViewController.hxx>
#include <AIS_ViewCube.hxx>
#include <Graphic3d_ClipPlane.hxx>
#include <V3d_View.hxx>
#include <V3d_Viewer.hxx>

#include <QImage>
#include <QTimer>
#include <QWidget>
#include <array>
#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "AppDocument.hpp"
#include "BodyShape.hpp"
#include "Theme.hpp"

class JobRunner;
class Job;

class Viewport : public QWidget, protected AIS_ViewController {
  Q_OBJECT
 public:
  enum class NavPreset { Fusion, SolidWorks, Onshape, Blender };
  enum class Style { Shaded, ShadedEdges, Wireframe };
  enum class SelFilter { Body, Face, Edge, Vertex };

  explicit Viewport(AppDocument* doc, QWidget* parent = nullptr);
  ~Viewport() override;

  void setTokens(const Tokens& t);
  void setNavPreset(NavPreset p);
  NavPreset navPreset() const { return m_preset; }
  void setStyle(Style s);
  Style style() const { return m_style; }
  void setGrid(bool on);
  void setShadows(bool on);
  void setOrthographic(bool ortho);
  bool isOrthographic() const;
  void setSelectionFilter(SelFilter f);
  SelFilter selectionFilter() const { return m_filter; }

  void fitAll();
  void fitWhenReady();   // fit now if bodies are displayed, otherwise once the first meshes arrive
  void cancelMeshing();  // stop tessellating the remaining bodies (they stay hidden until resetMeshing)
  void resetMeshing();
  int skippedCount() const { return static_cast<int>(m_meshSkipped.size()); }
  void fitSelection();
  void fitNodes(const std::vector<std::string>& ids);
  void standardView(const QString& name);
  void home();
  void rollView(double degrees);  // animated turn about the view axis; positive = counter-clockwise on screen

  void warmUp();  // create the OpenGL viewer now rather than on first paint
  void benchShot(const QString& path);  // --bench-select with OPAD_BENCH_SHOT: hover the view cube, save a frame
  void benchPick();  // --bench-select: pick at the view centre through the context and log what it hit
  void setJobs(JobRunner* jobs);  // long operations (selection, mode switches) run through the app's JobRunner
  std::vector<opad::Ref> selection() const;
  // Highlights the given nodes' bodies as a sliced job; emits selectionApplied() when it has settled. Sets
  // that would take longer than ~0.5 s to highlight are shown as translucent boxes instead.
  void selectNodes(const std::vector<std::string>& ids);
  void clearSelection();  // emits selectionChanged() once the un-highlight has settled
  // Isolate mode: exactly these nodes' bodies are shown, whatever their visibility flags say, until
  // isolate({}) or until none of them exists any more (all deleted). isolationChanged() reports both.
  void isolate(const std::vector<std::string>& ids);  // empty = exit the mode
  bool isIsolated() const { return !m_isolated.empty(); }
  int isolatedCount() const { return static_cast<int>(m_isolated.size()); }

  void setSection(bool enabled, const opad::Vec3& origin, const opad::Vec3& normal, bool caps = true);
  bool sectionEnabled() const { return m_sectionEnabled; }
  void showDimension(const opad::Vec3& a, const opad::Vec3& b, const QString& label);
  void clearDimension();

  opad::json cameraJson() const;
  void setCameraJson(const opad::json& j);
  QImage grabImage();

  QPaintEngine* paintEngine() const override { return nullptr; }

 signals:
  void selectionChanged();
  void selectionApplied();  // a selectNodes() call has been applied (highlight or shade) and selection() reflects it
  void hoverChanged(const QString& text);
  void contextMenuRequested(const QPoint& globalPos);
  void meshingProgress(int remaining);
  void isolationChanged();  // entered, left, or left because every isolated object was deleted

 public slots:
  void sync();
  void requestSync();  // coalesces mesh arrivals: at most one sync per 50 ms

 protected:
  void paintEvent(QPaintEvent*) override;
  void resizeEvent(QResizeEvent*) override;
  void showEvent(QShowEvent*) override;
  void mousePressEvent(QMouseEvent*) override;
  void mouseReleaseEvent(QMouseEvent*) override;
  void mouseMoveEvent(QMouseEvent*) override;
  void wheelEvent(QWheelEvent*) override;
  void OnSelectionChanged(const Handle(AIS_InteractiveContext)& ctx, const Handle(V3d_View)& view) override;

 private:
  struct Item {
    Handle(AIS_Shape) ais;
    std::string key;
    opad::Mat4 world;
    std::array<double, 3> color;
    double opacity;
    TopoDS_Shape located;
  };
  void initViewer();
  void syncWindowSize();
  void applyStyle(const Handle(AIS_Shape)& ais);
  void activateSelection(const Handle(AIS_Shape)& ais);
  void startMeshing(std::vector<std::string> keys);
  void displayBody(const std::string& id);
  void finishSync(int pendingCount, bool added);
  void showShade(const std::vector<std::string>& ids);
  void clearShade();
  double deflectionFor(const std::string& key);
  Graphic3d_Vec2i devicePos(const QPointF& p) const;
  void updateAnnotations();
  void updateClipPlanes();
  void applyTokens();
  void requestRedraw() { update(); }
  // After a change made through the context without an update (Display/Redisplay/selection with
  // theToUpdateViewer=false): the view must be invalidated, or FlushViewEvents finds nothing to redraw and
  // the change only shows on the next orbit.
  void redrawScene() { if (m_initialised) m_view->Invalidate(); requestRedraw(); }

  AppDocument* m_doc;
  Tokens m_tokens;
  Handle(V3d_Viewer) m_viewer;
  Handle(V3d_View) m_view;
  Handle(AIS_InteractiveContext) m_ctx;
  Handle(AIS_ViewCube) m_cube;
  std::map<std::string, Item> m_items;
  std::map<const AIS_InteractiveObject*, std::string> m_nodeOf;
  std::vector<Handle(AIS_InteractiveObject)> m_labels;
  std::vector<Handle(AIS_InteractiveObject)> m_dimension;
  Handle(Graphic3d_ClipPlane) m_sectionPlane;

  NavPreset m_preset = NavPreset::Fusion;
  Style m_style = Style::ShadedEdges;
  SelFilter m_filter = SelFilter::Body;
  bool m_grid = false, m_sectionEnabled = false, m_sectionCaps = true, m_initialised = false, m_needFit = false;
  opad::Vec3 m_sectionOrigin{0, 0, 0}, m_sectionNormal{0, 0, 1};
  std::set<std::string> m_isolated;

  std::mutex m_meshMu;
  const void* m_activeCache = nullptr;  // the document's shape cache the mesh bookkeeping refers to
  std::set<std::string> m_meshed;                                // meshed and presentation built (m_prs)
  std::map<std::string, std::shared_ptr<BodyPrs>> m_prs;         // per key, built on the worker, consumed by displayBody
  std::set<std::string> m_meshing;
  std::set<std::string> m_meshSkipped;
  std::pair<int, int> m_lastSyncedSize{-1, -1};  // device px OCCT was last told about, for syncWindowSize
  JobRunner* m_jobs = nullptr;
  Job* m_displayJob = nullptr;                    // in-flight sync(): bodies being added to the context
  QTimer m_syncTimer;
  Job* m_selJob = nullptr;                        // in-flight selectNodes
  Job* m_filterJob = nullptr;                     // in-flight setSelectionFilter
  std::vector<Handle(AIS_Shape)> m_selApplied;    // objects selectNodes highlighted through the context
  std::vector<Handle(AIS_Shape)> m_shade;         // translucent boxes standing in for a large selection
  std::vector<std::string> m_shadeBodies;         // the bodies those boxes represent (reported by selection())
  bool m_notifyWhenApplied = false;               // clearSelection(): emit selectionChanged once settled
  std::shared_ptr<std::atomic<bool>> m_meshCancel = std::make_shared<std::atomic<bool>>(false);
  std::shared_ptr<std::atomic<bool>> m_alive;

  QTimer m_timer;
  QString m_hover;
  QPoint m_pressPos;
  bool m_rightPress = false;
  bool m_cubeGesture = false;  // this left press started on the view cube: dragging orbits instead of rubber-banding
};
