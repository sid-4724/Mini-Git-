#include "minigit/graph.hpp"

#include <unordered_set>

namespace minigit {
using namespace std;

void validateHistoryExtension(const CommitGraph& graph, const Commit& newCommit) {
    if (graph.size() >= MAX_GRAPH_COMMITS) throw Error("History limit reached: at most 20000 reachable commits.");
    size_t bytes = newCommit.message.size() + newCommit.parentCommitIds.size() * 64 + 128;
    for (const pair<const string, Commit>& entry : graph) {
        bytes += entry.second.message.size() + entry.second.parentCommitIds.size() * 64 + 128;
    }
    if (bytes > MAX_HISTORY_BYTES) throw Error("New commit would exceed the 64 MiB history metadata limit.");
}

void validateGraph(const CommitGraph& graph) {
    // 0 = unseen, 1 = on the active DFS path, 2 = completely checked.
    map<string, int> color;
    for (const pair<const string, Commit>& entry : graph) {
        if (color[entry.first] != 0) continue;
        vector<pair<string, size_t>> stack;
        stack.push_back({entry.first, 0});
        color[entry.first] = 1;
        while (!stack.empty()) {
            string currentCommitId = stack.back().first;
            size_t parentIndex = stack.back().second;
            const vector<string>& parentCommitIds = graph.at(currentCommitId).parentCommitIds;
            if (parentIndex == parentCommitIds.size()) {
                color[currentCommitId] = 2;
                stack.pop_back();
                continue;
            }
            ++stack.back().second;
            const string& parentId = parentCommitIds[parentIndex];
            if (graph.count(parentId) == 0) throw Error("Invalid parent reference: missing commit " + parentId);
            if (color[parentId] == 1) throw Error("Cycle detected in corrupted commit history.");
            if (color[parentId] == 0) {
                color[parentId] = 1;
                stack.push_back({parentId, 0});
            }
        }
    }
}

CommitGraph loadHistory(const ObjectStore& objects, const vector<string>& tips) {
    CommitGraph graph;
    vector<string> pending = tips;
    unordered_set<string> validatedSnapshots;
    set<string> validatedBlobIds;
    size_t historyBytes = 0;
    while (!pending.empty()) {
        string currentCommitId = pending.back();
        pending.pop_back();
        if (currentCommitId.empty() || graph.count(currentCommitId) != 0) continue;
        if (graph.size() >= MAX_GRAPH_COMMITS) throw Error("Reachable history exceeds 20000 commits.");
        Commit commit = objects.getCommit(currentCommitId);
        historyBytes += commit.message.size() + commit.parentCommitIds.size() * 64 + 128;
        if (historyBytes > MAX_HISTORY_BYTES) throw Error("Commit metadata exceeds the 64 MiB history memory limit.");
        if (validatedSnapshots.count(commit.snapshotId) == 0) {
            objects.getSnapshot(commit.snapshotId, validatedBlobIds);
            validatedSnapshots.insert(commit.snapshotId);
        }
        graph[currentCommitId] = commit;
        for (const string& parentId : commit.parentCommitIds) pending.push_back(parentId);
    }
    validateGraph(graph);
    return graph;
}

CommitGraph validateStateReferences(const ObjectStore& objects, const State& state) {
    objects.getSnapshot(state.indexId);
    vector<string> tips;
    for (const pair<const string, string>& branch : state.branches) {
        tips.push_back(branch.second);
        if (branch.second.empty() && (branch.first != "main" || state.branches.size() != 1)) throw Error("Invalid unborn branch metadata.");
    }
    if (state.merge.active) {
        tips.push_back(state.merge.ours);
        tips.push_back(state.merge.theirs);
        tips.push_back(state.merge.base);
        Snapshot original = objects.getSnapshot(state.merge.originalIndexId);
        Snapshot owned = objects.getSnapshot(state.merge.ownedSnapshotId);
        Snapshot theirs = objects.getSnapshot(objects.getCommit(state.merge.theirs).snapshotId);
        for (const string& path : state.merge.unresolvedPaths) {
            if (original.count(path) == 0 && owned.count(path) == 0 && theirs.count(path) == 0) throw Error("Invalid conflict path in merge state.");
        }
    }
    CommitGraph graph = loadHistory(objects, tips);
    if (state.merge.active) {
        vector<string> bases = bestCommonAncestors(graph, state.merge.ours, state.merge.theirs);
        if (state.merge.originalIndexId != graph.at(state.merge.ours).snapshotId ||
            bases != vector<string>{state.merge.base} ||
            isAncestor(graph, state.merge.ours, state.merge.theirs) ||
            isAncestor(graph, state.merge.theirs, state.merge.ours)) throw Error("Inconsistent merge base or original index metadata.");
    }
    return graph;
}

set<string> ancestors(const CommitGraph& graph, const string& tip) {
    set<string> result;
    vector<string> stack;
    if (!tip.empty()) stack.push_back(tip);
    while (!stack.empty()) {
        string currentCommitId = stack.back();
        stack.pop_back();
        if (!result.insert(currentCommitId).second) continue;
        CommitGraph::const_iterator commit = graph.find(currentCommitId);
        if (commit == graph.end()) throw Error("Missing commit in ancestor traversal: " + currentCommitId);
        for (const string& parentId : commit->second.parentCommitIds) stack.push_back(parentId);
    }
    return result;
}
bool isAncestor(const CommitGraph& graph, const string& ancestorId, const string& tip) {
    if (ancestorId.empty() || tip.empty()) return false;
    return ancestors(graph, tip).count(ancestorId) != 0;
}

vector<string> reachableOrder(const CommitGraph& graph, const string& tip) {
    vector<string> result;
    vector<string> stack;
    unordered_set<string> visited;
    if (!tip.empty()) stack.push_back(tip);
    while (!stack.empty()) {
        string currentCommitId = stack.back();
        stack.pop_back();
        if (!visited.insert(currentCommitId).second) continue;
        CommitGraph::const_iterator commit = graph.find(currentCommitId);
        if (commit == graph.end()) throw Error("Missing commit in log traversal.");
        result.push_back(currentCommitId);
        const vector<string>& parents = commit->second.parentCommitIds;
        size_t index = parents.size();
        while (index > 0) {
            --index;
            stack.push_back(parents[index]);
        }
    }
    return result;
}

vector<string> bestCommonAncestors(const CommitGraph& graph,
                                           const string& first, const string& second) {
    set<string> firstAncestors = ancestors(graph, first);
    set<string> secondAncestors = ancestors(graph, second);
    set<string> common;
    for (const string& id : firstAncestors) {
        if (secondAncestors.count(id) != 0) common.insert(id);
    }
    // Every strict ancestor of any common node is dominated by that node.
    // A shared multi-source traversal visits each dominated ancestor only once.
    vector<string> pending;
    for (const string& id : common) {
        for (const string& parentId : graph.at(id).parentCommitIds) pending.push_back(parentId);
    }
    unordered_set<string> dominated;
    while (!pending.empty()) {
        string currentCommitId = pending.back();
        pending.pop_back();
        if (!dominated.insert(currentCommitId).second) continue;
        for (const string& parentId : graph.at(currentCommitId).parentCommitIds) pending.push_back(parentId);
    }
    vector<string> best;
    for (const string& id : common) {
        if (dominated.count(id) == 0) best.push_back(id);
    }
    return best;
}
}
