// fragment_probe - standalone проверка прохода "фрагменты запросов".
//
// Повторяет логику плагина (ветка sonnix/injections): обход BSL-дерева в поиске
// строковых литералов -> вероятностная оценка -> парсинг SDBL -> список токенов.
// Не требует Notepad++/Scintilla, поэтому служит объективным доказательством.
//
//   fragment_probe <bsl.dll> <sdbl.dll> <lexicon.json> <file.bsl>
//
// Печатает: для каждого литерала — score и вердикт; для прошедших порог —
// подсвеченные SDBL-токены (текст + капчура).

#include "QueryFragmentScorer.h"
#include "tree_sitter/api.h"

#include <windows.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

typedef const TSLanguage* (*LangFn)();

static std::string ReadFile(const char* path) {
    FILE* fp = nullptr;
    if (fopen_s(&fp, path, "rb") != 0 || !fp) return {};
    std::string data;
    char buf[8192];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), fp)) > 0) data.append(buf, n);
    fclose(fp);
    return data;
}

static std::string MakeSdblInput(const char* raw, size_t len) {
    std::string out(raw, len);
    size_t lineStart = 0;
    for (size_t i = 0; i < len; ++i) {
        char c = out[i];
        if (c == '"') out[i] = ' ';
        else if (c == '\n') lineStart = i + 1;
        else if (c == '|') {
            bool onlyWs = true;
            for (size_t j = lineStart; j < i; ++j)
                if (out[j] != ' ' && out[j] != '\t') { onlyWs = false; break; }
            if (onlyWs) out[i] = ' ';
        }
    }
    return out;
}

static std::string StripWs(const std::string& s) {
    std::string t = s;
    for (auto& c : t) if (c == '\n' || c == '\r' || c == '\t') c = ' ';
    size_t a = t.find_first_not_of(' ');
    size_t b = t.find_last_not_of(' ');
    if (a == std::string::npos) return {};
    return t.substr(a, b - a + 1);
}

// Тот же guard, что в плагине: отбрасываем захваты, являющиеся частью слова
// (артефакт грамматики SDBL: ключи без границ слова + identifier prec(-1)).
static bool IsWordByte(unsigned char c) {
    return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') ||
           (c >= 'a' && c <= 'z') || c == '_' || c >= 0x80;
}

int main(int argc, char** argv) {
    if (argc < 5) {
        std::fprintf(stderr, "usage: fragment_probe <bsl.dll> <sdbl.dll> <lexicon.json> <file.bsl>\n");
        return 2;
    }
    HMODULE hBsl = LoadLibraryA(argv[1]);
    HMODULE hSdbl = LoadLibraryA(argv[2]);
    if (!hBsl || !hSdbl) { std::fprintf(stderr, "LoadLibrary failed\n"); return 1; }
    const TSLanguage* bsl = ((LangFn)GetProcAddress(hBsl, "tree_sitter_bsl"))();
    const TSLanguage* sdbl = ((LangFn)GetProcAddress(hSdbl, "tree_sitter_sdbl"))();

    qf::QueryFragmentScorer scorer;
    if (!scorer.LoadFromFile(argv[3])) { std::fprintf(stderr, "lexicon load failed\n"); return 1; }
    std::fprintf(stderr, "threshold=%.2f\n", scorer.Threshold());

    std::string src = ReadFile(argv[4]);
    if (src.empty()) { std::fprintf(stderr, "cannot read %s\n", argv[4]); return 1; }

    TSParser* parser = ts_parser_new();
    ts_parser_set_language(parser, bsl);
    TSTree* tree = ts_parser_parse_string(parser, nullptr, src.c_str(), (uint32_t)src.size());
    TSNode root = ts_tree_root_node(tree);

    // Обход в поиске строковых литералов.
    std::vector<TSNode> stack{root};
    int strings = 0, detected = 0;
    while (!stack.empty()) {
        TSNode node = stack.back();
        stack.pop_back();
        if (ts_node_is_named(node) && std::strcmp(ts_node_type(node), "string") == 0) {
            uint32_t s = ts_node_start_byte(node), e = ts_node_end_byte(node);
            std::string input = MakeSdblInput(src.data() + s, e - s);
            qf::ScoreResult res = scorer.Score(input);
            bool pass = res.score >= scorer.Threshold();
            ++strings;
            std::string human = StripWs(input);
            if (human.size() > 70) human = human.substr(0, 70) + "...";
            std::printf("%s score=%.3f  \"%s\"\n", pass ? "[QUERY]" : "[str  ]", res.score,
                        human.c_str());
            if (!pass) continue;
            ++detected;

            // Парсинг SDBL и список токенов.
            TSParser* sp = ts_parser_new();
            ts_parser_set_language(sp, sdbl);
            TSTree* st = ts_parser_parse_string(sp, nullptr, input.c_str(), (uint32_t)input.size());
            TSNode sroot = ts_tree_root_node(st);
            // Пройдём всё поддерево и покажем named-терминалы с текстом.
            std::vector<TSNode> stk{sroot};
            std::vector<std::pair<uint32_t, std::string>> toks;
            int dropped = 0;
            while (!stk.empty()) {
                TSNode n = stk.back();
                stk.pop_back();
                if (ts_node_child_count(n) == 0 && ts_node_is_named(n)) {
                    uint32_t cs = ts_node_start_byte(n), ce = ts_node_end_byte(n);
                    if (cs < ce && ce <= input.size()) {
                        const bool leftWord = cs > 0 && IsWordByte((unsigned char)input[cs - 1]);
                        const bool rightWord = ce < input.size() && IsWordByte((unsigned char)input[ce]);
                        if (leftWord || rightWord) {
                            ++dropped;  // часть слова — не красим
                        } else {
                            toks.push_back({cs, std::string(ts_node_type(n)) + "='" + input.substr(cs, ce - cs) + "'"});
                        }
                    }
                }
                uint32_t cc = ts_node_child_count(n);
                for (uint32_t i = 0; i < cc; ++i) stk.push_back(ts_node_child(n, i));
            }
            std::sort(toks.begin(), toks.end());
            std::string line = "         tokens: ";
            for (size_t i = 0; i < toks.size() && i < 14; ++i) line += toks[i].second + " ";
            if (dropped) line += ("  [dropped " + std::to_string(dropped) + " word-slice]");
            std::printf("%s\n", line.c_str());
            ts_tree_delete(st);
            ts_parser_delete(sp);
            continue;  // внутрь строк не спускаемся
        }
        uint32_t cc = ts_node_child_count(node);
        for (uint32_t i = 0; i < cc; ++i) stack.push_back(ts_node_child(node, i));
    }
    std::printf("\nstrings=%d detected=%d\n", strings, detected);
    return 0;
}
