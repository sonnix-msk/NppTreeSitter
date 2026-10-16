// Standalone-тест C++-порта скоринга: сверить с Python (query_scan.py).
//
//   scorer_test <lexicon.json> <lines.txt>
//
// lines.txt — UTF-8, по одному тестовому тексту на строку (без внешних кавычек BSL).
// Печатает: score<TAB>текст  — по одной строке на вход.

#include "query_scorer.h"

#include <cstdio>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: scorer_test <lexicon.json> <lines.txt>\n");
        return 2;
    }
    qf::QueryFragmentScorer scorer;
    if (!scorer.LoadFromFile(argv[1])) {
        std::fprintf(stderr, "failed to load lexicon: %s\n", argv[1]);
        return 1;
    }
    std::fprintf(stderr, "loaded, threshold=%.2f\n", scorer.Threshold());

    std::ifstream in(argv[2], std::ios::binary);
    if (!in) { std::fprintf(stderr, "cannot open %s\n", argv[2]); return 1; }

    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        qf::ScoreResult r = scorer.Score(line);
        std::printf("%.6f\t%s\n", r.score, line.c_str());
    }
    return 0;
}
