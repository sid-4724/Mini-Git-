#include "minigit/diff.hpp"

#include <algorithm>
#include <cstdint>
#include <ostream>
#include <vector>

namespace minigit {
using namespace std;

bool isBinary(const string& bytes) {
    for (unsigned char character : bytes) {
        if ((character < 32 && character != '\n' && character != '\r' && character != '\t') || character == 127) return true;
    }
    return false;
}

static vector<string> lines(const string& bytes) {
    vector<string> result;
    size_t start = 0;
    while (start < bytes.size()) {
        size_t end = bytes.find('\n', start);
        if (end == string::npos) end = bytes.size();
        else ++end;
        result.push_back(bytes.substr(start, end - start));
        start = end;
        if (result.size() > MAX_DIFF_LINES) break;
    }
    return result;
}

static void printLine(ostream& output, char prefix, const string& line) {
    output << prefix << line;
    if (line.empty() || line.back() != '\n') output << "\n\\ No newline at end of file\n";
}

void printTextDiff(ostream& output, const string& before,
                   const string& after, bool existedBefore, bool existsAfter) {
    if (isBinary(before) || isBinary(after)) {
        output << "Binary contents differ; bytes omitted.\n";
        return;
    }
    if (before.size() + after.size() > MAX_DIFF_BYTES) {
        output << "Detailed diff omitted: combined text exceeds 1 MiB.\n";
        return;
    }
    vector<string> oldLines = lines(before);
    vector<string> newLines = lines(after);
    size_t rows = oldLines.size() + 1;
    size_t columns = newLines.size() + 1;
    // Division avoids overflow before the quadratic table is allocated.
    if (oldLines.size() > MAX_DIFF_LINES || newLines.size() > MAX_DIFF_LINES || rows > MAX_DIFF_CELLS / columns) {
        output << "Detailed diff omitted: LCS table exceeds 1000000 cells or 20000 lines.\n";
        return;
    }
    if (before.empty() && after.empty()) {
        if (!existedBefore && existsAfter) output << "[empty file added]\n";
        else if (existedBefore && !existsAfter) output << "[empty file deleted]\n";
        return;
    }
    vector<uint32_t> longestCommonSubsequence(rows * columns, 0);
    size_t oldIndex = oldLines.size();
    while (oldIndex > 0) {
        --oldIndex;
        size_t newIndex = newLines.size();
        while (newIndex > 0) {
            --newIndex;
            size_t cell = oldIndex * columns + newIndex;
            if (oldLines[oldIndex] == newLines[newIndex]) {
                longestCommonSubsequence[cell] = 1 + longestCommonSubsequence[(oldIndex + 1) * columns + newIndex + 1];
            } else {
                longestCommonSubsequence[cell] = max(longestCommonSubsequence[(oldIndex + 1) * columns + newIndex],
                                                         longestCommonSubsequence[cell + 1]);
            }
        }
    }
    oldIndex = 0;
    size_t newIndex = 0;
    while (oldIndex < oldLines.size() || newIndex < newLines.size()) {
        if (oldIndex < oldLines.size() && newIndex < newLines.size() && oldLines[oldIndex] == newLines[newIndex]) {
            printLine(output, ' ', oldLines[oldIndex]);
            ++oldIndex;
            ++newIndex;
        } else if (oldIndex < oldLines.size() && (newIndex == newLines.size() ||
                   longestCommonSubsequence[(oldIndex + 1) * columns + newIndex] >= longestCommonSubsequence[oldIndex * columns + newIndex + 1])) {
            // On ties, print deletion first for a deterministic, easy-to-read diff.
            printLine(output, '-', oldLines[oldIndex++]);
        } else {
            printLine(output, '+', newLines[newIndex++]);
        }
    }
}
}
