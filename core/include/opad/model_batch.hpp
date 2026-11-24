#pragma once
#include "agent.hpp"
#include <functional>
#include <map>
#include <set>

namespace opad::agent {
// The sub-shape references a connection was given (entity_details, query_entities, selection, change lists), by
// normalised ref, with the body key and placement they were given for.
struct KnownRef {std::string geometry,placement;};
using KnownRefs=std::map<std::string,KnownRef>;
using BatchSteps=std::map<std::string,json>;  // step id -> its result, from this connection's earlier batches

// Every face/edge/vertex input needs a current token in `references`, or to be a reference this connection was given
// earlier whose body still has the key and placement it had then. Throws stale_reference / unchecked_reference.
void check_references(const Document& doc,const Scene& scene,const json& args,const KnownRefs* known=nullptr);

// Solid bodies of `after` that are new or whose shape changed since `before` and fail the kernel's validity check, in
// node order. Keys in `valid` are known good and skipped; keys found good are added to it.
std::vector<std::string> invalid_changed_solids(const Document& doc,const Scene& before,const Scene& after,std::set<std::string>* valid=nullptr);

struct BatchOptions {
  const KnownRefs* known=nullptr;
  const BatchSteps* before=nullptr;          // earlier batches' step results, for @{step#/...}
  std::function<bool()> cancelled;
  std::set<std::string>* valid_keys=nullptr;  // body keys every step checked valid, so the caller need not check again
  // Called after each step that succeeded, with the scenes before and after it (the app words it for its activity row).
  std::function<void(const std::string& command,const json& input,const json& result,const Scene& before,const Scene& after)> step_done;
};
// model_batch: a small declarative language over existing typed commands, never executable code. Every step is checked
// the way a single command is (its new and changed solids must be valid), so a later step cannot hide an invalid body
// an earlier one made. On failure `output` keeps the steps so far, the failing one with state "failed" and its error,
// and `failed_step`; then it throws.
void model_batch(Document& doc,const json& args,json& output,const BatchOptions& options={});
}  // namespace opad::agent
