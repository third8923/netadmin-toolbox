// ============================================================================
//  NetAdmin Toolbox — YARA 子集扫描引擎实现（v2.14 M5）
//  链接：无新增库（纯 kernel32 范畴）
// ============================================================================
#include "yara.h"
#include "ruleset.h"        // ReadTextSmart（编码自适应读规则文件）

#include <algorithm>
#include <cstdio>
#include <cwchar>
#include <cwctype>
#include <unordered_map>

namespace mz {
namespace yara {

// ================================================================ 匹配原语

namespace {

bool ByteEq(BYTE a, BYTE b, bool nocase) {
    if (nocase) {
        if (a >= 'A' && a <= 'Z') a = (BYTE)(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z') b = (BYTE)(b - 'A' + 'a');
    }
    return a == b;
}

bool IsWordByte(BYTE b) {
    return (b >= 'a' && b <= 'z') || (b >= 'A' && b <= 'Z') ||
           (b >= '0' && b <= '9') || b == '_';
}

// 朴素查找（首个命中即返回；fullword 校验边界字节非字母数字下划线）
bool FindBytes(const BYTE* data, size_t size, const vector<BYTE>& pat,
               bool nocase, bool fullword, size_t* offOut) {
    if (pat.empty() || pat.size() > size) return false;
    for (size_t i = 0; i + pat.size() <= size; ++i) {
        size_t j = 0;
        while (j < pat.size() && ByteEq(data[i + j], pat[j], nocase)) ++j;
        if (j < pat.size()) continue;
        if (fullword) {
            if (i > 0 && IsWordByte(data[i - 1])) continue;
            if (i + pat.size() < size && IsWordByte(data[i + pat.size()])) continue;
        }
        if (offOut) *offOut = i;
        return true;
    }
    return false;
}

// 十六进制 token 串匹配（跳转递归回溯）
bool MatchHexAt(const BYTE* data, size_t size, size_t pos,
                const vector<HexTok>& toks, size_t ti) {
    while (ti < toks.size()) {
        const HexTok& t = toks[ti];
        if (t.type == 0) {                       // 字节
            if (pos >= size || data[pos] != t.byte) return false;
            ++pos; ++ti;
        } else if (t.type == 1) {                // ??
            if (pos >= size) return false;
            ++pos; ++ti;
        } else {                                 // [lo-hi] 跳转
            ULONG maxJ = (t.jhi == ULONG_MAX) ? (ULONG)(size - pos) : t.jhi;
            for (ULONG j = 0; j <= maxJ; ++j) {
                if (j < t.jlo) continue;
                if (pos + j > size) break;
                if (MatchHexAt(data, size, pos + j, toks, ti + 1)) return true;
            }
            return false;
        }
    }
    return true;   // token 用完 = 命中
}

bool FindHex(const BYTE* data, size_t size, const vector<HexTok>& toks,
             size_t* offOut) {
    if (toks.empty()) return false;
    for (size_t i = 0; i <= size; ++i) {
        if (MatchHexAt(data, size, i, toks, 0)) { if (offOut) *offOut = i; return true; }
        if (i == size) break;
    }
    return false;
}

// ================================================================ 词法

struct Tok {
    wstring text;    // word 词素 / 运算符
    int kind = 0;    // 0=word 1=字符串 2=hex 3=数字 4=punct
    wstring sval;            // kind=1 引号内文本
    vector<HexTok> hex;      // kind=2（解析失败时 sval 以 HEX: 开头记原因）
    long long num = 0;       // kind=3
};

// 十六进制串体解析：4D 5A ?? EB [4] [2-6] [4-]
bool ParseHexBody(const wstring& body, vector<HexTok>& out, wstring* why) {
    size_t i = 0;
    auto skipWs = [&]() { while (i < body.size() && iswspace(body[i])) ++i; };
    skipWs();
    while (i < body.size()) {
        wchar_t c = body[i];
        if (c == L'?') {
            if (i + 1 >= body.size() || body[i + 1] != L'?') { *why = L"非法通配符"; return false; }
            HexTok t; t.type = 1; out.push_back(t);
            i += 2; skipWs(); continue;
        }
        if (c == L'[') {
            size_t e = body.find(L']', i);
            if (e == wstring::npos) { *why = L"跳转缺少 ]"; return false; }
            wstring in = body.substr(i + 1, e - i - 1);
            HexTok t; t.type = 2; t.jhi = ULONG_MAX;
            size_t dash = in.find(L'-');
            auto toNum = [](const wstring& s, ULONG* v) {
                if (s.empty()) return false;
                ULONG r = 0;
                for (wchar_t ch : s) { if (ch < L'0' || ch > L'9') return false; r = r * 10 + (ULONG)(ch - L'0'); }
                *v = r; return true;
            };
            if (dash == wstring::npos) {
                if (!toNum(in, &t.jlo)) { *why = L"跳转数字非法"; return false; }
                t.jhi = t.jlo;
            } else {
                if (!toNum(in.substr(0, dash), &t.jlo)) { *why = L"跳转数字非法"; return false; }
                wstring b = in.substr(dash + 1);
                if (b.empty()) t.jhi = ULONG_MAX;
                else if (!toNum(b, &t.jhi)) { *why = L"跳转数字非法"; return false; }
            }
            out.push_back(t);
            i = e + 1; skipWs(); continue;
        }
        if (iswxdigit(c)) {
            if (i + 1 >= body.size() || !iswxdigit(body[i + 1])) { *why = L"十六进制字节需两位"; return false; }
            auto nib = [](wchar_t h) { return h <= L'9' ? (wchar_t)(h - L'0') : (wchar_t)(towlower(h) - L'a' + 10); };
            HexTok t; t.type = 0;
            t.byte = (BYTE)(nib(c) * 16 + nib(body[i + 1]));
            out.push_back(t);
            i += 2; skipWs(); continue;
        }
        if (c == L'|') { *why = L"不支持 alternation（|）"; return false; }
        if (c == L'(') { *why = L"不支持 hex 分组"; return false; }
        *why = L"十六进制串含非法字符";
        return false;
    }
    if (out.empty()) { *why = L"空十六进制串"; return false; }
    return true;
}

bool IsWordCh(wchar_t c) {
    return (c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') ||
           (c >= L'0' && c <= L'9') || c == L'_' || c == L'$' || c == L'*' ||
           c == L'-' || c == L'.';
}

vector<BYTE> ToUtf8Bytes(const wstring& s) {
    vector<BYTE> out;
    if (s.empty()) return out;
    int n = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(),
                                nullptr, 0, nullptr, nullptr);
    if (n <= 0) return out;
    out.resize((size_t)n);
    WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(),
                        (char*)out.data(), n, nullptr, nullptr);
    return out;
}

vector<BYTE> ToWideBytes(const wstring& s) {
    vector<BYTE> out((size_t)s.size() * 2);
    for (size_t i = 0; i < s.size(); ++i) {
        out[i * 2] = (BYTE)(s[i] & 0xFF);
        out[i * 2 + 1] = (BYTE)(s[i] >> 8);
    }
    return out;
}

void Tokenize(const wstring& src, vector<Tok>& toks) {
    size_t i = 0;
    auto skipTrivia = [&]() {
        for (;;) {
            while (i < src.size() && iswspace(src[i])) ++i;
            if (i + 1 < src.size() && src[i] == L'/' && src[i + 1] == L'/') {
                while (i < src.size() && src[i] != L'\n') ++i;
                continue;
            }
            if (i + 1 < src.size() && src[i] == L'/' && src[i + 1] == L'*') {
                size_t e = src.find(L"*/", i + 2);
                i = (e == wstring::npos) ? src.size() : e + 2;
                continue;
            }
            break;
        }
    };
    while (i < src.size()) {
        skipTrivia();
        if (i >= src.size()) break;
        wchar_t c = src[i];
        Tok t;
        if (c == L'"') {
            ++i;
            while (i < src.size() && src[i] != L'"') {
                if (src[i] == L'\\' && i + 1 < src.size()) {
                    wchar_t n = src[i + 1];
                    if (n == L'"' || n == L'\\') { t.sval += n; i += 2; continue; }
                    if (n == L'n') { t.sval += L'\n'; i += 2; continue; }
                    if (n == L't') { t.sval += L'\t'; i += 2; continue; }
                    if (n == L'x' && i + 3 < src.size() &&
                        iswxdigit(src[i + 2]) && iswxdigit(src[i + 3])) {
                        auto nib = [](wchar_t h) { return h <= L'9' ? (wchar_t)(h - L'0') : (wchar_t)(towlower(h) - L'a' + 10); };
                        t.sval += (wchar_t)(nib(src[i + 2]) * 16 + nib(src[i + 3]));
                        i += 4; continue;
                    }
                    t.sval += n; i += 2; continue;
                }
                t.sval += src[i++];
            }
            ++i;   // 闭引号
            t.kind = 1;
            toks.push_back(std::move(t));
            continue;
        }
        // `{` 的双重语义消歧：紧跟在 `=` 之后 = 十六进制串体；否则 = 规则体开括号
        if (c == L'{' &&
            !toks.empty() && toks.back().kind == 4 && toks.back().text == L"=") {
            size_t e = src.find(L'}', i);
            if (e != wstring::npos) {
                wstring body = src.substr(i + 1, e - i - 1);
                t.kind = 2;
                wstring why;
                if (!ParseHexBody(body, t.hex, &why)) t.sval = L"HEX:" + why;
                toks.push_back(std::move(t));
                i = e + 1;
                continue;
            }
        }
        // 运算符/标点（含双字符 <= >= == !=）
        if (c == L'<' || c == L'>' || c == L'=' || c == L'!') {
            if (i + 1 < src.size() && src[i + 1] == L'=') {
                t.kind = 4; t.text = wstring(1, c) + L'=';
                toks.push_back(std::move(t));
                i += 2;
                continue;
            }
        }
        if (c == L'{' || c == L'}' || c == L'(' || c == L')' || c == L':' ||
            c == L'=' || c == L',' || c == L'<' || c == L'>' || c == L'!') {
            t.kind = 4; t.text = wstring(1, c);
            toks.push_back(std::move(t));
            ++i;
            continue;
        }
        if (IsWordCh(c)) {
            while (i < src.size() && IsWordCh(src[i])) t.text += src[i++];
            // 数字词法：123 / 64KB / 20MB → 数值 token
            {
                const wstring& x = t.text;
                size_t dg = x.find_first_not_of(L"0123456789");
                if (dg == wstring::npos) {
                    t.kind = 3;
                    t.num = _wtoi64(x.c_str());
                } else if (dg > 0 && (x.substr(dg) == L"KB" || x.substr(dg) == L"MB")) {
                    bool allDig = x.find_first_not_of(L"0123456789", 0) == dg;
                    if (allDig) {
                        t.kind = 3;
                        t.num = _wtoi64(x.substr(0, dg).c_str()) *
                                (x.substr(dg) == L"KB" ? 1024LL : 1024LL * 1024);
                    }
                }
            }
            toks.push_back(std::move(t));
            continue;
        }
        ++i;   // 其他字符（正则 /、& 等）丢弃
    }
}

// ================================================================ 条件求值

struct CondEval {
    const vector<CondTok>& toks;
    size_t pos = 0;
    const std::unordered_map<wstring, long long>& matched;   // id → 命中偏移（-1 = 未命中）
    size_t filesize = 0;
    wstring err;

