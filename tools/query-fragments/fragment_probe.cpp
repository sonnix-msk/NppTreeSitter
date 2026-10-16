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
#include <functional>
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

// Имена стилей запроса (совпадают с TSStyle 14..24).
static const char* QueryStyleName(int id) {
    switch (id) {
        case 3:  return "COMMENT";
        case 14: return "Q_KEYWORD";
        case 15: return "Q_IDENT";
        case 16: return "Q_STRING";
        case 17: return "Q_NUMBER";
        case 18: return "Q_DATE";
        case 19: return "Q_OPER";
        case 20: return "Q_FUNC";
        case 21: return "Q_TYPE";
        case 22: return "Q_CONST";
        case 23: return "Q_PARAM";
        case 24: return "Q_PUNCT";
        default: return "Q?";
    }
}

// Копия MapQueryCapture из плагина (для объективной проверки).
static int MapQueryCapture(const char* capture, const char* nodeType) {
    auto is = [&](const char* n) { return nodeType && std::strcmp(nodeType, n) == 0; };
    if (std::strcmp(capture, "constant.builtin") == 0 && is("parameter")) return 23;
    if (std::strcmp(capture, "comment") == 0) return 3;
    if (std::strncmp(capture, "keyword", 7) == 0) return 14;
    if (std::strncmp(capture, "type", 4) == 0) return 21;
    if (std::strncmp(capture, "constant", 8) == 0) return (is("date") || is("date_time_literal")) ? 18 : 22;
    if (std::strncmp(capture, "variable", 8) == 0) return 15;
    if (std::strncmp(capture, "function", 8) == 0 || std::strncmp(capture, "method", 6) == 0) return 20;
    if (std::strncmp(capture, "operator", 8) == 0) return 19;
    if (std::strncmp(capture, "punctuation", 11) == 0) return 24;
    if (std::strncmp(capture, "number", 6) == 0 || std::strcmp(capture, "float") == 0) return 17;
    if (std::strncmp(capture, "string", 6) == 0 || std::strcmp(capture, "character") == 0) return 16;
    return 15;
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

    // --- Повтор логики плагина: одиночные литералы + склейки через '+' ---

    // Печать подсвеченных токенов SDBL для заданного combined-текста.
    // Прогоняем НАСТОЯЩИЙ highlights-запрос SDBL и мапим капчуры в стили.
    const char* scmPath = (argc >= 6) ? argv[5] : nullptr;
    std::string scmSrc;
    if (scmPath) scmSrc = ReadFile(scmPath);
    TSQuery* sdblQuery = nullptr;
    if (!scmSrc.empty()) {
        uint32_t errOff = 0; TSQueryError err = TSQueryErrorNone;
        sdblQuery = ts_query_new(sdbl, scmSrc.c_str(), (uint32_t)scmSrc.size(), &errOff, &err);
        if (err != TSQueryErrorNone) { std::fprintf(stderr, "scm error at %u type %d\n", errOff, (int)err); sdblQuery = nullptr; }
    } else {
        std::fprintf(stderr, "no scm given - tokens shown by node type\n");
    }

    auto dumpTokens = [&](const std::string& input, const std::vector<std::pair<uint32_t,uint32_t>>& spans) {
        TSParser* sp = ts_parser_new();
        ts_parser_set_language(sp, sdbl);
        TSTree* st = ts_parser_parse_string(sp, nullptr, input.c_str(), (uint32_t)input.size());
        TSNode sroot = ts_tree_root_node(st);
        std::vector<std::pair<uint32_t, std::string>> toks;
        int dropped = 0;
        if (sdblQuery) {
            TSQueryCursor* cur = ts_query_cursor_new();
            ts_query_cursor_exec(cur, sdblQuery, sroot);
            TSQueryMatch m;
            while (ts_query_cursor_next_match(cur, &m)) {
                for (uint16_t i = 0; i < m.capture_count; ++i) {
                    const TSQueryCapture& cap = m.captures[i];
                    uint32_t cs = ts_node_start_byte(cap.node), ce = ts_node_end_byte(cap.node);
                    if (cs >= ce || ce > input.size()) continue;
                    bool inLit = false;
                    for (auto& sp2 : spans) if (cs >= sp2.first && ce <= sp2.second) { inLit = true; break; }
                    if (!inLit) continue;
                    const bool lSlice = cs > 0 && IsWordByte((unsigned char)input[cs]) && IsWordByte((unsigned char)input[cs-1]);
                    const bool rSlice = ce < input.size() && IsWordByte((unsigned char)input[ce-1]) && IsWordByte((unsigned char)input[ce]);
                    if (lSlice || rSlice) { ++dropped; continue; }
                    uint32_t nl = 0; const char* nm = ts_query_capture_name_for_id(sdblQuery, cap.index, &nl);
                    int st = MapQueryCapture(std::string(nm, nl).c_str(), ts_node_type(cap.node));
                    toks.push_back({cs, std::string(QueryStyleName(st)) + "='" + input.substr(cs, ce-cs) + "'"});
                }
            }
            ts_query_cursor_delete(cur);
        } else {
            std::vector<TSNode> stk{sroot};
            while (!stk.empty()) {
                TSNode n = stk.back(); stk.pop_back();
                if (ts_node_child_count(n) == 0 && ts_node_is_named(n)) {
                    uint32_t cs = ts_node_start_byte(n), ce = ts_node_end_byte(n);
                    bool inLit = false;
                    for (auto& sp2 : spans) if (cs >= sp2.first && ce <= sp2.second) { inLit = true; break; }
                    if (cs < ce && ce <= input.size() && inLit) {
                        const bool lw = cs > 0 && IsWordByte((unsigned char)input[cs-1]);
                        const bool rw = ce < input.size() && IsWordByte((unsigned char)input[ce]);
                        if (lw || rw) ++dropped;
                        else toks.push_back({cs, std::string(ts_node_type(n)) + "='" + input.substr(cs, ce-cs) + "'"});
                    }
                }
                uint32_t cc = ts_node_child_count(n);
                for (uint32_t i = 0; i < cc; ++i) stk.push_back(ts_node_child(n, i));
            }
        }
        std::sort(toks.begin(), toks.end());
        std::string line = "         tokens: ";
        for (size_t i = 0; i < toks.size() && i < 60; ++i) line += toks[i].second + " ";
        if (dropped) line += ("  [dropped " + std::to_string(dropped) + " word-slice]");
        std::printf("%s\n", line.c_str());
        ts_tree_delete(st);
        ts_parser_delete(sp);
    };

    auto isPlus = [&](TSNode n) {
        if (std::strcmp(ts_node_type(n), "binary_expression") != 0) return false;
        TSNode op = ts_node_child_by_field_name(n, "operator", 8);
        if (ts_node_is_null(op)) return false;
        uint32_t a = ts_node_start_byte(op), b = ts_node_end_byte(op);
        return b == a + 1 && src[a] == '+';
    };
    auto unwrapExpr = [&](TSNode n) {
        while (std::strcmp(ts_node_type(n), "expression") == 0) {
            TSNode only = {}; int named = 0;
            uint32_t cc = ts_node_child_count(n);
            for (uint32_t i = 0; i < cc; ++i) { TSNode c = ts_node_child(n, i); if (ts_node_is_named(c)) { ++named; only = c; } }
            if (named != 1) break;
            n = only;
        }
        return n;
    };
    auto leaf = [&](TSNode n) {
        while (true) {
            const char* t = ts_node_type(n);
            if (std::strcmp(t, "expression") != 0 && std::strcmp(t, "const_expression") != 0) return n;
            TSNode only = {}; int named = 0;
            uint32_t cc = ts_node_child_count(n);
            for (uint32_t i = 0; i < cc; ++i) { TSNode c = ts_node_child(n, i); if (ts_node_is_named(c)) { ++named; only = c; } }
            if (named != 1) return n;
            n = only;
        }
    };
    std::function<void(TSNode, std::vector<TSNode>&)> flatten = [&](TSNode expr, std::vector<TSNode>& out) {
        TSNode n = unwrapExpr(expr);
        if (isPlus(n)) {
            flatten(ts_node_child_by_field_name(n, "left", 4), out);
            flatten(ts_node_child_by_field_name(n, "right", 5), out);
        } else if (!ts_node_is_null(expr)) {
            out.push_back(expr);
        }
    };

    std::vector<TSNode> stack{root};
    int strings = 0, detected = 0;
    while (!stack.empty()) {
        TSNode node = stack.back(); stack.pop_back();
        const char* type = ts_node_type(node);

        if (ts_node_is_named(node) && std::strcmp(type, "string") == 0) {
            uint32_t s = ts_node_start_byte(node), e = ts_node_end_byte(node);
            std::string input = MakeSdblInput(src.data() + s, e - s);
            qf::ScoreResult res = scorer.Score(input);
            bool pass = res.score >= scorer.Threshold();
            ++strings;
            std::string human = StripWs(input);
            if (human.size() > 70) human = human.substr(0, 70) + "...";
            std::printf("%s score=%.3f  \"%s\"\n", pass ? "[QUERY]" : "[str  ]", res.score, human.c_str());
            if (pass) { ++detected; dumpTokens(input, {{0, (uint32_t)input.size()}}); }
            continue;
        }

        if (isPlus(node)) {
            TSNode parent = ts_node_parent(node);
            if (!ts_node_is_null(parent) && isPlus(parent)) continue;  // под-группа
            std::vector<TSNode> ops; flatten(node, ops);
            uint32_t gs = ts_node_start_byte(node), ge = ts_node_end_byte(node);
            std::vector<std::pair<uint32_t,uint32_t>> spans;
            std::string scoreText;
            bool ok = ops.size() >= 2 && ge <= src.size();
            for (TSNode op : ops) {
                if (!ok) break;
                TSNode lf = leaf(op);
                if (std::strcmp(ts_node_type(lf), "string") != 0) continue;
                uint32_t s = ts_node_start_byte(lf), e = ts_node_end_byte(lf);
                if (e > src.size()) { ok = false; break; }
                spans.push_back({s - gs, e - gs});
                scoreText += MakeSdblInput(src.data() + s, e - s);
                scoreText += ' ';
            }
            if (!ok || spans.empty()) { continue; }
            qf::ScoreResult res = scorer.Score(scoreText);
            bool pass = res.score >= scorer.Threshold();
            ++strings;
            std::string human = StripWs(scoreText);
            if (human.size() > 70) human = human.substr(0, 70) + "...";
            std::printf("%s concat score=%.3f  \"%s\"\n", pass ? "[QUERY]" : "[str  ]", res.score, human.c_str());
            if (!pass) continue;
            ++detected;
            std::string combined(ge - gs, ' ');
            for (auto& sp2 : spans) {
                std::string piece = MakeSdblInput(src.data() + gs + sp2.first, sp2.second - sp2.first);
                combined.replace(sp2.first, sp2.second - sp2.first, piece);
            }
            for (TSNode op : ops) {
                TSNode lf = leaf(op);
                if (std::strcmp(ts_node_type(lf), "string") == 0) continue;
                uint32_t s = ts_node_start_byte(op) - gs;
                if (s < combined.size() && combined[s] == ' ') combined[s] = 'X';
            }
            dumpTokens(combined, spans);
            continue;
        }

        uint32_t cc = ts_node_child_count(node);
        for (uint32_t i = 0; i < cc; ++i) stack.push_back(ts_node_child(node, i));
    }
    std::printf("\nitems=%d detected=%d\n", strings, detected);
    return 0;
}
