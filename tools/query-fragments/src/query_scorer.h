// QueryFragmentScorer — вероятностная оценка "это фрагмент запроса SDBL?".
//
// Порт query_scan.py на C++ (та же формула и признаки). Используется в плагине
// (ветка sonnix/injections) для подсветки фрагментов запроса внутри строк BSL,
// но самодостаточен: зависимостей кроме стандартной библиотеки нет.
//
// Легенда о фрагменте: см. docs/QUERY-FRAGMENTS.md. Ключевое:
//   - детекция вероятностная (фрагмент может быть серединой запроса);
//   - регистр — МОДИФИКАТОР веса, не фильтр;
//   - формула = сильнейший признак + затухающий бонус (НЕ 1-prod(1-w));
//   - лексикон/веса/порог читаются из lexicon.json (правки без пересборки).

#pragma once

#include <string>
#include <vector>

namespace qf {

struct Feature {
    std::string name;
    double weight;
};

struct ScoreResult {
    double score = 0.0;      // итоговая оценка
    double strongest = 0.0;  // сильнейший одиночный признак
    std::vector<Feature> features;  // сработавшие признаки (по убыванию веса)
};

class QueryFragmentScorer {
public:
    // Загрузить из строки JSON (содержимое lexicon.json).
    bool LoadFromJson(const std::string& json);
    // Загрузить из файла. Возвращает false, если файл не прочитан/не распознан.
    bool LoadFromFile(const std::string& path);

    bool IsLoaded() const { return m_loaded; }
    double Threshold() const { return m_threshold; }

    // Оценка текста (UTF-8). Хранит пояснение признаков в ScoreResult.
    ScoreResult Score(const std::string& utf8Text) const;

private:
    struct StrongForm {
        std::string name;
        std::string literal;  // искомое слово (в нижнем регистре, UTF-8)
        double weight = 0.0;
        bool thenParen = false;
    };

    bool m_loaded = false;
    double m_threshold = 0.30;
    double m_caseUpper = 1.0, m_caseTitle = 0.6, m_caseLower = 0.35;
    double m_paramPrefix = 0.40;
    double m_metadataDotted = 0.60;
    double m_dotted = 0.12;
    double m_bonusFactor = 0.5, m_decay = 0.5;
    std::vector<std::pair<std::string, double>> m_keywords;  // (lowercase, weight)
    std::vector<std::string> m_metadataTypes;                 // lowercase
    std::vector<StrongForm> m_strongForms;
};

}  // namespace qf
