// ============================================================
// suggest.h — 顶层未声明/未定义标识符的"did you mean"建议
//
// 供解释器（src/interpreter.cpp）与编译器（src/codegen/CodeGen.cpp）
// 共用，给出拼写相近的候选（编辑距离阈值），例如:
//   error: 't' was not declared in this scope; did you mean 't1'?
// ============================================================
#ifndef VORTEX_SUGGEST_H
#define VORTEX_SUGGEST_H

#include <algorithm>
#include <string>
#include <vector>

namespace vortex {
namespace suggest {

// 小字符串 Levenshtein 距离
inline int edit_distance(const std::string& a, const std::string& b) {
    const size_t n = a.size(), m = b.size();
    if (n == 0) return (int)m;
    if (m == 0) return (int)n;
    std::vector<int> prev(m + 1), cur(m + 1);
    for (size_t j = 0; j <= m; ++j) prev[j] = (int)j;
    for (size_t i = 1; i <= n; ++i) {
        cur[0] = (int)i;
        for (size_t j = 1; j <= m; ++j) {
            int cost = (a[i - 1] == b[j - 1]) ? 0 : 1;
            cur[j] = std::min({prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + cost});
        }
        std::swap(prev, cur);
    }
    return prev[m];
}

// 从候选里挑一个最相近的；若差距可接受则返回" ; did you mean 'x'?"，否则返回空串。
inline std::string did_you_mean(const std::string& name, const std::vector<std::string>& candidates) {
    if (name.empty()) return "";
    // 阈值：短名≤2，长名放宽到 max(2, len/3)
    const int thr = std::max(2, (int)name.size() / 3);
    int best = thr + 1;
    std::string best_cand;
    for (const auto& c : candidates) {
        if (c.empty() || c == name) continue;
        int d = edit_distance(name, c);
        if (d == 0) continue;
        if (d < best || (d == best && c.size() < best_cand.size())) {
            best = d;
            best_cand = c;
        }
    }
    if (best_cand.empty()) return "";
    return "; did you mean '" + best_cand + "'?";
}

} // namespace suggest
} // namespace vortex

#endif // VORTEX_SUGGEST_H