    CondEval(const vector<CondTok>& t, const std::unordered_map<wstring, long long>& m,
             size_t fs) : toks(t), matched(m), filesize(fs) {}

    static bool Hit(const std::unordered_map<wstring, long long>& m, const wstring& id) {
        auto it = m.find(id);
        return it != m.end() && it->second >= 0;
    }

    const CondTok* peek() { return pos < toks.size() ? &toks[pos] : nullptr; }
    bool atWord(const wchar_t* w) {
        const CondTok* t = peek();
        return t && !t->isNum && t->t == w;
    }
    bool eatWord(const wchar_t* w) { if (atWord(w)) { ++pos; return true; } return false; }

    // 集合展开：them / ($a, $b*, $c)
    vector<wstring> parseSet() {
        vector<wstring> ids;
        if (eatWord(L"them")) {
            for (const auto& kv : matched) ids.push_back(kv.first);
            return ids;
        }
        if (!eatWord(L"(")) { err = L"of 后需要 them 或 ("; return ids; }
        for (;;) {
            const CondTok* t = peek();
            if (!t) { err = L"集合未闭合"; break; }
            if (t->t == L")") { ++pos; break; }
            if (!t->t.empty() && t->t[0] == L'$') {
                if (t->t.size() >= 2 && t->t[t->t.size() - 1] == L'*') {
                    // 前缀通配 $ab*
                    wstring pre = t->t.substr(0, t->t.size() - 1);
                    for (const auto& kv : matched)
                        if (kv.first.rfind(pre, 0) == 0) ids.push_back(kv.first);
                } else {
                    ids.push_back(t->t);
                }
            }
            ++pos;
        }
        return ids;
    }

