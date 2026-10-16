#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Харнесс вероятностной детекции ФРАГМЕНТОВ запроса SDBL в строковых литералах BSL.

Роль: ДИАГНОСТИКА и ТЮНИНГ. Детектор для плагина будет на C++, но веса/порог
подбираются здесь на реальном корпусе. Лексикон и параметры берутся из
lexicon.json (единый источник истины, будет использован и в C++).

Что делает:
  1. Сканирует .bsl, вырезает строковые литералы (учёт "" и многострочного '|').
  2. Объединяет литералы, склеенные через '+', в одну "группу" (конкатенация).
  3. Считает оценку "похоже на фрагмент запроса" по признакам.
  4. Пишет отчёт: распределение по полосам, топ-примеры, спорные, ловушки.

Запуск:
  python query_scan.py <папка_или_файл> [--report out.txt] [--threshold 0.30]
"""

import re
import sys
import os
import json
import argparse
from collections import Counter

HERE = os.path.dirname(os.path.abspath(__file__))
CFG = json.load(open(os.path.join(HERE, 'lexicon.json'), encoding='utf-8'))

KW = {k: v for k, v in CFG['keywords'].items() if not k.startswith('_')}
METADATA_TYPES = set(CFG['metadata_types'])
CASE_FACTORS = CFG['case_factors']
STRONG_FORMS = [(f['name'], re.compile(f['regex']), f['weight']) for f in CFG['strong_forms']]
FEAT = CFG['feature_weights']
COMBINE = CFG['combine']

RE_WORD = re.compile(r'[A-Za-zА-Яа-яЁё_][A-Za-zА-Яа-яЁё0-9_]*')
RE_PARAM = re.compile(r'&[A-Za-zА-Яа-яЁё_][A-Za-zА-Яа-яЁё0-9_]*')
RE_DOTTED = re.compile(r'[A-Za-zА-Яа-яЁё_][A-Za-zА-Яа-яЁё0-9_]*\.[A-Za-zА-Яа-яЁё_][A-Za-zА-Яа-яЁё0-9_]*')


def case_factor(word):
    letters = [c for c in word if c.isalpha()]
    if len(letters) < 2:
        return 0.5
    core = ''.join(letters)
    if core == core.upper():
        return CASE_FACTORS['upper']
    if letters[0].isupper() and all(c.islower() for c in letters[1:]):
        return CASE_FACTORS['title']
    return CASE_FACTORS['lower']


def score_text(text):
    feats = []
    by_word = {}
    for m in RE_WORD.finditer(text):
        w = m.group(0)
        lw = w.lower()
        if lw in KW:
            contrib = KW[lw] * case_factor(w)
            if contrib > by_word.get(lw, 0.0):
                by_word[lw] = contrib
    for lw, c in sorted(by_word.items(), key=lambda kv: -kv[1]):
        feats.append(('kw:' + lw, c))

    if RE_PARAM.search(text):
        feats.append(('param:&', FEAT['param_prefix']))

    for m in RE_DOTTED.finditer(text):
        head = m.group(0).split('.', 1)[0].lower()
        if head in METADATA_TYPES:
            feats.append(('metadata:' + head, FEAT['metadata_dotted']))
            break
    else:
        if RE_DOTTED.search(text):
            feats.append(('dotted', FEAT['dotted']))

    for name, rx, weight in STRONG_FORMS:
        if rx.search(text):
            feats.append((name, weight))

    if not feats:
        return 0.0, 0.0, feats
    ws = sorted((w for _, w in feats), reverse=True)
    strongest = ws[0]
    bonus = 0.0
    mult = 1.0
    for w in ws[1:]:
        bonus += w * mult
        mult *= COMBINE['decay']
    score = min(1.0, strongest + COMBINE['bonus_factor'] * bonus)
    return score, strongest, feats


def split_statements(src):
    stmts = []
    buf = []
    i = 0
    n = len(src)
    while i < n:
        c = src[i]
        if c == '/' and i + 1 < n and src[i + 1] == '/':
            while i < n and src[i] != '\n':
                i += 1
            continue
        if c == '"':
            start = i
            i += 1
            while i < n:
                if src[i] == '"':
                    if i + 1 < n and src[i + 1] == '"':
                        i += 2
                        continue
                    i += 1
                    break
                i += 1
            buf.append(src[start:i])
            continue
        if c == ';':
            stmts.append(''.join(buf))
            buf = []
            i += 1
            continue
        buf.append(c)
        i += 1
    if buf:
        stmts.append(''.join(buf))
    return stmts


def extract_strings(s):
    out = []
    i = 0
    n = len(s)
    while i < n:
        if s[i] == '"':
            start = i
            i += 1
            while i < n:
                if s[i] == '"':
                    if i + 1 < n and s[i + 1] == '"':
                        i += 2
                        continue
                    i += 1
                    break
                i += 1
            raw = s[start:i]
            out.append((s.count('\n', 0, start) + 1, '\n' in raw, raw))
            continue
        i += 1
    return out


def strip_string(raw):
    x = raw
    if x.startswith('"'):
        x = x[1:]
    if x.endswith('"'):
        x = x[:-1]
    x = x.replace('""', '"')
    x = re.sub(r'(?m)^\s*\|?\s?', '', x)
    return x


def analyze_file(path):
    with open(path, 'r', encoding='utf-8-sig', errors='replace') as f:
        src = f.read()
    items = []
    for stmt in split_statements(src):
        strings = extract_strings(stmt)
        if not strings:
            continue
        pos = src.find(strings[0][2])
        line = src.count('\n', 0, pos) + 1 if pos >= 0 else 0
        if len(strings) == 1 and '+' not in stmt:
            items.append((line, strings[0][1], strip_string(strings[0][2])))
        else:
            text = ' '.join(strip_string(s[2]) for s in strings if strip_string(s[2]))
            items.append((line, any(s[1] for s in strings), text))
    return items


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('path')
    ap.add_argument('--report', default=None)
    ap.add_argument('--threshold', type=float, default=CFG['threshold'])
    args = ap.parse_args()

    files = []
    if os.path.isdir(args.path):
        for root, _, names in os.walk(args.path):
            for nm in names:
                if nm.lower().endswith('.bsl'):
                    files.append(os.path.join(root, nm))
    else:
        files = [args.path]

    cases = []
    total = 0
    band = Counter()
    for fp in files:
        try:
            items = analyze_file(fp)
        except Exception as e:
            print('  ! %s: %s' % (fp, e), file=sys.stderr)
            continue
        for line, ml, text in items:
            total += 1
            sc, mx, feats = score_text(text)
            if sc >= 0.75:
                b = 'A>=0.75'
            elif sc >= args.threshold:
                b = 'B %.2f-0.75' % args.threshold
            elif sc >= 0.20:
                b = 'C 0.20-thresh'
            elif sc >= 0.15:
                b = 'D 0.15-0.20'
            else:
                b = 'E<0.15'
            band[b] += 1
            cases.append((sc, mx, fp, line, ml, text, feats))

    out = []
    out.append('=' * 100)
    out.append('КОРПУС: %d файлов, строковых литералов/групп: %d' % (len(files), total))
    out.append('=' * 100)
    out.append('Распределение оценок (порог %.2f):' % args.threshold)
    for b in sorted(band, key=lambda x: -band[x]):
        out.append('  %-16s %6d  (%5.1f%%)' % (b, band[b], 100.0 * band[b] / max(total, 1)))

    cases.sort(key=lambda t: -t[0])

    def dump(title, sel, limit=40):
        out.append('')
        out.append('-' * 100)
        out.append('%s (%d)' % (title, len(sel)))
        out.append('-' * 100)
        for sc, mx, fp, line, ml, text, feats in sel[:limit]:
            t = re.sub(r'\s+', ' ', text).strip()
            if len(t) > 110:
                t = t[:110] + '…'
            top = ', '.join('%s=%.2f' % (n, v) for n, v in sorted(feats, key=lambda f: -f[1])[:4])
            out.append('[%.3f] %s:%d ml=%d | %s' % (sc, os.path.basename(fp), line, int(ml), t))
            out.append('        признаки: %s' % top)

    dump('A. Высокая (>=0.75) — должны быть запросами',
         [c for c in cases if c[0] >= 0.75], 25)
    dump('B. Полоса у порога (%.2f-0.75) — тюнинг' % args.threshold,
         [c for c in cases if args.threshold <= c[0] < 0.75], 40)
    dump('C. Ниже порога, но есть запросное слово (0.20-threshold) — возможные пропуски',
         [c for c in cases if 0.20 <= c[0] < args.threshold], 40)
    traps = [c for c in cases if c[0] < 0.20 and re.search(r'(?i)\b(выбрать|где|из|как|есть)\b', c[5])]
    traps.sort(key=lambda t: -t[0])
    dump('D. Проза со словами-ловушками, <0.20 (проверить FP)', traps, 30)

    text = '\n'.join(out)
    if args.report:
        with open(args.report, 'w', encoding='utf-8') as f:
            f.write(text)
        print('Отчёт: %s' % args.report)
    else:
        print(text)


if __name__ == '__main__':
    main()
