#include "query_scorer.h"

#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <algorithm>
#include <cmath>
#include <functional>

namespace qf {
namespace {

// ===========================================================================
// Минимальный JSON (достаточно для lexicon.json: объекты/массивы/строки/числа).
// ===========================================================================
struct JVal {
    enum Type { Null, Bool, Num, Str, Arr, Obj } type = Null;
    bool b = false;
    double num = 0.0;
    std::string str;
    std::vector<JVal> arr;
    std::vector<std::pair<std::string, JVal>> obj;

    const JVal* Get(const std::string& k) const {
        for (const auto& p : obj)
            if (p.first == k) return &p.second;
        return nullptr;
    }
};

class JsonParser {
public:
    explicit JsonParser(const std::string& s) : m_s(s) {}
    bool Parse(JVal& out) {
        SkipWs();
        if (!ParseValue(out)) return false;
        SkipWs();
        return true;
    }

private:
    const std::string& m_s;
    size_t m_i = 0;

    void SkipWs() {
        while (m_i < m_s.size()) {
            char c = m_s[m_i];
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n') { ++m_i; continue; }
            break;
        }
    }
    bool ParseValue(JVal& v) {
        SkipWs();
        if (m_i >= m_s.size()) return false;
        char c = m_s[m_i];
        if (c == '{') return ParseObject(v);
        if (c == '[') return ParseArray(v);
        if (c == '"') { v.type = JVal::Str; return ParseString(v.str); }
        if (c == 't' || c == 'f') return ParseBool(v);
        if (c == 'n') { m_i += 4; v.type = JVal::Null; return true; }
        return ParseNumber(v);
    }
    bool ParseBool(JVal& v) {
        if (m_s.compare(m_i, 4, "true") == 0) { v.type = JVal::Bool; v.b = true; m_i += 4; return true; }
        if (m_s.compare(m_i, 5, "false") == 0) { v.type = JVal::Bool; v.b = false; m_i += 5; return true; }
        return false;
    }
    bool ParseNumber(JVal& v) {
        size_t start = m_i;
        while (m_i < m_s.size()) {
            char c = m_s[m_i];
            if ((c >= '0' && c <= '9') || c == '-' || c == '+' || c == '.' || c == 'e' || c == 'E') ++m_i;
            else break;
        }
        if (m_i == start) return false;
        v.type = JVal::Num;
        v.num = std::strtod(m_s.substr(start, m_i - start).c_str(), nullptr);
        return true;
    }
    bool ParseString(std::string& out) {
        if (m_i >= m_s.size() || m_s[m_i] != '"') return false;
        ++m_i;
        out.clear();
        while (m_i < m_s.size()) {
            char c = m_s[m_i++];
            if (c == '"') return true;
            if (c == '\\' && m_i < m_s.size()) {
                char e = m_s[m_i++];
                switch (e) {
                    case 'n': out.push_back('\n'); break;
                    case 't': out.push_back('\t'); break;
                    case 'r': out.push_back('\r'); break;
                    case '"': out.push_back('"'); break;
                    case '\\': out.push_back('\\'); break;
                    case '/': out.push_back('/'); break;
                    case 'u': {
                        // \uXXXX -> UTF-8 (BMP)
                        if (m_i + 4 <= m_s.size()) {
                            uint32_t cp = std::strtoul(m_s.substr(m_i, 4).c_str(), nullptr, 16);
                            m_i += 4;
                            EncodeUtf8(out, cp);
                        }
                        break;
                    }
                    default: out.push_back(e);
                }
            } else {
                out.push_back(c);
            }
        }
        return false;
    }
    bool ParseArray(JVal& v) {
        v.type = JVal::Arr;
        ++m_i;  // [
        SkipWs();
        if (m_i < m_s.size() && m_s[m_i] == ']') { ++m_i; return true; }
        while (m_i < m_s.size()) {
            JVal item;
            if (!ParseValue(item)) return false;
            v.arr.push_back(std::move(item));
            SkipWs();
            if (m_i < m_s.size() && m_s[m_i] == ',') { ++m_i; continue; }
            if (m_i < m_s.size() && m_s[m_i] == ']') { ++m_i; return true; }
            return false;
        }
        return false;
    }
    bool ParseObject(JVal& v) {
        v.type = JVal::Obj;
        ++m_i;  // {
        SkipWs();
        if (m_i < m_s.size() && m_s[m_i] == '}') { ++m_i; return true; }
        while (m_i < m_s.size()) {
            SkipWs();
            std::string key;
            if (!ParseString(key)) return false;
            SkipWs();
            if (m_i >= m_s.size() || m_s[m_i] != ':') return false;
            ++m_i;
            JVal val;
            if (!ParseValue(val)) return false;
            v.obj.emplace_back(std::move(key), std::move(val));
            SkipWs();
            if (m_i < m_s.size() && m_s[m_i] == ',') { ++m_i; continue; }
            if (m_i < m_s.size() && m_s[m_i] == '}') { ++m_i; return true; }
            return false;
        }
        return false;
    }
    static void EncodeUtf8(std::string& out, uint32_t cp) {
        if (cp < 0x80) out.push_back(static_cast<char>(cp));
        else if (cp < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }
};

// ===========================================================================
// UTF-8 + регистр (ASCII + кириллица).
// ===========================================================================
uint32_t DecodeUtf8(const std::string& s, size_t i, size_t& len) {
    unsigned char c = static_cast<unsigned char>(s[i]);
    if (c < 0x80) { len = 1; return c; }
    if ((c & 0xE0) == 0xC0 && i + 1 < s.size()) {
        len = 2;
        return ((c & 0x1F) << 6) | (static_cast<unsigned char>(s[i + 1]) & 0x3F);
    }
    if ((c & 0xF0) == 0xE0 && i + 2 < s.size()) {
        len = 3;
        return ((c & 0x0F) << 12) | ((static_cast<unsigned char>(s[i + 1]) & 0x3F) << 6) |
               (static_cast<unsigned char>(s[i + 2]) & 0x3F);
    }
    len = 1;
    return c;
}

bool IsAsciiAlphaNum(uint32_t cp) {
    return (cp >= '0' && cp <= '9') || (cp >= 'A' && cp <= 'Z') ||
           (cp >= 'a' && cp <= 'z') || cp == '_';
}
bool IsCyrillic(uint32_t cp) { return cp >= 0x0400 && cp <= 0x04FF; }
bool IsWordCp(uint32_t cp) { return IsAsciiAlphaNum(cp) || IsCyrillic(cp); }
// Начало слова — буква или '_' (как в Python RE: числа токеном НЕ считаются,
// иначе "1.5" ложно давало бы признак dotted).
bool IsWordStartCp(uint32_t cp) {
    return (cp >= 'A' && cp <= 'Z') || (cp >= 'a' && cp <= 'z') || cp == '_' || IsCyrillic(cp);
}
bool IsAlphaCp(uint32_t cp) {
    return (cp >= 'A' && cp <= 'Z') || (cp >= 'a' && cp <= 'z') || IsCyrillic(cp);
}

bool IsUpperCp(uint32_t cp) {
    if (cp >= 'A' && cp <= 'Z') return true;
    if (cp >= 0x0410 && cp <= 0x042F) return true;  // А..Я
    if (cp == 0x0401) return true;                  // Ё
    return false;
}
bool IsLowerCp(uint32_t cp) {
    if (cp >= 'a' && cp <= 'z') return true;
    if (cp >= 0x0430 && cp <= 0x044F) return true;  // а..я
    if (cp == 0x0451) return true;                  // ё
    return false;
}
uint32_t ToLowerCp(uint32_t cp) {
    if (cp >= 'A' && cp <= 'Z') return cp + 0x20;
    if (cp >= 0x0410 && cp <= 0x042F) return cp + 0x20;
    if (cp == 0x0401) return 0x0451;
    return cp;
}
void EncodeUtf8(std::string& out, uint32_t cp) {
    if (cp < 0x80) out.push_back(static_cast<char>(cp));
    else if (cp < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

std::string ToLowerUtf8(const std::string& s) {
    std::string out;
    size_t i = 0;
    while (i < s.size()) {
        size_t len = 0;
        uint32_t cp = DecodeUtf8(s, i, len);
        EncodeUtf8(out, ToLowerCp(cp));
        i += len;
    }
    return out;
}

struct Token {
    size_t startByte;
    size_t endByte;
    std::string lower;   // UTF-8 в нижнем регистре
    double caseFactor;
};

// Кодпоинт, заканчивающийся на позиции pos (для проверки левой границы слова).
uint32_t PrevCodepoint(const std::string& s, size_t pos) {
    if (pos == 0) return 0;
    size_t back = 1;
    while (back < 4 && pos >= back &&
           (static_cast<unsigned char>(s[pos - back]) & 0xC0) == 0x80) ++back;
    size_t len = 0;
    return DecodeUtf8(s, pos - back, len);
}

double CaseFactorFor(const std::string& rawWord, double up, double title, double low) {
    size_t letters = 0, uppers = 0, lowers = 0;
    bool firstIsUpper = false;
    size_t i = 0;
    bool first = true;
    while (i < rawWord.size()) {
        size_t len = 0;
        uint32_t cp = DecodeUtf8(rawWord, i, len);
        if (IsAlphaCp(cp)) {
            ++letters;
            bool up = IsUpperCp(cp), lo = IsLowerCp(cp);
            if (up) ++uppers;
            if (lo) ++lowers;
            if (first) { firstIsUpper = up; first = false; }
        }
        i += len;
    }
    if (letters < 2) return 0.5;
    if (uppers == letters) return up;                       // ВЫБРАТЬ
    if (firstIsUpper && lowers == letters - 1) return title; // Выбрать
    return low;                                              // выбрать / смешанный
}

std::vector<Token> Tokenize(const std::string& s, double up, double title, double low) {
    std::vector<Token> out;
    size_t i = 0;
    while (i < s.size()) {
        size_t len = 0;
        uint32_t cp = DecodeUtf8(s, i, len);
        if (IsWordStartCp(cp)) {
            size_t start = i;
            while (i < s.size()) {
                size_t l2 = 0;
                uint32_t c2 = DecodeUtf8(s, i, l2);
                if (!IsWordCp(c2)) break;
                i += l2;
            }
            Token t;
            t.startByte = start;
            t.endByte = i;
            std::string raw = s.substr(start, i - start);
            t.lower = ToLowerUtf8(raw);
            t.caseFactor = CaseFactorFor(raw, up, title, low);
            out.push_back(std::move(t));
        } else {
            i += len;
        }
    }
    return out;
}

}  // namespace

// ===========================================================================
// Публичный API
// ===========================================================================
bool QueryFragmentScorer::LoadFromJson(const std::string& json) {
    JVal root;
    JsonParser p(json);
    if (!p.Parse(root) || root.type != JVal::Obj) return false;

    if (const JVal* t = root.Get("threshold")) if (t->type == JVal::Num) m_threshold = t->num;
    if (const JVal* cf = root.Get("case_factors")) {
        if (const JVal* v = cf->Get("upper")) m_caseUpper = v->num;
        if (const JVal* v = cf->Get("title")) m_caseTitle = v->num;
        if (const JVal* v = cf->Get("lower")) m_caseLower = v->num;
    }
    if (const JVal* cb = root.Get("combine")) {
        if (const JVal* v = cb->Get("bonus_factor")) m_bonusFactor = v->num;
        if (const JVal* v = cb->Get("decay")) m_decay = v->num;
    }
    if (const JVal* fw = root.Get("feature_weights")) {
        if (const JVal* v = fw->Get("param_prefix")) m_paramPrefix = v->num;
        if (const JVal* v = fw->Get("metadata_dotted")) m_metadataDotted = v->num;
        if (const JVal* v = fw->Get("dotted")) m_dotted = v->num;
    }
    m_keywords.clear();
    if (const JVal* kw = root.Get("keywords")) {
        for (const auto& kv : kw->obj) {
            if (!kv.first.empty() && kv.first[0] == '_') continue;
            if (kv.second.type == JVal::Num)
                m_keywords.emplace_back(ToLowerUtf8(kv.first), kv.second.num);
        }
    }
    m_metadataTypes.clear();
    if (const JVal* mt = root.Get("metadata_types")) {
        for (const auto& e : mt->arr)
            if (e.type == JVal::Str) m_metadataTypes.push_back(ToLowerUtf8(e.str));
    }
    m_strongForms.clear();
    if (const JVal* sf = root.Get("strong_forms")) {
        for (const auto& e : sf->arr) {
            StrongForm f;
            if (const JVal* v = e.Get("name")) f.name = v->str;
            if (const JVal* v = e.Get("weight")) f.weight = v->num;
            // Наш конфиг содержит одну форму — periods_call (регекс "(?i)\bпериодами\s*\(").
            // Разбираем её явно, без regex: слово + следующий '('.
            if (f.name == "periods_call") { f.literal = "\xD0\xBF\xD0\xB5\xD1\x80\xD0\xB8\xD0\xBE\xD0\xB4\xD0\xB0\xD0\xBC\xD0\xB8"; /* периодами */ f.thenParen = true; }
            if (!f.literal.empty()) m_strongForms.push_back(std::move(f));
        }
    }
    m_loaded = !m_keywords.empty();
    return m_loaded;
}

bool QueryFragmentScorer::LoadFromFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    std::ostringstream ss;
    ss << in.rdbuf();
    std::string data = ss.str();
    if (data.size() >= 3 && static_cast<unsigned char>(data[0]) == 0xEF &&
        static_cast<unsigned char>(data[1]) == 0xBB && static_cast<unsigned char>(data[2]) == 0xBF)
        data.erase(0, 3);
    return LoadFromJson(data);
}

ScoreResult QueryFragmentScorer::Score(const std::string& text) const {
    ScoreResult r;
    if (!m_loaded) return r;

    std::vector<Token> tokens = Tokenize(text, m_caseUpper, m_caseTitle, m_caseLower);

    // 1) ключевые слова: по каждому РАЗЛИЧНОМУ слову — максимум вклада (регистр × лексема).
    std::unordered_map<std::string, double> byWord;
    for (const auto& t : tokens) {
        for (const auto& kv : m_keywords) {
            if (t.lower == kv.first) {
                double c = kv.second * t.caseFactor;
                auto it = byWord.find(t.lower);
                if (it == byWord.end() || c > it->second) byWord[t.lower] = c;
                break;
            }
        }
    }
    {
        std::vector<std::pair<std::string, double>> sortedKw(byWord.begin(), byWord.end());
        std::sort(sortedKw.begin(), sortedKw.end(),
                  [](const auto& a, const auto& b) { return a.second > b.second; });
        for (const auto& kv : sortedKw)
            r.features.push_back({"kw:" + kv.first, kv.second});
    }

    // 2) &Имя — параметр запроса. '&' непосредственно перед словом.
    bool hasParam = false;
    for (const auto& t : tokens) {
        if (t.startByte > 0 && text[t.startByte - 1] == '&') { hasParam = true; break; }
    }
    if (hasParam) r.features.push_back({"param:&", m_paramPrefix});

    // 3) путь через точку; head в наборе метаданных — сильнее.
    bool hasMetadata = false, hasDotted = false;
    std::string metadataHead;
    for (size_t k = 0; k < tokens.size() && !hasMetadata; ++k) {
        const Token& t = tokens[k];
        if (t.endByte < text.size() && text[t.endByte] == '.' && t.endByte + 1 < text.size()) {
            size_t l2 = 0;
            uint32_t cp = DecodeUtf8(text, t.endByte + 1, l2);
            if (IsWordStartCp(cp)) {
                hasDotted = true;
                for (const auto& m : m_metadataTypes) {
                    if (t.lower == m) { hasMetadata = true; metadataHead = t.lower; break; }
                }
            }
        }
    }
    if (hasMetadata) r.features.push_back({"metadata:" + metadataHead, m_metadataDotted});
    else if (hasDotted) r.features.push_back({"dotted", m_dotted});

    // 4) сильные формы (например ПЕРИОДАМИ( ).
    {
        std::string lower = ToLowerUtf8(text);
        for (const auto& f : m_strongForms) {
            size_t pos = 0;
            while ((pos = lower.find(f.literal, pos)) != std::string::npos) {
                const bool leftOk = !IsWordCp(PrevCodepoint(lower, pos));
                size_t after = pos + f.literal.size();
                if (f.thenParen) {
                    while (after < lower.size() && (lower[after] == ' ' || lower[after] == '\t')) ++after;
                    if (leftOk && after < lower.size() && lower[after] == '(') {
                        r.features.push_back({f.name, f.weight});
                        break;
                    }
                } else if (leftOk) {
                    r.features.push_back({f.name, f.weight});
                    break;
                }
                pos += f.literal.size();
            }
        }
    }

    if (r.features.empty()) return r;

    std::vector<double> ws;
    ws.reserve(r.features.size());
    for (const auto& f : r.features) ws.push_back(f.weight);
    std::sort(ws.begin(), ws.end(), std::greater<double>());
    r.strongest = ws[0];
    double bonus = 0.0, mult = 1.0;
    for (size_t i = 1; i < ws.size(); ++i) {
        bonus += ws[i] * mult;
        mult *= m_decay;
    }
    r.score = std::min(1.0, r.strongest + m_bonusFactor * bonus);
    return r;
}

}  // namespace qf
