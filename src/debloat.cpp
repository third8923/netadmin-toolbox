#include "debloat.h"
#include "regengine.h"
#include "features_tsv.h"     // 由 gen_data.py 生成：内嵌 features.tsv
#include "regfiles.h"         // 内嵌 reg 原始内容
#include <sstream>

namespace mz {

// ---------------------------------------------------------------- 内嵌数据

// 手写分割：不用 wistringstream + getline(L'\t')。
// MinGW 的宽字符流在默认 locale 下对单字符分隔符处理不可靠
// （实测会把 "DisableTelemetry\t..." 的首字段解析成 "D"），故全部改为 find 切分。
static vector<wstring> SplitBy(const wstring& s, wchar_t sep) {
    vector<wstring> out;
    size_t start = 0;
    while (true) {
        size_t pos = s.find(sep, start);
        if (pos == wstring::npos) {
            out.push_back(s.substr(start));
            break;
        }
        out.push_back(s.substr(start, pos - start));
        start = pos + 1;
    }
    return out;
}

static vector<wstring> SplitLines(const wstring& s) {
    vector<wstring> out;
    size_t start = 0;
    while (start <= s.size()) {
        size_t pos = s.find(L'\n', start);
        wstring line = (pos == wstring::npos) ? s.substr(start) : s.substr(start, pos - start);
        if (!line.empty() && line.back() == L'\r') line.pop_back();
        out.push_back(line);
        if (pos == wstring::npos) break;
        start = pos + 1;
    }
    return out;
}

const vector<DebloatFeature>& Features() {
    static vector<DebloatFeature> cache;
    static bool loaded = false;
    if (loaded) return cache;
    loaded = true;

    std::string raw((const char*)FEATURES_TSV, FEATURES_TSV_SIZE);
    wstring all = Utf8ToW(raw);

    for (auto& line : SplitLines(all)) {
        if (line.empty() || line[0] == L'#') continue;
        auto f = SplitBy(line, L'\t');
        if (f.size() < 11) continue;
        DebloatFeature d;
        d.id = f[0];
        d.categoryEn = f[1];
        d.category = f[2];
        d.labelEn = f[3];
        d.label = f[4];
        d.tip = f[5];
        d.applyKey = f[6];
        d.undoKey = f[7];
        d.applyText = f[8];
        d.undoLabel = f[9];
        wstring flags = f[10];
        d.needsExplorer = flags.find(L"EXPLORER") != wstring::npos;
        d.needsReboot = flags.find(L"REBOOT") != wstring::npos;
        cache.push_back(d);
    }
    return cache;
}

vector<std::pair<wstring, vector<const DebloatFeature*>>> FeaturesByCategory() {
    vector<std::pair<wstring, vector<const DebloatFeature*>>> out;
    for (auto& f : Features()) {
        bool found = false;
        for (auto& g : out) {
            if (g.first == f.category) { g.second.push_back(&f); found = true; break; }
        }
        if (!found) out.push_back({ f.category, { &f } });
    }
    return out;
}

bool GetRegBlob(const wstring& key, vector<unsigned char>* out) {
    for (size_t i = 0; i < REG_BLOB_COUNT; ++i) {
        if (key == Utf8ToW(REG_BLOBS[i].key)) {
            out->assign(REG_BLOBS[i].data, REG_BLOBS[i].data + REG_BLOBS[i].size);
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------- 应用

ApplyOutcome ApplyFeature(const DebloatFeature& f, bool undo, const LogSink& log) {
    ApplyOutcome r;
    vector<unsigned char> blob;
    const wstring& key = undo ? f.undoKey : f.applyKey;
    if (!GetRegBlob(key, &blob)) {
        r.message = L"找不到内嵌注册表内容: " + key;
        return r;
    }
    RegPatchStats st = ApplyRegContent(blob, log);
    r.valuesSet = st.valuesSet;
    r.errors = st.errors;
    r.success = (st.errors == 0);
    r.needsExplorer = f.needsExplorer;
    r.needsReboot = f.needsReboot;

    wchar_t buf[512];
    if (r.success) {
        swprintf_s(buf, L"完成：写入 %d 项", st.valuesSet);
    } else {
        swprintf_s(buf, L"部分失败：成功 %d，失败 %d", st.valuesSet, st.errors);
    }
    r.message = buf;
    return r;
}

BatchOutcome ApplyFeatures(const vector<const DebloatFeature*>& feats, bool undo,
                           const LogSink& log, const ProgressFn& progress,
                           const CancelFn& cancel) {
    BatchOutcome out;
    int total = (int)feats.size();
    int i = 0;
    for (auto f : feats) {
        if (cancel && cancel()) break;
        ++i;
        if (progress) progress(total ? (i * 100 / total) : 100, (undo ? L"正在还原: " : L"正在应用: ") + f->label);
        if (log.onLine) log.Line((undo ? L"[还原] " : L"[应用] ") + f->label);

        ApplyOutcome r = ApplyFeature(*f, undo, log);
        if (log.onLine && !r.message.empty()) log.Line(L"        " + r.message);

        if (r.success) out.ok++;
        else { out.failed++; out.failedIds.push_back(f->id); }
        if (r.needsExplorer) out.needExplorer = true;
    }
    return out;
}

// ---------------------------------------------------------------- 状态检测

// 从 reg 内容中提取「期望值」：支持 dword / qword / 字符串 / hex 四类，
// 旧版只认 dword，导致使用 REG_SZ（如 MouseSpeed="0"、Flags="506"）的特性
// 应用后状态永远显示「未应用」，被误判为「应用不成功」。
enum class ExpKind { Dword, Qword, String, Binary };
struct ExpectedVal {
    wstring key;          // [HKEY_...] 完整路径
    wstring name;         // 值名（默认值为空字符串）
    ExpKind kind;
    wstring raw;          // dword/qword 的十六进制原文 / string 的解码文本
    vector<BYTE> bytes;  // Binary 模式的原始字节
};

// 解析 "name" 直到匹配的引号，支持 \" 转义
static wstring ParseQuotedName(const wstring& line, size_t* after) {
    wstring out;
    size_t i = 1;
    while (i < line.size()) {
        wchar_t c = line[i];
        if (c == L'\\' && i + 1 < line.size()) {
            wchar_t n = line[i + 1];
            switch (n) {
                case L'\\': out.push_back(L'\\'); break;
                case L'"':  out.push_back(L'"');  break;
                default:    out.push_back(n);     break;
            }
            i += 2;
        } else if (c == L'"') {
            if (after) *after = i + 1;
            return out;
        } else {
            out.push_back(c);
            ++i;
        }
    }
    if (after) *after = i;
    return out;
}

static vector<BYTE> ParseHexDigits2(const wstring& s, size_t start) {
    vector<BYTE> out;
    wstring hex;
    for (size_t i = start; i < s.size(); ++i) {
        wchar_t c = s[i];
        if (iswxdigit(c)) hex.push_back(c);
    }
    if (hex.size() % 2) hex.push_back(L'0');
    for (size_t i = 0; i + 1 < hex.size(); i += 2) {
        wchar_t buf[3] = { hex[i], hex[i + 1], 0 };
        out.push_back((BYTE)wcstoul(buf, nullptr, 16));
    }
    return out;
}

static void ExtractExpected(const vector<unsigned char>& blob, vector<ExpectedVal>* out) {
    wstring text;
    if (blob.size() >= 2 && blob[0] == 0xFF && blob[1] == 0xFE) {
        const wchar_t* p = reinterpret_cast<const wchar_t*>(blob.data() + 2);
        size_t chars = (blob.size() - 2) / 2;
        while (chars > 0 && p[chars - 1] == 0) --chars;
        text.assign(p, chars);
    } else {
        text = Utf8ToW(std::string((const char*)blob.data(), blob.size()));
    }
    if (!text.empty() && text[0] == 0xFEFF) text.erase(0, 1);

    wstring curKey;
    size_t start = 0;
    while (start <= text.size()) {
        size_t nl = text.find(L'\n', start);
        wstring line = (nl == wstring::npos) ? text.substr(start) : text.substr(start, nl - start);
        start = (nl == wstring::npos) ? text.size() + 1 : nl + 1;
        if (!line.empty() && line.back() == L'\r') line.pop_back();
        size_t b = line.find_first_not_of(L" \t");
        if (b == wstring::npos) continue;
        line = line.substr(b);
        if (line[0] == L';') continue;
        if (line[0] == L'[') {
            size_t e = line.find(L']');
            if (e != wstring::npos) curKey = line.substr(1, e - 1);
            continue;
        }
        // 值行：@=... 或 "name"=...
        wstring name, data;
        if (line[0] == L'@') {
            size_t eq = line.find(L'=');
            if (eq == wstring::npos) continue;
            name = L"";
            data = line.substr(eq + 1);
        } else if (line[0] == L'"') {
            size_t after = 0;
            name = ParseQuotedName(line, &after);
            size_t eq = line.find(L'=', after);
            if (eq == wstring::npos) continue;
            data = line.substr(eq + 1);
        } else {
            continue;
        }
        if (curKey.empty()) continue;
        // 删除值（"-"）：应用态要求该值不存在，不计入期望值
        if (!data.empty() && data[0] == L'-' && (data.size() == 1 || data[1] != L' '))
            continue;
        while (!data.empty() && (data.back() == L' ' || data.back() == L'\r')) data.pop_back();
        while (!data.empty() && data.front() == L' ') data.erase(0, 1);

        ExpectedVal v;
        v.key = curKey;
        v.name = name;
        if (_wcsnicmp(data.c_str(), L"dword:", 6) == 0) {
            v.kind = ExpKind::Dword;
            v.raw = data.substr(6);
        } else if (_wcsnicmp(data.c_str(), L"qword:", 6) == 0) {
            v.kind = ExpKind::Qword;
            v.raw = data.substr(6);
        } else if (_wcsnicmp(data.c_str(), L"hex", 3) == 0) {
            v.kind = ExpKind::Binary;
            size_t s = 3;
            if (data.size() > 3 && data[3] == L'(') {
                size_t close = data.find(L')');
                if (close != wstring::npos) s = close + 1;
            }
            if (s < data.size() && data[s] == L':') ++s;
            v.bytes = ParseHexDigits2(data, s);
        } else if (!data.empty() && data[0] == L'"') {
            size_t after = 0;
            v.kind = ExpKind::String;
            v.raw = ParseQuotedName(data, &after);
        } else {
            continue;  // 不支持的格式，跳过
        }
        out->push_back(v);
    }
}

bool IsFeatureApplied(const DebloatFeature& f) {
    vector<unsigned char> blob;
    if (!GetRegBlob(f.applyKey, &blob)) return false;

    vector<ExpectedVal> expected;
    ExtractExpected(blob, &expected);
    if (expected.empty()) return false;

    int matched = 0, checked = 0;
    for (auto& e : expected) {
        HKEY root; wstring sub;
        if (!SplitRegPath(e.key, &root, &sub)) continue;
        ++checked;
        bool ok = false;
        switch (e.kind) {
        case ExpKind::Dword: {
            DWORD cur = 0;
            if (ReadRegDword(root, sub, e.name, &cur))
                ok = (cur == (DWORD)wcstoul(e.raw.c_str(), nullptr, 16));
            break;
        }
        case ExpKind::Qword: {
            unsigned long long cur = 0;
            if (ReadRegQword(root, sub, e.name, &cur))
                ok = (cur == _wcstoui64(e.raw.c_str(), nullptr, 16));
            break;
        }
        case ExpKind::String: {
            wstring cur;
            if (ReadRegString(root, sub, e.name, &cur)) {
                wstring a = e.raw, bb = cur;
                while (!a.empty() && (a.back() == L' ' || a.back() == L'\r')) a.pop_back();
                while (!bb.empty() && (bb.back() == L' ' || bb.back() == L'\r')) bb.pop_back();
                ok = (_wcsicmp(a.c_str(), bb.c_str()) == 0);
            }
            break;
        }
        case ExpKind::Binary: {
            vector<BYTE> cur;
            if (ReadRegBinary(root, sub, e.name, &cur))
                ok = (cur == e.bytes);
            break;
        }
        }
        if (ok) ++matched;
    }
    if (checked == 0) return false;
    // 全部期望值都匹配才视为已应用
    return matched == checked;
}

} // namespace mz
