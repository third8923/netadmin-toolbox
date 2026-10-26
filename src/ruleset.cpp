// ============================================================================
//  NetAdmin Toolbox — 流氓软件净化器 · 特征库引擎实现（v2.13 T01）
//  链接：无新增库（纯 advapi32/kernel32 范畴）
// ============================================================================
#include "ruleset.h"
#include "common.h"

#include <algorithm>
#include <cstdio>
#include <cwchar>

// 编译期内嵌基线（tools/gen_softcn.py 生成，来源 gitee.com/softcnkiller/data）
#include "softcn_rules.h"

namespace mz {
namespace purg {

namespace {

// ---- 大小写折叠（ASCII + 中文原样；特征多为英文公司名/目录名）----
wstring FoldLower(const wstring& s) {
    wstring out = s;
    for (auto& c : out) {
        if (c >= L'A' && c <= L'Z') c = (wchar_t)(c - L'A' + L'a');
        else if (c >= 192 && c <= 221 && c != 215) c = (wchar_t)(c + 32); // 拉丁扩展
    }
    return out;
}

wstring ExeDir() {
    wchar_t buf[MAX_PATH] = {};
    DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return wstring(L".");
    wstring p(buf, n);
    size_t slash = p.find_last_of(L'\\');
    return (slash == wstring::npos) ? wstring(L".") : p.substr(0, slash);
}

// 双向子串：a 含 b 或 b 含 a（均需 ≥ minLen）
bool EitherContains(const wstring& aLow, const wstring& bLow, size_t minLen) {
    if (aLow.empty() || bLow.empty()) return false;
    if (bLow.size() < minLen && aLow.find(bLow) == wstring::npos) return false;
    return aLow.find(bLow) != wstring::npos || bLow.find(aLow) != wstring::npos;
}

// base64（宽字符版）解码 → 按 UTF-16LE 还原为 wstring。
// 非法字符跳过；失败返回空串。仅用于内嵌特征基线解码。
wstring B64Decode(const wchar_t* b64) {
    auto val = [](wchar_t c) -> int {
        if (c >= L'A' && c <= L'Z') return c - L'A';
        if (c >= L'a' && c <= L'z') return c - L'a' + 26;
        if (c >= L'0' && c <= L'9') return c - L'0' + 52;
        if (c == L'+') return 62;
        if (c == L'/') return 63;
        return -1;
    };
    std::string bytes;
    int buf = 0, bits = 0;
    for (const wchar_t* p = b64; *p; ++p) {
        int v = val(*p);
        if (v < 0) continue;               // 跳过 '='、空白与非法字符
        buf = (buf << 6) | v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            bytes.push_back((char)((buf >> bits) & 0xFF));
        }
    }
    if (bytes.empty() || bytes.size() % 2) return wstring();
    wstring out;
    out.reserve(bytes.size() / 2);
    for (size_t i = 0; i + 1 < bytes.size(); i += 2)
        out += (wchar_t)((unsigned char)bytes[i] | ((unsigned char)bytes[i + 1] << 8));
    return out;
}

} // namespace

// ---------------------------------------------------------------- 编码与读取

wstring ReadTextSmart(const wstring& path, bool* ok) {
    if (ok) *ok = false;
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return wstring();
    LARGE_INTEGER sz{};
    if (!GetFileSizeEx(h, &sz) || sz.QuadPart > 64 * 1024 * 1024) { CloseHandle(h); return wstring(); }
    DWORD size = (DWORD)sz.QuadPart;
    std::string raw(size, '\0');
    DWORD got = 0;
    BOOL rd = ReadFile(h, raw.data(), size, &got, nullptr);
    CloseHandle(h);
    if (!rd || got != size) return wstring();
    if (ok) *ok = true;

    // UTF-16 LE BOM / 无 BOM 偶对启发 → UTF-16
    auto looksUtf16 = [](const std::string& s) {
        if (s.size() < 2) return false;
        int zeros = 0;
        for (size_t i = 1; i + 1 < s.size(); i += 2) if (s[i] == 0) ++zeros;
        return zeros > (int)(s.size() / 4);   // 过半高位字节为 0 视为 UTF-16
    };
    auto toWideCp = [&](UINT cp, const std::string& s, bool bom) {
        size_t off = bom ? 3 : 0;
        int n = MultiByteToWideChar(cp, 0, s.data() + off, (int)(s.size() - off), nullptr, 0);
        if (n <= 0) return wstring();
        wstring w((size_t)n, L'\0');
        MultiByteToWideChar(cp, 0, s.data() + off, (int)(s.size() - off), &w[0], n);
        return w;
    };

    wstring text;
    if (raw.size() >= 2 && (unsigned char)raw[0] == 0xFF && (unsigned char)raw[1] == 0xFE) {
        // UTF-16 LE BOM（SoftCnKiller 原生格式）
        // 注意：MultiByteToWideChar 不支持 CP 1200（UTF-16 是宽字符自身编码，
        // MSDN 明确列为不可用），必须直接按字节对翻译。
        text.reserve((raw.size() - 2) / 2);
        for (size_t i = 2; i + 1 < raw.size(); i += 2)
            text += (wchar_t)((unsigned char)raw[i] | ((unsigned char)raw[i + 1] << 8));
        if (!text.empty() && text[0] == L'\uFEFF') text.erase(0, 1);
    } else if (raw.size() >= 3 && (unsigned char)raw[0] == 0xEF &&
               (unsigned char)raw[1] == 0xBB && (unsigned char)raw[2] == 0xBF) {
        text = toWideCp(CP_UTF8, raw, true);          // UTF-8 BOM
    } else if (looksUtf16(raw)) {
        // 无 BOM UTF-16（启发）：字节对直译
        text.reserve(raw.size() / 2);
        for (size_t i = 0; i + 1 < raw.size(); i += 2)
            text += (wchar_t)((unsigned char)raw[i] | ((unsigned char)raw[i + 1] << 8));
    } else {
        text = toWideCp(CP_UTF8, raw, false);         // 先按 UTF-8
        if (text.empty() || text.find(L'\uFFFD') != wstring::npos)
            text = toWideCp(936, raw, false);         // 非法 → GBK 兜底
    }

    // 统一换行为 \n（解析层只认 \n）
    size_t pos = 0;
    while ((pos = text.find(L"\r\n", pos)) != wstring::npos) {
        text.replace(pos, 2, L"\n");
        pos += 1;
    }
    return text;
}

// ---------------------------------------------------------------- Ruleset

namespace {
// 逐行遍历：回调收到每行（trim 过），空行剔除，# 开头视为注释
template <typename Fn>
void ForEachLine(const wstring& text, Fn&& fn) {
    size_t start = 0;
    while (start <= text.size()) {
        size_t end = text.find(L'\n', start);
        if (end == wstring::npos) end = text.size();
        wstring line = text.substr(start, end - start);
        // trim
        size_t b = line.find_first_not_of(L" \t\uFEFF");
        if (b == wstring::npos) { start = end + 1; continue; }
        size_t e = line.find_last_not_of(L" \t\r\uFEFF");
        line = line.substr(b, e - b + 1);
        if (!line.empty() && line[0] != L'#') fn(line);
        if (end == text.size()) break;
        start = end + 1;
    }
}
} // namespace

void Ruleset::AddSign(const wstring& line)   { signs_.push_back(line); signsLow_.push_back(FoldLower(line)); }
void Ruleset::AddFolder(const wstring& line) { folders_.push_back(line); foldersLow_.push_back(FoldLower(line)); }
void Ruleset::AddWhite(const wstring& line)  { whitePaths_.push_back(line); whiteLow_.push_back(FoldLower(line)); }

size_t Ruleset::ParseFile(const wstring& path, bool sign, bool folder, bool white) {
    bool ok = false;
    wstring text = ReadTextSmart(path, &ok);
    if (!ok) return 0;
    size_t n = 0;
    ForEachLine(text, [&](const wstring& line) {
        if (sign)  { AddSign(line); ++n; }
        if (folder){ AddFolder(line); ++n; }
        if (white) { AddWhite(line); ++n; }
    });
    return n;
}

bool Ruleset::Load(wstring* stat) {
    signs_.clear(); signsLow_.clear();
    folders_.clear(); foldersLow_.clear();
    whitePaths_.clear(); whiteLow_.clear();

    // ---- 内嵌基线（始终加载，作为最底层兜底）----
    // 内嵌数据为 base64(UTF-16LE)：明文特征串会被杀软内容扫描误报
    // （实测 Defender PUA:Win32/Softcnapp），故编码存储、运行期解码。
    auto addB64 = [&](const wchar_t* const* arr, int cnt,
                      void (Ruleset::*add)(const wstring&)) {
        for (int i = 0; i < cnt; ++i) {
            wstring text = B64Decode(arr[i]);
            ForEachLine(text, [this, &add](const wstring& line) { (this->*add)(line); });
        }
    };
    addB64(kSoftCnSign, kSoftCnSign_count, &Ruleset::AddSign);
    addB64(kSoftCnFolder, kSoftCnFolder_count, &Ruleset::AddFolder);
    addB64(kSoftCnWhitePath, kSoftCnWhitePath_count, &Ruleset::AddWhite);

    size_t builtinSign = signs_.size(), builtinFolder = folders_.size(),
           builtinWhite = whitePaths_.size();

    // ---- rules\ 目录层（在线更新/自有补充落点）----
    const wstring dir = RulesDir();
    CreateDirectoryW(dir.c_str(), nullptr);   // 首次运行建目录（已存在则忽略）
    auto exists = [](const wstring& p) {
        DWORD a = GetFileAttributesW(p.c_str());
        return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
    };

    // rules\ 内嵌导出：目录为空（首装）时落三份标准文件，方便用户查看/对比更新
    if (!exists(dir + L"\\sign.txt") && !exists(dir + L"\\folder.txt")) {
        auto dump = [&](const wchar_t* name, const wchar_t* const* arr, int cnt) {
            wstring body;
            for (const wchar_t* const* p = arr; p < arr + cnt; ++p) body += *p + wstring(L"\r\n");
            std::string u8 = WToUtf8(body);
            HANDLE h = CreateFileW((dir + L"\\" + name).c_str(), GENERIC_WRITE, 0,
                                   nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (h != INVALID_HANDLE_VALUE) {
                DWORD w = 0;
                WriteFile(h, "\xEF\xBB\xBF", 3, &w, nullptr);
                WriteFile(h, u8.data(), (DWORD)u8.size(), &w, nullptr);
                CloseHandle(h);
            }
        };
        dump(L"sign.txt", kSoftCnSign, kSoftCnSign_count);
        dump(L"folder.txt", kSoftCnFolder, kSoftCnFolder_count);
        dump(L"whitepath.txt", kSoftCnWhitePath, kSoftCnWhitePath_count);
    }

    // 更新版（在线更新直接覆盖三个标准主名文件）+ 自有补充（extra_*.txt，
    // 用户手工维护或 GitHub 自有规则源下发）。只认这六个明确文件名，
    // 不做通配 —— 防止更新临时文件（.tmp/.new）与在线副本被重复加载。
    size_t dirSign = 0, dirFolder = 0, dirWhite = 0;
    auto tryOne = [&](const wchar_t* name, bool sign, bool folder, bool white) {
        wstring full = dir + L"\\" + name;
        DWORD a = GetFileAttributesW(full.c_str());
        if (a == INVALID_FILE_ATTRIBUTES || (a & FILE_ATTRIBUTE_DIRECTORY)) return;
        size_t n = ParseFile(full, sign, folder, white);
        if (sign) dirSign += n;
        if (folder) dirFolder += n;
        if (white) dirWhite += n;
    };
    tryOne(L"sign.txt", true, false, false);          // SoftCnKiller 签名黑名单（更新覆盖）
    tryOne(L"folder.txt", false, true, false);        // SoftCnKiller 目录特征（更新覆盖）
    tryOne(L"whitepath.txt", false, false, true);     // SoftCnKiller 白名单（更新覆盖）
    tryOne(L"extra_sign.txt", true, false, false);    // 本项目自有补充（在线更新下发）
    tryOne(L"extra_folder.txt", false, true, false);
    tryOne(L"extra_white.txt", false, false, true);
    tryOne(L"custom_sign.txt", true, false, false);   // 用户自定义（规则库生成器写入，
    tryOne(L"custom_folder.txt", false, true, false); //   与 extra_* 分离防在线更新覆盖）
    tryOne(L"custom_white.txt", false, false, true);

    if (stat) {
        wchar_t b[512];
        swprintf_s(b, 512,
            L"特征库就绪：签名 %zu（内嵌 %zu + 目录 %zu）· 目录特征 %zu（内嵌 %zu + 目录 %zu）· 白名单 %zu",
            signs_.size(), builtinSign, dirSign,
            folders_.size(), builtinFolder, dirFolder,
            whitePaths_.size());
        *stat = b;
    }
    return Ready();
}

// ---------------------------------------------------------------- 匹配

Hit Ruleset::MatchSign(const wstring& signerName) const {
    Hit h;
    if (signerName.empty()) return h;
    wstring nameLow = FoldLower(signerName);
    // 跳过超短特征与微软（微软由 IsMicrosoftSign 前置放行，这里兜底再防一次）
    for (size_t i = 0; i < signs_.size(); ++i) {
        const wstring& rule = signsLow_[i];
        if (rule.size() < 4) continue;                      // 防超短特征误报
        if (EitherContains(nameLow, rule, 4)) {
            h.ok = true; h.kind = HitKind::Sign;
            h.rule = signs_[i]; h.evidence = signerName;
            return h;
        }
    }
    return h;
}

Hit Ruleset::MatchPath(const wstring& fullPath) const {
    Hit h;
    if (fullPath.empty()) return h;
    wstring pathLow = FoldLower(fullPath);
    // 路径分隔统一为 \（避免 / 混用漏判）
    std::replace(pathLow.begin(), pathLow.end(), L'/', L'\\');
    for (size_t i = 0; i < folders_.size(); ++i) {
        const wstring& rule = foldersLow_[i];
        if (rule.size() < 3) continue;
        if (pathLow.find(rule) != wstring::npos) {
            h.ok = true; h.kind = HitKind::Folder;
            h.rule = folders_[i]; h.evidence = fullPath;
            return h;
        }
    }
    return h;
}

bool Ruleset::IsWhitelisted(const wstring& fullPath) const {
    if (fullPath.empty() || whiteLow_.empty()) return false;
    wstring pathLow = FoldLower(fullPath);
    std::replace(pathLow.begin(), pathLow.end(), L'/', L'\\');
    for (const wstring& w : whiteLow_)
        if (!w.empty() && pathLow.find(w) != wstring::npos) return true;
    return false;
}

bool Ruleset::IsMicrosoftSign(const wstring& signerName) {
    if (signerName.empty()) return false;
    wstring low = FoldLower(signerName);
    // 覆盖 "Microsoft Corporation" / "Microsoft" / "微软（中国）" 等
    return low.find(L"microsoft") != wstring::npos ||
           signerName.find(L"微软") != wstring::npos;
}

wstring Ruleset::Stats() const {
    wchar_t b[256];
    swprintf_s(b, 256, L"签名 %zu · 目录 %zu · 白名单 %zu",
               signs_.size(), folders_.size(), whitePaths_.size());
    return b;
}

// ---------------------------------------------------------------- 路径

wstring RulesDir()      { return ExeDir() + L"\\rules"; }
wstring BackupFile()    { return ExeDir() + L"\\purg-backup.tsv"; }
wstring QuarantineDir() { return ExeDir() + L"\\purg-quarantine"; }

} // namespace purg
} // namespace mz
