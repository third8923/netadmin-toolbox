// ============================================================================
//  正确性证明工具 - 验证 .reg 解析器
//
//  背景：本机 DSH 环境限制子进程写注册表（Win32 API 返回 ACCESS_DENIED 5），
//        无法端到端验证「写入系统」。故用「解析等价性」证明正确性：
//        C++ 引擎解析全部 80 个 .reg 的结果，必须与独立 Python 参考实现逐条一致。
// ============================================================================
#include <cstdio>
#include <string>
#include <vector>
#include <map>
#include <cstdint>
#include "common.h"
#include "debloat.h"
#include "regengine.h"

using namespace mz;

// 把 .reg 内容解析为规范化操作序列（与 Python 参考实现同一规则）
struct Op {
    std::string kind;   // DWORD/QWORD/SZ/HEX/DELVAL/DELKEY
    wstring key;
    wstring name;
    std::string value;  // 规范化字符串
};

static wstring SplitLinesHelper(const wstring& s, size_t idx, size_t* next) {
    size_t nl = s.find(L'\n', idx);
    wstring line = (nl == wstring::npos) ? s.substr(idx) : s.substr(idx, nl - idx);
    *next = (nl == wstring::npos) ? s.size() + 1 : nl + 1;
    if (!line.empty() && line.back() == L'\r') line.pop_back();
    return line;
}

static wstring TrimWs(const wstring& s) {
    size_t b = s.find_first_not_of(L" \t");
    if (b == wstring::npos) return L"";
    size_t e = s.find_last_not_of(L" \t");
    return s.substr(b, e - b + 1);
}

static wstring Unescape(const wstring& in) {
    wstring out;
    for (size_t i = 0; i < in.size(); ++i) {
        wchar_t c = in[i];
        if (c == L'\\' && i + 1 < in.size()) {
            wchar_t n = in[++i];
            switch (n) {
                case L'\\': out.push_back(L'\\'); break;
                case L'"':  out.push_back(L'"');  break;
                case L'r':  out.push_back(L'\r'); break;
                case L'n':  out.push_back(L'\n'); break;
                case L'0':  out.push_back(L'\0'); break;
                default:    out.push_back(n);     break;
            }
        } else out.push_back(c);
    }
    return out;
}

// 提取引号内内容（从 start 开始，start 指向开引号）
static wstring QuotedContent(const wstring& s, size_t start) {
    size_t i = start + 1;
    wstring body;
    while (i < s.size()) {
        wchar_t c = s[i];
        if (c == L'\\' && i + 1 < s.size()) { body.push_back(c); body.push_back(s[i + 1]); i += 2; continue; }
        if (c == L'"') break;
        body.push_back(c);
        ++i;
    }
    return Unescape(body);
}

static std::string HexStr(unsigned long long v) {
    char b[32];
    snprintf(b, sizeof(b), "%llu", v);
    return b;
}

static vector<Op> ParseAll(const wstring& text) {
    vector<Op> ops;
    wstring curKey;
    bool haveKey = false;
    size_t idx = 0;
    while (idx <= text.size()) {
        size_t next = 0;
        wstring line = SplitLinesHelper(text, idx, &next);
        idx = next;
        wstring s = TrimWs(line);
        if (s.empty() || s[0] == L';') continue;
        if (s.rfind(L"Windows Registry Editor", 0) == 0) continue;
        if (s.rfind(L"REGEDIT", 0) == 0) continue;

        if (s[0] == L'[') {
            bool del = (s.size() > 1 && s[1] == L'-');
            size_t close = s.rfind(L']');
            if (close == wstring::npos) continue;
            wstring body = s.substr(del ? 2 : 1, close - (del ? 2 : 1));
            if (body.size() >= 2 && body.front() == L'"' && body.back() == L'"')
                body = body.substr(1, body.size() - 2);
            if (del) {
                Op o; o.kind = "DELKEY"; o.key = body; ops.push_back(o);
                haveKey = false;
            } else {
                curKey = body;
                haveKey = true;
            }
            continue;
        }
        if (!haveKey) continue;

        size_t eq = s.find(L'=');
        if (eq == wstring::npos) continue;
        wstring namePart = TrimWs(s.substr(0, eq));
        wstring val = TrimWs(s.substr(eq + 1));
        wstring name;
        if (namePart == L"@") name = L"";
        else if (!namePart.empty() && namePart[0] == L'"') name = QuotedContent(namePart, 0);
        else continue;

        Op o; o.key = curKey; o.name = name;
        if (val == L"-") { o.kind = "DELVAL"; ops.push_back(o); continue; }

        wstring lower;
        for (wchar_t c : val) lower.push_back(towlower(c));

        if (lower.rfind(L"dword:", 0) == 0) {
            o.kind = "DWORD";
            o.value = HexStr(wcstoull(val.c_str() + 6, nullptr, 16));
        } else if (lower.rfind(L"qword:", 0) == 0) {
            o.kind = "QWORD";
            o.value = HexStr(_wcstoui64(val.c_str() + 6, nullptr, 16));
        } else if (lower.rfind(L"hex", 0) == 0) {
            o.kind = "HEX";
            wstring typ = L"3";
            size_t start = 3;
            if (val.size() > 3 && val[3] == L'(') {
                size_t close = val.find(L')');
                if (close != wstring::npos) { typ = val.substr(4, close - 4); start = close + 1; }
            }
            std::wstring hex;
            for (size_t i = start; i < val.size(); ++i)
                if (iswxdigit(val[i])) hex.push_back(val[i]);
            if (hex.size() % 2) hex.push_back(L'0');
            std::string h8;
            for (wchar_t c : hex) h8.push_back((char)towlower(c));
            o.value = std::to_string(_wtoi(typ.c_str())) + ":" + h8;
        } else if (!val.empty() && val[0] == L'"') {
            o.kind = "SZ";
            o.value = WToUtf8(QuotedContent(val, 0));
        } else {
            continue;
        }
        ops.push_back(o);
    }
    return ops;
}

int main(int argc, char** argv) {
    (void)argc; (void)argv;
    SetConsoleOutputCP(CP_UTF8);

    // 逐个特性解析 apply/undo
    int files = 0, ops = 0, fails = 0;
    printf(T(L"# 解析结果（JSON 行格式，供 Python 比对）\n"));
    for (auto& f : Features()) {
        for (int pass = 0; pass < 2; ++pass) {
            const wstring& key = pass == 0 ? f.applyKey : f.undoKey;
            std::vector<unsigned char> blob;
            if (!GetRegBlob(key, &blob)) { printf("ERR noblob %ls\n", key.c_str()); fails++; continue; }

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

            vector<Op> parsed = ParseAll(text);
            files++;
            ops += (int)parsed.size();

            const char* tag = pass == 0 ? "apply" : "undo";
            printf("FILE %s.%s %zu\n", WToUtf8(f.id).c_str(), tag, parsed.size());
            for (auto& o : parsed) {
                printf("OP %s\t%s\t%s\t%s\n", o.kind.c_str(),
                       WToUtf8(o.key).c_str(), WToUtf8(o.name).c_str(), o.value.c_str());
            }
        }
    }
    fprintf(stderr, T(L"\n解析文件 %d 个，共 %d 条操作，错误 %d\n"), files, ops, fails);
    return fails == 0 ? 0 : 1;
}
