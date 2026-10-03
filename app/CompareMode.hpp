#pragma once
// Compare (UI-58): two versions of the open document, A (compared with) and B (shown), in one view. The view draws B: the
// session's bodies tinted by what became of them since A (added, modified, moved; the rest as ghosts), ghosts of what A
// had (a removed body, a moved body's old place, a modified body's old geometry), dashed arrows of the moves, and B's own
// bodies where B is not the session. The panel (ComparePanel, in the "compare" ToolPanel) picks the versions (this
// session, the saved file, git commits, recovery snapshots, another file), weights A against B, hides kinds of change by
// their legend chips, lists the changes (] and [ step through them; a row selects and fits its bodies and shows what it
// changed) and the timeline marks B's new and changed ops. The versions are read, resolved and diffed on a worker
// (opad::semantic_diff, opad::body_changes), the other version's bodies meshed on another; the view only takes a look
// layer (LookSource::Compare) and Viewport::setCompare. Closing the panel (Done, Esc) ends it. `opad.exe --compare a b`
// opens b and compares a (a file, or git:REV) with it.
#include <QObject>
#include <QPointer>
#include <QTimer>
#include <array>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "ComparePanel.hpp"
#include "Viewport.hpp"

class AreaServices;
class GitWatch;
class QLabel;
class ToolPanel;
struct SelectionContext;
namespace opad {
class Document;
}

class CompareMode : public QObject {
  Q_OBJECT
 public:
  CompareMode(AreaServices& services, GitWatch* git);
  ~CompareMode() override;
  bool active() const { return m_active; }
  // Opens it on the open document: A the last commit when git tracks the file, else the file as saved; B this session.
  void open();
  void compare(const CompareVersion& a, const CompareVersion& b);  // opens it too
  void close();
  void step(int delta);  // the next (1) or previous (-1) change, round the list
  void documentChanged(bool replaced);
  void selectionChanged(const SelectionContext& selection);
  ToolPanel* toolPanel() const { return m_tool; }  // null until Compare first opens
  ComparePanel* panel() const { return m_panel; }
  bool settled() const;  // compared, every part drawn, nothing running (benches)
  const std::vector<CompareVersion>& versions() const { return m_versions; }
  // opad.exe --compare <version> <file>: compared with the file once it is open. "git:REV" or a file (made absolute).
  static void setStartup(const QString& version);
  static CompareVersion parseVersion(const QString& spec);
  bool bench(const QString& prefix);  // OPAD_BENCH_COMPARE (CompareBench.cpp)
 signals:
  void activeChanged(bool active);
 private:
  struct Run;
  struct PartInfo {
    int category;  // ComparePanel::Category
    bool sideA;    // a ghost of A (else B's own body)
  };
  void makePanel();
  void release(std::shared_ptr<Run>& run, bool all = true);  // its documents freed on a thread of their own; all: the run too
  void addVersion(const CompareVersion& v);  // once
  int indexOf(const CompareVersion& v) const;
  void showVersions();
  void listVersions();  // git commits and recovery snapshots, on a worker
  void start();         // reads and diffs the chosen pair
  void diff(unsigned serial, std::shared_ptr<opad::Document> session, unsigned long long generation, unsigned long long revision);
  void show(std::shared_ptr<Run> run);
  void buildParts();
  void restyle();  // the look layer, the parts and the arrows for the emphasis and the chips
  void activate(int change);
  void failed(const QString& error);
  opad::json changes() const;   // the diff's, as listed
  std::string relation() const;  // how the histories relate ("" before a result)
  void otherFile(int side);
  AreaServices& m_services;
  GitWatch* m_git;
  ComparePanel* m_panel = nullptr;
  ToolPanel* m_tool = nullptr;
  QLabel* m_chip = nullptr;
  std::vector<CompareVersion> m_versions;
  int m_a = -1, m_b = -1;
  bool m_active = false, m_running = false, m_again = false, m_meshing = false, m_selecting = false;
  unsigned m_serial = 0;
  std::shared_ptr<Run> m_run;
  std::vector<Viewport::ComparePart> m_parts;
  std::vector<PartInfo> m_partInfo;
  std::vector<Viewport::CompareArrow> m_arrows;
  QTimer m_rerun, m_restyle;
  QString m_listed;  // the file whose versions were listed
};
