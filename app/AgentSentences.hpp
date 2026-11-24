#pragma once
// What an AI assistant did, in the user's words (the AI activity panel, its chip, Undo labels, the timeline's tooltips).
// OPAD writes every sentence itself from what changed: past tense from the scenes before and after a call and its result,
// never from the assistant's arguments alone (those only give the "running" text); the user's own names (bodies, steps,
// sketches) and OPAD's CAD words (the feature labels on the timeline), never a tool name. Text an assistant supplied (a
// transaction's label, a note's text, a file name) is quoted and capped, and never stands alone as a claim.
// Pure functions, safe on a worker: tr() lookups, the scenes they are given and a units::Display captured on the UI thread.
#include <QCoreApplication>
#include <QDateTime>
#include <QString>
#include <QStringList>
#include <string>
#include <vector>

#include "Units.hpp"
#include "opad/core.hpp"

namespace agentnames {
// The client's name as the user knows it, from the clientInfo name an MCP client reports ("claude-code" -> Claude Code /
// Claude; "codex-mcp-client" -> Codex). Empty: "AI assistant". Capped at 40 characters, plain text.
struct Names {
  QString full;   // strips, tooltips, Settings
  QString brief;  // sentences, the chip, Undo labels, toasts
};
Names display(const QString& raw);
}  // namespace agentnames

namespace agentsay {
struct Say {
  Q_DECLARE_TR_FUNCTIONS(AgentSay)
};
// How a call shows in the activity: Hidden never makes a row (polling, schemas); Fold joins the "Looked over the model"
// row; the others are a row of their own. Decided before the call runs (from its name and arguments); a selection or a
// validation that found nothing still folds once done (Sentence::kind says).
enum class Kind { Hidden, Fold, Look, Change, Group, Note, Result, Quiet };
Kind kindOf(const std::string& tool, const opad::json& args);

struct Sentence {
  Kind kind = Kind::Change;
  QString text;    // the row's sentence: "Filleted 4 edges of Bracket, 2 mm"
  QString second;  // the step and the bodies, a value or a status: "Fillet 1 · Bracket"
  QString icon;    // an Icons.cpp name
  std::vector<std::string> subjects;  // bodies (node ids) it is about: hover tint, Select, Show
  std::string op;                     // the step it made (an op id), when one
  bool issue = false;                 // a finding (an overlap, a collision): the row's icon in amber
};

// The running form, from the arguments and the scene as it is ("Adding a fillet to Bracket…"); empty for Hidden and Fold.
QString running(const std::string& tool, const opad::json& args, const opad::Scene& scene, const units::Display& d);
// The done form, from what happened: `before` and `after` the call (the same scene for a read), the tool's result.
Sentence done(const std::string& tool, const opad::json& args, const opad::json& output, const opad::Scene& before,
              const opad::Scene& after, const units::Display& d);
// The steps a model batch made, one sentence each, from the scenes around each step.
Sentence feature(const opad::json& args, const opad::Feature& made, const opad::Scene& before, const opad::Scene& after, const units::Display& d);

// A failed call, for the user: empty when it makes no row (the assistant recovers by itself: a busy moment, a stale
// revision, wrong arguments), else what happened ("Couldn't add a fillet to Bracket: the result wasn't a valid solid.").
// `doing` is the call's running sentence, when it had one.
bool retryable(const std::string& code);
QString failure(const std::string& code, const QString& message, const QString& doing);

// Helpers shared with the panel (and tested).
QString quote(const QString& text, int cap = 60);  // “text”, plain, capped, isolated
QString isolate(const QString& text);               // FSI..PDI: names keep their order in either direction
QString bodies(const opad::Scene& scene, const std::vector<std::string>& ids);  // "Bracket", "Bracket and Lid", "3 parts"
QString lookups(int n);                            // "1 lookup", "2 lookups", "12 lookups"
QString changesCount(int n);                       // "1 change", "2 changes", "4 changes"
QString stepsSoFar(int n);                         // "1 step so far", ...
QString plain(QString text);                       // without the isolation marks (tests, Copy as text)

// Who made a step, from its op's data: "Claude (AI)" for an AI assistant's (by "Agent", its name saved as ai_client; "AI"
// when none was saved), else the name it was saved with. authorLine adds when: "Claude (AI) · 14:02" for an AI step made
// today (the time the activity panel shows), the date too on another day; a person's step as it always read ("by · date").
QString author(const opad::json& opData);
QString authorLine(const opad::json& opData, const QDateTime& now);
}  // namespace agentsay