    static int CountOf(const std::unordered_map<wstring, long long>& m,
                       const vector<wstring>& ids) {
        int cnt = 0;
        for (const wstring& id : ids)
            if (Hit(m, id)) ++cnt;
        return cnt;
    }

    bool eval() {
        bool v = evalOr();
        if (err.empty() && pos != toks.size())
            err = L"条件末尾存在未识别内容";
        return v;
    }

    bool evalOr() {
        bool v = evalAnd();
        while (err.empty() && eatWord(L"or")) v = evalAnd() || v;
        return v;
    }
    bool evalAnd() {
        bool v = evalNot();
        while (err.empty() && eatWord(L"and")) {
            bool r = evalNot();
            v = v && r;
        }
        return v;
    }
    bool evalNot() {
        if (eatWord(L"not")) return !evalNot();
        return evalPrim();
    }

    bool evalPrim() {
        const CondTok* t = peek();
        if (!t) { err = L"条件意外结束"; return false; }

        if (t->t == L"(") {
            ++pos;
            bool v = evalOr();
            if (!eatWord(L")")) { if (err.empty()) err = L"缺少 )"; return false; }
            return v;
        }
        if (t->t == L"true") { ++pos; return true; }
        if (t->t == L"false") { ++pos; return false; }

        if (t->t == L"filesize") {
            ++pos;
            const CondTok* op = peek();
            if (!op || op->isNum || (op->t != L"<" && op->t != L">" &&
                op->t != L"<=" && op->t != L">=" && op->t != L"==" && op->t != L"!="))
                return true;   // 裸 filesize = true
            ++pos;
            const CondTok* n = peek();
            if (!n || !n->isNum) { err = L"filesize 比较缺少数字"; return false; }
            ++pos;
            long long mult = 1;
            if (atWord(L"KB")) { mult = 1024; ++pos; }
            else if (atWord(L"MB")) { mult = 1024 * 1024; ++pos; }
            long long rhs = n->num * mult;
            const wstring& o = op->t;
            if (o == L"<") return filesize < rhs;
            if (o == L">") return filesize > rhs;
            if (o == L"<=") return filesize <= rhs;
            if (o == L">=") return filesize >= rhs;
            if (o == L"==") return filesize == rhs;
            return filesize != rhs;
        }

        // N of set / any of set / all of set
        if (t->t == L"any" || t->t == L"all" || t->isNum) {
            long long need = 1;
            bool allMode = false;
            if (t->t == L"any") { need = 1; ++pos; }
            else if (t->t == L"all") { allMode = true; ++pos; }
            else { need = t->num; ++pos; }
            if (!eatWord(L"of")) { err = L"缺少 of"; return false; }
            vector<wstring> ids = parseSet();
            if (!err.empty()) return false;
            if (ids.empty()) { err = L"集合为空"; return false; }
            int cnt = CountOf(matched, ids);
            return allMode ? (cnt == (int)ids.size()) : (cnt >= (int)need);
        }

        if (!t->t.empty() && t->t[0] == L'$') {
            ++pos;
            bool hit = Hit(matched, t->t);
            // $a at N（命中位置校验）
            if (eatWord(L"at")) {
                const CondTok* n = peek();
                if (!n || !n->isNum) { err = L"at 后需要数字"; return false; }
                ++pos;
                auto it = matched.find(t->t);
                return hit && it != matched.end() && (size_t)it->second == (size_t)n->num;
            }
            return hit;
        }

        err = L"不支持的条件元素：" + t->t;
        return false;
    }
};

// ================================================================ 解析

} // namespace

size_t ParseRules(const wstring& text, vector<Rule>& out, wstring* err) {
    vector<Tok> toks;
    Tokenize(text, toks);
    size_t pos = 0;

    auto peek = [&](size_t ahead = 0) -> const Tok* {
        return (pos + ahead < toks.size()) ? &toks[pos + ahead] : nullptr;
    };

    while (pos < toks.size()) {
        const Tok* t = peek();
        if (!t) break;
        if (!(t->kind == 0 && t->text == L"rule")) { ++pos; continue; }
        ++pos;
        Rule r;
        const Tok* nm = peek();
        if (!nm || nm->kind != 0) { if (err) *err = L"rule 后缺少名称"; break; }
        r.name = nm->text;
        ++pos;
        // 可选 tags
        if (peek() && peek()->kind == 4 && peek()->text == L":") {
            ++pos;
            while (peek() && peek()->kind == 0 && peek()->text != L"{") {
                r.tags += (r.tags.empty() ? L"" : L",") + peek()->text;
                ++pos;
            }
        }
        if (!peek() || peek()->kind != 4 || peek()->text != L"{") {
            r.error = L"缺少规则体 {";
            out.push_back(std::move(r));
            continue;
        }
        ++pos;

        // ---- 规则体：meta / strings / condition 三段 ----
        enum Sec { S_NONE, S_META, S_STRINGS, S_COND };
        Sec sec = S_NONE;
        YaraString cur;
        wstring curText;      // 文本串原文（供 wide 形态构建）
        bool hasText = false, hasHex = false;
        bool wideF = false, asciiF = false;
        bool inStr = false;
        int depth = 1;
        while (pos < toks.size() && depth > 0) {
            const Tok* tk = peek();
            if (tk->kind == 4 && tk->text == L"{") { ++depth; ++pos; continue; }
            if (tk->kind == 4 && tk->text == L"}") {
                --depth; ++pos;
                continue;
            }
            if (tk->kind == 0 && tk->text == L"meta") { sec = S_META; ++pos; if (peek() && peek()->text == L":") ++pos; continue; }
            if (tk->kind == 0 && tk->text == L"strings") { sec = S_STRINGS; inStr = false; ++pos; if (peek() && peek()->text == L":") ++pos; continue; }
            if (tk->kind == 0 && tk->text == L"condition") { sec = S_COND; ++pos; if (peek() && peek()->text == L":") ++pos; continue; }

            if (sec == S_META) { ++pos; continue; }

            if (sec == S_STRINGS) {
                if (tk->kind == 0 && !tk->text.empty() && tk->text[0] == L'$') {
                    // 新条目前先落盘上一条
                    if (inStr) {
                        if (hasText && !wideF) asciiF = true;   // 无修饰 = ascii
                        if (hasText && asciiF) cur.ascii = ToUtf8Bytes(curText);
                        if (hasText && wideF) cur.wide = ToWideBytes(curText);
                        r.strings.push_back(cur);
                    }
                    cur = YaraString{};
                    cur.id = tk->text;
                    curText.clear();
                    hasText = hasHex = wideF = asciiF = false;
                    inStr = true;
                    ++pos;
                    if (peek() && peek()->kind == 4 && peek()->text == L"=") ++pos;
                    continue;
                }
                if (!inStr) { ++pos; continue; }
                if (tk->kind == 1) {   // 文本串
                    curText = tk->sval;
                    hasText = true;
                    ++pos;
                    continue;
                }
                if (tk->kind == 2) {   // hex 串
                    if (!tk->sval.empty() && tk->sval.rfind(L"HEX:", 0) == 0)
                        r.error = L"十六进制串：" + tk->sval.substr(4);
                    else { cur.hex = tk->hex; hasHex = true; }
                    ++pos;
                    continue;
                }
                if (tk->kind == 0) {   // 修饰词
                    if (tk->text == L"wide") wideF = true;
                    else if (tk->text == L"ascii") asciiF = true;
                    else if (tk->text == L"nocase") cur.nocase = true;
                    else if (tk->text == L"fullword") cur.fullword = true;
                    else r.error = L"不支持的字符串修饰：" + tk->text;
                    ++pos;
                    continue;
                }
                ++pos;
                continue;
            }

            if (sec == S_COND) {
                CondTok ct;
                ct.t = tk->text;
                ct.isNum = (tk->kind == 3);
                ct.num = tk->num;
                r.cond.push_back(std::move(ct));
                ++pos;
                continue;
            }
            ++pos;
        }
        if (inStr) {
            if (hasText && !wideF) asciiF = true;
            if (hasText && asciiF) cur.ascii = ToUtf8Bytes(curText);
            if (hasText && wideF) cur.wide = ToWideBytes(curText);
            r.strings.push_back(cur);
        }

        // 条件合法性预检（不求值，抓不认识的词）
        if (r.error.empty() && r.cond.empty()) r.error = L"缺少 condition";
        out.push_back(std::move(r));
    }
    return out.size();
}

// ================================================================ 扫描

void ScanBuffer(vector<Rule>& rules, const BYTE* data, size_t size,
                const wstring& label, vector<Hit>& out) {
    for (Rule& r : rules) {
        if (!r.error.empty()) continue;
        std::unordered_map<wstring, long long> matched;   // id → 偏移（-1 未命中）
        size_t firstHitOff = 0;
        // ① 逐字符串匹配
        for (const YaraString& s : r.strings) {
            s.matched = false;
            s.firstOff = 0;
            size_t off = 0;
            bool hit = false;
            if (!s.hex.empty()) {
                hit = FindHex(data, size, s.hex, &off);
            } else {
                if (!s.ascii.empty() && FindBytes(data, size, s.ascii, s.nocase, s.fullword, &off))
                    hit = true;
                if (!hit && !s.wide.empty() && FindBytes(data, size, s.wide, s.nocase, s.fullword, &off))
                    hit = true;
            }
            matched[s.id] = hit ? (long long)off : -1LL;
            if (hit) { s.matched = true; s.firstOff = off; }
        }
        // ② 条件求值
        if (r.cond.empty()) continue;
        CondEval ev(r.cond, matched, size);
        bool cond = ev.eval();
        if (!ev.err.empty()) {
            // 求值失败视为该规则不适用（解析期未发现的问题）
            continue;
        }
        if (!cond) continue;
        // ③ 命中汇总
        Hit h;
        h.rule = r.name;
        h.file = label;
        for (const YaraString& s : r.strings) {
            if (s.matched) {
                h.sids += (h.sids.empty() ? L"" : L",") + s.id +
                          L"@" + std::to_wstring(s.firstOff);
                if (firstHitOff == 0) firstHitOff = s.firstOff;
            }
        }
        h.offset = firstHitOff;
        out.push_back(std::move(h));
    }
}

void ScanPath(vector<Rule>& rules, const wstring& path,
              vector<Hit>& out, int* fileCount, const bool* cancelled,
              wstring* err) {
    DWORD attr = GetFileAttributesW(path.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES) {
        if (err) *err = L"路径不存在：" + path;
        return;
    }
    const ULONGLONG kMaxFile = 32ull * 1024 * 1024;
    int files = 0;

    // 单文件
    if (!(attr & FILE_ATTRIBUTE_DIRECTORY)) {
        WIN32_FILE_ATTRIBUTE_DATA fa{};
        if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fa) &&
            ((unsigned long long)fa.nFileSizeHigh << 32 | fa.nFileSizeLow) > kMaxFile)
            return;   // 超大文件跳过
        HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
                               nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) return;
        LARGE_INTEGER sz{};
        GetFileSizeEx(h, &sz);
        vector<BYTE> buf((size_t)sz.QuadPart);
        DWORD got = 0;
        BOOL ok = ReadFile(h, buf.data(), (DWORD)buf.size(), &got, nullptr);
        CloseHandle(h);
        if (ok) {
            ++files;
            ScanBuffer(rules, buf.data(), buf.size(), path, out);
        }
        if (fileCount) *fileCount = files;
        return;
    }

    // 目录递归
    vector<wstring> stack{ path };
    while (!stack.empty()) {
        if (cancelled && *cancelled) break;
        wstring dir = stack.back();
        stack.pop_back();
        WIN32_FIND_DATAW fd{};
        HANDLE f = FindFirstFileW((dir + L"\\*").c_str(), &fd);
        if (f == INVALID_HANDLE_VALUE) continue;
        do {
            if (cancelled && *cancelled) break;
            wstring name = fd.cFileName;
            if (name == L"." || name == L"..") continue;
            wstring full = dir + L"\\" + name;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                stack.push_back(full);
                continue;
            }
            ULONGLONG fsz = ((ULONGLONG)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
            if (fsz > kMaxFile) continue;
            if (fsz == 0) continue;
            if (files >= 20000) break;
            HANDLE h = CreateFileW(full.c_str(), GENERIC_READ, FILE_SHARE_READ,
                                   nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (h == INVALID_HANDLE_VALUE) continue;
            vector<BYTE> buf((size_t)fsz);
            DWORD got = 0;
            BOOL ok = ReadFile(h, buf.data(), (DWORD)buf.size(), &got, nullptr);
            CloseHandle(h);
            if (!ok || got == 0) continue;
            ++files;
            ScanBuffer(rules, buf.data(), buf.size(), full, out);
        } while (FindNextFileW(f, &fd));
        FindClose(f);
    }
    if (fileCount) *fileCount = files;
}

} // namespace yara
} // namespace mz
