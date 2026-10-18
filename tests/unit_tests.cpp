#include "minigit/diff.hpp"
#include "minigit/graph.hpp"
#include "minigit/storage.hpp"

#include <iostream>
#include <sstream>

using namespace std;
using namespace minigit;

static void require(bool condition, const string& description) {
    if (!condition) throw Error("Test failed: " + description);
}

static void testShaAndCodec() {
    require(sha256("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855", "SHA-256 empty vector");
    require(sha256("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", "SHA-256 abc vector");
    require(sha256(string(1000000, 'a')) == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0", "SHA-256 million-a vector");
    string binary("a\0b\n|:", 6);
    require(unpack(envelope("blob", binary), "blob") == binary, "binary envelope round trip");
    require(sha256(envelope("blob", "x")) != sha256(envelope("snapshot", "x")), "object domain separation");
    Encoder encoder;
    encoder.field("spaces | : =");
    encoder.field("");
    Decoder decoder(encoder.bytes);
    require(decoder.field(100) == "spaces | : =", "length-prefixed separators");
    require(decoder.field(1).empty(), "empty field");
    decoder.finish();
    bool rejected = false;
    try {
        string truncated = encoder.bytes.substr(0, encoder.bytes.size() - 1);
        Decoder invalid(truncated);
        invalid.field(100);
        invalid.field(1);
    } catch (const Error&) { rejected = true; }
    require(rejected, "truncated field rejected");
    rejected = false;
    try { unpack(envelope("commit", ""), "blob"); }
    catch (const Error&) { rejected = true; }
    require(rejected, "wrong type rejected");
    Encoder unsupported;
    unsupported.field("MiniGit");
    unsupported.number(99);
    unsupported.field("blob");
    unsupported.field("");
    rejected = false;
    try { unpack(unsupported.bytes, "blob"); }
    catch (const Error&) { rejected = true; }
    require(rejected, "unsupported version rejected");
    Encoder length;
    length.number(MAX_TRANSACTION_BYTES);
    Decoder atLimit(length.bytes);
    require(atLimit.count(MAX_TRANSACTION_BYTES) == MAX_TRANSACTION_BYTES, "exact codec length bound");
    length.bytes.clear();
    length.number(MAX_TRANSACTION_BYTES + 1);
    Decoder overLimit(length.bytes);
    rejected = false;
    try { overLimit.count(MAX_TRANSACTION_BYTES); }
    catch (const Error&) { rejected = true; }
    require(rejected, "codec length bound rejects next byte before allocation");
    require(quoteShellArgument("") == "''", "empty shell argument");
    require(quoteShellArgument("a'b") == "'a'\"'\"'b'", "literal single quote in shell argument");
}

static void testGraphErrors() {
    CommitGraph graph;
    graph["a"].parentCommitIds = {"b"};
    graph["b"].parentCommitIds = {"a"};
    bool rejected = false;
    try { validateGraph(graph); }
    catch (const Error&) { rejected = true; }
    require(rejected, "cycle detection");
    graph.erase("b");
    rejected = false;
    try { validateGraph(graph); }
    catch (const Error&) { rejected = true; }
    require(rejected, "missing parent detection");
}

static void testMergeBasesAgainstDefinition() {
    // Enumerate every four-node DAG whose edges point to lower-numbered nodes.
    // Floyd-Warshall gives an independent reference for reachability.
    const int nodeCount = 4;
    const int edgeCount = 6;
    for (int mask = 0; mask < (1 << edgeCount); ++mask) {
        CommitGraph graph;
        bool reachable[nodeCount][nodeCount] = {};
        int edge = 0;
        for (int node = 0; node < nodeCount; ++node) {
            string id = to_string(node);
            graph[id] = Commit{};
            reachable[node][node] = true;
            for (int parent = 0; parent < node; ++parent) {
                if ((mask & (1 << edge)) != 0) {
                    graph[id].parentCommitIds.push_back(to_string(parent));
                    reachable[node][parent] = true;
                }
                ++edge;
            }
        }
        validateGraph(graph);
        for (int middle = 0; middle < nodeCount; ++middle) {
            for (int from = 0; from < nodeCount; ++from) {
                for (int to = 0; to < nodeCount; ++to) {
                    reachable[from][to] = reachable[from][to] || (reachable[from][middle] && reachable[middle][to]);
                }
            }
        }
        for (int first = 0; first < nodeCount; ++first) {
            for (int second = 0; second < nodeCount; ++second) {
                vector<string> expected;
                for (int candidate = 0; candidate < nodeCount; ++candidate) {
                    if (!reachable[first][candidate] || !reachable[second][candidate]) continue;
                    bool dominated = false;
                    for (int other = 0; other < nodeCount; ++other) {
                        if (other != candidate && reachable[first][other] && reachable[second][other] && reachable[other][candidate]) dominated = true;
                    }
                    if (!dominated) expected.push_back(to_string(candidate));
                }
                require(bestCommonAncestors(graph, to_string(first), to_string(second)) == expected, "merge bases match the mathematical definition");
            }
        }
    }
}

static void testDiff() {
    ostringstream output;
    printTextDiff(output, "a\nb\na\n", "a\na\nc\n", true, true);
    require(output.str() == " a\n-b\n a\n+c\n", "repeated-line LCS");
    output.str("");
    printTextDiff(output, "", "", false, true);
    require(output.str() == "[empty file added]\n", "empty-file addition");
    output.str("");
    printTextDiff(output, "x", "x\n", true, true);
    require(output.str().find("No newline at end of file") != string::npos, "newline differences visible");
    output.str("");
    printTextDiff(output, string(20001, '\n'), "x\n", true, true);
    require(output.str().find("omitted") != string::npos, "line bound");
    // 999 lines per side gives exactly 1000 * 1000 DP cells.
    string boundary(999, '\n');
    output.str("");
    printTextDiff(output, boundary, boundary, true, true);
    require(output.str().find("omitted") == string::npos, "exact LCS cell limit allowed");
    output.str("");
    printTextDiff(output, boundary + "\n", boundary, true, true);
    require(output.str().find("LCS table exceeds") != string::npos, "next LCS row refused");
    output.str("");
    printTextDiff(output, string(MAX_DIFF_BYTES, 'x'), "", true, false);
    require(output.str().find("omitted") == string::npos, "exact text byte limit allowed");
    output.str("");
    printTextDiff(output, string(MAX_DIFF_BYTES + 1, 'x'), "", true, false);
    require(output.str().find("combined text exceeds") != string::npos, "next text byte refused");
    require(isBinary(string("\0", 1)), "NUL is binary");
    require(!isBinary("नमस्ते\n"), "UTF-8 is displayed as text");
}

int main() {
    try {
        testShaAndCodec();
        testGraphErrors();
        testMergeBasesAgainstDefinition();
        testDiff();
        cout << "SHA-256 vectors, codec checks, graph errors, 1024 DAG merge-base comparisons, and text diff checks passed.\n";
        return 0;
    } catch (const exception& error) {
        cerr << error.what() << '\n';
        return 1;
    }
}
