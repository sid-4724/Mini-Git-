#pragma once

#include "minigit/storage.hpp"

namespace minigit {

using CommitGraph = map<string, Commit>;

CommitGraph loadHistory(const ObjectStore& objects, const vector<string>& tips);
CommitGraph validateStateReferences(const ObjectStore& objects, const State& state);
void validateGraph(const CommitGraph& graph);
void validateHistoryExtension(const CommitGraph& graph, const Commit& newCommit);
set<string> ancestors(const CommitGraph& graph, const string& tip);
bool isAncestor(const CommitGraph& graph, const string& ancestorId, const string& tip);
vector<string> reachableOrder(const CommitGraph& graph, const string& tip);
vector<string> bestCommonAncestors(const CommitGraph& graph,
                                           const string& first, const string& second);
}
