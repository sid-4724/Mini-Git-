#pragma once

#include <iosfwd>
#include <string>

namespace minigit {
using namespace std;

constexpr size_t MAX_DIFF_BYTES = 1024 * 1024;
constexpr size_t MAX_DIFF_LINES = 20000;
constexpr size_t MAX_DIFF_CELLS = 1000000;

bool isBinary(const string& bytes);
void printTextDiff(ostream& output, const string& before,
                   const string& after, bool existedBefore, bool existsAfter);
}
