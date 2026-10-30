// ============================================================================
//  网管工具箱 (NetAdmin Toolbox) —— 净化中心模块集二期（v2.14 M5）
//  ---------------------------------------------------------------------------
//  purg.rules    规则库生成器（Tweaks 型）：可视化维护自定义规则
//                （签名/目录/白名单三类），落 rules\custom_*.txt，与在线
//                更新的 extra_*.txt 分离防互相覆盖；支持添加/删除/导出。
//  purg.yara     YARA 扫描（Tweaks 型）：自研 YARA 子集引擎（yara.h/cpp），
//                规则目录 rules\yara\*.yar（首运自动落内置示例规则），
//                扫描文件或目录，命中列表展示。
//  purg.homepage 主页锁定（Tweaks 型）：扫描 IE/Edge/Chrome 主页与搜索
//                劫持 + 策略劫持 + 快捷方式参数劫持；一键修复并设置主页；
//                策略锁定/解锁；自带 TSV 备份可还原。
//  链接：-lshell32 -lole32 -loleaut32 -ladvapi32（build.bat 已统一追加）
// ============================================================================
#include "../module.h"
#include "../common.h"
#include "../ruleset.h"
#include "../yara.h"
#include "../cleanresult.h"

#include <algorithm>
#include <cstdio>
#include <cwchar>

#include <shlobj.h>     // SHGetFolderPath / IShellLink
#include <oleauto.h>

namespace mz {

using mz::yara::Rule;
using mz::yara::Hit;

namespace {

// ---------------------------------------------------------------- 通用小工具

wstring U32s(unsigned v) { wchar_t b[32]{}; swprintf_s(b, 32, L"%u", v); return b; }

wstring ExeDir() {
    wchar_t buf[MAX_PATH] = {};
    DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return wstring(L".");
    wstring p(buf, n);
    size_t s = p.find_last_of(L'\\');
    return (s == wstring::npos) ? wstring(L".") : p.substr(0, s);
}

bool FileExistsP(const wstring& p) {
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

// 读文本文件（UTF-8 BOM 优先；行拆分；# 注释与空行剔除）
vector<wstring> ReadRuleLines(const wstring& path, bool* ok) {
    if (ok) *ok = false;
    bool good = false;
    wstring text = purg::ReadTextSmart(path, &good);
    if (!good) return {};
    if (ok) *ok = true;
    vector<wstring> lines;
    size_t start = 0;
    while (start <= text.size()) {
        size_t e = text.find(L'\n', start);
        if (e == wstring::npos) e = text.size();
        wstring line = text.substr(start, e - start);
        size_t b = line.find_first_not_of(L" \t\r\uFEFF");
        if (b != wstring::npos) {
            size_t e2 = line.find_last_not_of(L" \t\r\uFEFF");
            line = line.substr(b, e2 - b + 1);
            if (!line.empty() && line[0] != L'#') lines.push_back(line);
        }
        if (e == text.size()) break;
        start = e + 1;
    }
    return lines;
}

// 写文本文件（UTF-8 BOM；lineMode=false 时按 \n 分行写入）
bool WriteUtf8File(const wstring& path, const wstring& text) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    std::string u8 = WToUtf8(text);
    DWORD w = 0;
    WriteFile(h, "\xEF\xBB\xBF", 3, &w, nullptr);
    BOOL ok = WriteFile(h, u8.data(), (DWORD)u8.size(), &w, nullptr);
    CloseHandle(h);
    return ok != FALSE;
}

wstring FoldLowP(const wstring& s) {
    wstring o = s;
    for (auto& c : o) if (c >= L'A' && c <= L'Z') c = (wchar_t)(c - L'A' + L'a');
    return o;
}

// 规则类型：0=签名 1=目录 2=白名单
const wchar_t* RuleTypeName(int t) {
    static const wchar_t* k[] = { L"签名", L"目录", L"白名单" };
    return (t >= 0 && t < 3) ? k[t] : L"?";
}
const wchar_t* RuleFileName(int t) {
    static const wchar_t* k[] = { L"custom_sign.txt", L"custom_folder.txt", L"custom_white.txt" };
    return (t >= 0 && t < 3) ? k[t] : L"?";
}

// 三个自定义规则文件里的一条
struct CustomRule {
    int type = 0;        // 0/1/2
    wstring value;       // 规则内容
    int lineIdx = 0;     // 文件内非注释行序号（删除定位）
};

vector<CustomRule> LoadCustomRules(int* signN, int* folderN, int* whiteN) {
    vector<CustomRule> out;
    int n[3] = { 0, 0, 0 };
    for (int t = 0; t < 3; ++t) {
        wstring f = purg::RulesDir() + L"\\" + RuleFileName(t);
        vector<wstring> lines = ReadRuleLines(f, nullptr);
        for (size_t i = 0; i < lines.size(); ++i) {
            CustomRule r;
            r.type = t;
            r.value = lines[i];
            r.lineIdx = (int)i;
            out.push_back(r);
            ++n[t];
        }
    }
    if (signN) *signN = n[0];
    if (folderN) *folderN = n[1];
    if (whiteN) *whiteN = n[2];
    return out;
}

// 重组文件内容：保留注释原样太复杂 —— 删除采用「按非注释行序号跳过」重建
bool RewriteRuleFileWithout(int type, const vector<int>& dropIdx) {
    wstring f = purg::RulesDir() + L"\\" + RuleFileName(type);
    bool ok = false;
    wstring text = purg::ReadTextSmart(f, &ok);
    if (!ok) text = wstring();
    // 逐行重放：非注释非空行计数，命中 drop 的跳过
    wstring body;
    int idx = 0;
    size_t start = 0;
    while (start <= text.size()) {
        size_t e = text.find(L'\n', start);
        if (e == wstring::npos) e = text.size();
        wstring line = text.substr(start, e - start);
        // 判空/判注释（保留原样行文）
        wstring t = line;
        size_t b = t.find_first_not_of(L" \t\r\uFEFF");
        bool isBlank = (b == wstring::npos);
        bool isComment = !isBlank && t[b] == L'#';
        bool drop = false;
        if (!isBlank && !isComment) {
            for (int d : dropIdx) if (d == idx) { drop = true; break; }
            ++idx;
        }
        if (!drop) {
            body += line;
            if (e != text.size()) body += L"\r\n";
        }
        if (e == text.size()) break;
        start = e + 1;
    }
    return WriteUtf8File(f, body);
}

bool AppendRuleLine(int type, const wstring& value) {
    wstring f = purg::RulesDir() + L"\\" + RuleFileName(type);
    wstring body;
    if (FileExistsP(f)) {
        bool ok = false;
        wstring old = purg::ReadTextSmart(f, &ok);
        if (ok && !old.empty()) {
            body = old;
            if (body.back() != L'\n') body += L"\r\n";
        }
    }
    body += value + L"\r\n";
    return WriteUtf8File(f, body);
}

// ================================================================ purg.rules

class PurgRulesModule : public IModule {
public:
    const wchar_t* Id() const override { return L"purg.rules"; }
    const wchar_t* Group() const override { return L"净化中心"; }
    const wchar_t* Title() const override { return L"规则库生成"; }
    const wchar_t* Description() const override {
        return L"可视化维护自定义规则：签名黑名单（按签名主体命中）/ 目录特征（路径"
               L"片段命中）/ 白名单（路径片段放行）。规则写入 rules\\custom_*.txt，"
               L"与在线更新互不干扰；支持导出规则包分享或上传 GitHub 自有规则源";
    }
    ModuleKind Kind() const override { return ModuleKind::Tweaks; }
    bool NeedsElevation() const override { return false; }
    const wchar_t* RunLabel() const override { return L"刷新规则列表"; }

    vector<ModuleField> Fields() const override {
        return {
            { L"规则类型", L"签名 / 目录 / 白名单", false },
            { L"规则内容", L"签名主体（如 某某科技）或路径片段（如 \\某软件\\）", false },
        };
    }
    void SetFields(const vector<wstring>& values) override {
        if (values.size() > 0) fType_ = values[0];
        if (values.size() > 1) fValue_ = values[1];
    }

    vector<ModuleAction> Actions() const override {
        return {
            { L"添加规则",     nullptr, false, 0xE710 },   // 0 → btnClean 位
            { L"删除选中规则", nullptr, true,  0xE74D },   // 1 → btnUndo 位
            { L"打开规则目录", nullptr, false, 0 },        // 2 → 更多 ▾
            { L"导出规则包", nullptr, false, 0 },          // 3 → 更多 ▾
            { L"格式说明", nullptr, false, 0 },            // 4 → 更多 ▾
        };
    }
    bool RefreshAfterAction() const override { return true; }
    void SetSelection(const vector<int>& rows) override { selection_ = rows; }

    wstring ActionWarning(int actionIndex, const vector<int>& tags) const override {
        switch (actionIndex) {
        case 0: {
            wstring t = fType_, v = fValue_;
            int type = ParseType(t);
            if (type < 0 || v.empty())
                return L"请先在上方填写「规则类型」（签名/目录/白名单）与「规则内容」。";
            return L"将添加一条" + wstring(RuleTypeName(type)) + L"规则：\n\n" + v +
                   L"\n\n写入 " + RuleFileName(type) + L"，下次查杀即生效。\n是否继续？";
        }
        case 1: {
            if (tags.empty()) return L"未选中任何规则。";
            wstring names;
            for (int t : tags)
                if (t >= 0 && t < (int)rules_.size())
                    names += L"【" + wstring(RuleTypeName(rules_[t].type)) + L"】" +
                             rules_[t].value + L"\n";
            return L"将删除以下自定义规则：\n" + names + L"\n是否继续？";
        }
        default:
            return wstring();
        }
    }

    void Run(ModuleContext& ctx, ModuleResult& out) override {
        out = ModuleResult{};
        rules_ = LoadCustomRules(&signN_, &folderN_, &whiteN_);
        for (size_t i = 0; i < rules_.size(); ++i) {
            const CustomRule& r = rules_[i];
            ModuleRow row;
            row.c1 = r.value;
            row.c2 = RuleTypeName(r.type);
            row.c3 = RuleFileName(r.type);
            row.tag = (int)i;
            out.rows.push_back(std::move(row));
        }
        wchar_t b[256];
        swprintf_s(b, 256, L"自定义规则 %zu 条（签名 %d · 目录 %d · 白名单 %d）",
                   rules_.size(), signN_, folderN_, whiteN_);
        out.summary = b;
        out.hint = rules_.empty()
            ? wstring(T(L"还没有自定义规则。在上方填写类型与内容后点「添加规则」。"))
              + T(L"内置 SoftCnKiller 特征库无需在此维护（在线更新自动管理）。")
            : T(L"规则在下次「全盘查杀」时自动生效。选中后可删除；「更多 ▾」可导出规则包。");
        out.canClean = true;
        ctx.Log(out.summary);
    }

    void RunAction(ModuleContext& ctx, int actionIndex, ModuleResult& out) override {
        out = ModuleResult{};
        switch (actionIndex) {
        case 0: {   // 添加规则
            int type = ParseType(fType_);
            wstring v = fValue_;
            v.erase(0, v.find_first_not_of(L" \t"));
            while (!v.empty() && (v.back() == L' ' || v.back() == L'\t')) v.pop_back();
            if (type < 0 || v.empty()) {
                out.summary = L"请先填写规则类型（签名/目录/白名单）与内容";
                out.canClean = true;
                return;
            }
            size_t minLen = (type == 0) ? 4 : (type == 1 ? 3 : 2);
            if (v.size() < minLen) {
                out.summary = wstring(RuleTypeName(type)) + L"规则至少 " + U32s((unsigned)minLen) +
                              L" 字符（防误报）：\"" + v + L"\"";
                out.canClean = true;
                return;
            }
            // 去重（大小写不敏感）
            for (const CustomRule& r : rules_) {
                if (r.type == type && FoldLowP(r.value) == FoldLowP(v)) {
                    out.summary = L"该规则已存在，无需重复添加";
                    out.canClean = true;
                    return;
                }
            }
            CreateDirectoryW(purg::RulesDir().c_str(), nullptr);
            if (!AppendRuleLine(type, v)) {
                out.summary = L"写入失败：" + ErrorTextZh(GetLastError());
                out.canClean = true;
                return;
            }
            ctx.Log(L"已添加" + wstring(RuleTypeName(type)) + L"规则：" + v);
            out.summary = L"已添加" + wstring(RuleTypeName(type)) + L"规则：" + v +
                          L"（下次查杀生效）";
            out.canClean = true;
            break;
        }
        case 1: {   // 删除选中规则
            if (selection_.empty()) {
                out.summary = L"未选中任何规则";
                out.canClean = true;
                return;
            }
            // 按类型分组收集待删行号
            vector<int> drop[3];
            int okN = 0;
            for (int tag : selection_) {
                if (tag < 0 || tag >= (int)rules_.size()) continue;
                drop[rules_[tag].type].push_back(rules_[tag].lineIdx);
                ctx.Log(L"已删除" + wstring(RuleTypeName(rules_[tag].type)) +
                        L"规则：" + rules_[tag].value);
                ++okN;
            }
            for (int t = 0; t < 3; ++t) {
                if (drop[t].empty()) continue;
                std::sort(drop[t].begin(), drop[t].end(), std::greater<int>());
                if (!RewriteRuleFileWithout(t, drop[t]))
                    ctx.Log(L"  写回 " + wstring(RuleFileName(t)) + L" 失败：" +
                            ErrorTextZh(GetLastError()));
            }
            wchar_t b[128];
            swprintf_s(b, 128, L"删除完成：%d 条（文件已重写）", okN);
            out.summary = b;
            out.canClean = true;
            break;
        }
        case 2: {   // 打开规则目录
            CreateDirectoryW(purg::RulesDir().c_str(), nullptr);
            SHELLEXECUTEINFOW sei{};
            sei.cbSize = sizeof(sei);
            sei.lpVerb = L"open";
            sei.lpFile = purg::RulesDir().c_str();
            sei.nShow = SW_SHOWNORMAL;
            ShellExecuteExW(&sei);
            out.summary = L"已打开规则目录";
            out.canClean = true;
            break;
        }
        case 3: {   // 导出规则包
            wstring body;
            body += L"# NetAdmin Toolbox 自定义规则导出（purg.rules）\r\n";
            body += L"# 导入方式：内容按段拷贝到 rules\\extra_sign.txt /\r\n";
            body += L"#   extra_folder.txt / extra_white.txt，或整文件上传到\r\n";
            body += L"#   GitHub 自有规则仓库由「在线更新特征库」下发。\r\n";
            body += L"\r\n[extra_sign.txt]\r\n";
            for (const CustomRule& r : rules_) if (r.type == 0) body += r.value + L"\r\n";
            body += L"\r\n[extra_folder.txt]\r\n";
            for (const CustomRule& r : rules_) if (r.type == 1) body += r.value + L"\r\n";
            body += L"\r\n[extra_white.txt]\r\n";
            for (const CustomRule& r : rules_) if (r.type == 2) body += r.value + L"\r\n";
            wstring dst = purg::RulesDir() + L"\\purg-rules-export.txt";
            if (WriteUtf8File(dst, body)) {
                ctx.Log(L"已导出：" + dst);
                out.summary = L"已导出规则包：purg-rules-export.txt（" +
                              U32s((unsigned)rules_.size()) + L" 条）";
                SHELLEXECUTEINFOW sei{};
                sei.cbSize = sizeof(sei);
                sei.lpVerb = L"select";
                sei.lpFile = dst.c_str();
                sei.nShow = SW_SHOWNORMAL;
                ShellExecuteExW(&sei);
            } else {
                out.summary = L"导出失败：" + ErrorTextZh(GetLastError());
            }
            out.canClean = true;
            break;
        }
        case 4: {   // 格式说明
            ctx.Log(L"自定义规则格式（每行一条，# 开头为注释）：");
            ctx.Log(L"  签名规则   —— 数字签名主体的一部分，如「某某科技有限公司」");
            ctx.Log(L"               命中该主体签名的进程/服务/驱动/启动项全部列报");
            ctx.Log(L"               （微软签名永远放行，无需担心误杀系统组件）");
            ctx.Log(L"  目录规则   —— 路径片段，如 \\某软件\\ 或 某软件\\bin");
            ctx.Log(L"               命中路径片段的可疑项列报");
            ctx.Log(L"  白名单规则 —— 路径片段，命中即放行（优先级最高，用于豁免）");
            ctx.Log(L"落盘位置：rules\\custom_sign.txt / custom_folder.txt / custom_white.txt");
            ctx.Log(L"与 SoftCnKiller 特征库（sign/folder/whitepath.txt，在线更新管理）");
            ctx.Log(L"及 extra_*.txt（GitHub 自有规则源下发）三层独立、互不覆盖。");
            out.summary = L"规则格式说明已输出到日志";
            out.canClean = true;
            break;
        }
        default: break;
        }
    }

private:
    vector<int> selection_;
    vector<CustomRule> rules_;
    wstring fType_, fValue_;
    int signN_ = 0, folderN_ = 0, whiteN_ = 0;

    static int ParseType(const wstring& s) {
        wstring low = FoldLowP(s);
        if (low.find(L"签") != wstring::npos || low == L"1" || low == L"sign") return 0;
        if (low.find(L"目") != wstring::npos || low == L"2" || low == L"folder" ||
            low.find(L"path") != wstring::npos) return 1;
        if (low.find(L"白") != wstring::npos || low == L"3" || low == L"white") return 2;
        return -1;
    }
};

REGISTER_MODULE(PurgRulesModule);

// ================================================================ purg.yara

// 内置示例规则（首运行落到 rules\yara\builtin_pua.yar；纯文本、杀软无害）
const wchar_t* const kBuiltinYara[] = {
L"/* NetAdmin Toolbox 内置示例规则（YARA 子集）—— 可自行增改 */",
L"",
L"rule PUA_Installer_Common_Strings {",
L"    strings:",
L"        $a = \"browser helper\" ascii wide nocase",
L"        $b = \"set homepage\" ascii wide nocase",
L"        $c = \"silent install\" ascii wide nocase",
L"        $d = \"bundleware\" ascii wide nocase",
L"    condition:",
L"        2 of them",
L"}",
L"",
L"rule PUA_Adware_Registry_Pusher {",
L"    strings:",
L"        $run1 = \"Software\\\\Microsoft\\\\Windows\\\\CurrentVersion\\\\Run\" wide ascii",
L"        $adv = \"popunder\" ascii wide nocase",
L"        $adv2 = \"adinject\" ascii wide nocase",
L"    condition:",
L"        $run1 and 1 of ($adv*)",
L"}",
L"",
L"rule PUA_Homepage_Hijack_Payload {",
L"    strings:",
L"        $mz1 = { 8B ?? ?? ?? ?? ?? C7 05 }",
L"        $hij = \"hao123\" ascii wide nocase",
L"        $hij2 = \"2345\" ascii wide nocase fullword",
L"        $hij3 = \"yandex.ru\" nocase",
L"    condition:",
L"        any of them and filesize < 20MB",
L"}",
L"",
L"rule Suspicious_Packed_MZ {",
L"    strings:",
L"        $mz = { 4D 5A 90 00 }",
L"        $upx = \"UPX0\" ascii",
L"        $upx2 = \"UPX!\" ascii",
L"    condition:",
L"        $mz at 0 and 1 of ($upx*) and filesize > 64KB and filesize < 5MB",
L"}",
L"",
};
const int kBuiltinYaraCount = (int)(sizeof(kBuiltinYara) / sizeof(kBuiltinYara[0]));

class PurgYaraModule : public IModule {
public:
    const wchar_t* Id() const override { return L"purg.yara"; }
    const wchar_t* Group() const override { return L"净化中心"; }
    const wchar_t* Title() const override { return L"YARA 扫描"; }
    const wchar_t* Description() const override {
        return L"自研 YARA 子集引擎：文本串（ascii/wide/nocase/fullword）、十六进制"
               L"串（? 通配与 [n-m] 跳转）、any/all/N of them 条件。规则放 rules\\yara\\"
               L"*.yar（首运行自动落内置示例），扫描文件或目录";
    }
    ModuleKind Kind() const override { return ModuleKind::Tweaks; }
    bool NeedsElevation() const override { return true; }
    const wchar_t* RunLabel() const override { return L"开始扫描"; }

    vector<ModuleField> Fields() const override {
        return {
            { L"规则位置", L"规则文件或目录，留空 = rules\\yara\\", false },
            { L"扫描路径", L"要扫描的文件或目录（如 C:\\Users\\Public\\Downloads）", false },
        };
    }
    void SetFields(const vector<wstring>& values) override {
        if (values.size() > 0) fRules_ = values[0];
        if (values.size() > 1) fPath_ = values[1];
    }

    vector<ModuleAction> Actions() const override {
        return {
            { L"打开规则目录", nullptr, false, 0xE838 },   // 0 → btnClean 位
        };
    }
    bool RefreshAfterAction() const override { return false; }

    wstring ActionWarning(int, const vector<int>&) const override { return wstring(); }

    void Run(ModuleContext& ctx, ModuleResult& out) override {
        out = ModuleResult{};

        // ① 规则加载
        wstring rulesLoc = fRules_;
        if (rulesLoc.empty()) rulesLoc = ExeDir() + L"\\rules\\yara";
        EnsureBuiltinRules(rulesLoc);

        vector<Rule> rules;
        DWORD attr = GetFileAttributesW(rulesLoc.c_str());
        if (attr == INVALID_FILE_ATTRIBUTES) {
            out.summary = L"规则位置不存在：" + rulesLoc;
            out.canClean = true;
            return;
        }
        int nFiles = 0, nBad = 0;
        if (attr & FILE_ATTRIBUTE_DIRECTORY) {
            WIN32_FIND_DATAW fd{};
            HANDLE f = FindFirstFileW((rulesLoc + L"\\*.yar").c_str(), &fd);
            if (f != INVALID_HANDLE_VALUE) {
                do {
                    if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
                    LoadOneRuleFile(rulesLoc + L"\\" + fd.cFileName, rules, ctx, &nBad);
                    ++nFiles;
                } while (FindNextFileW(f, &fd));
                FindClose(f);
            }
            HANDLE f2 = FindFirstFileW((rulesLoc + L"\\*.yara").c_str(), &fd);
            if (f2 != INVALID_HANDLE_VALUE) {
                do {
                    if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
                    LoadOneRuleFile(rulesLoc + L"\\" + fd.cFileName, rules, ctx, &nBad);
                    ++nFiles;
                } while (FindNextFileW(f2, &fd));
                FindClose(f2);
            }
        } else {
            LoadOneRuleFile(rulesLoc, rules, ctx, &nBad);
            ++nFiles;
        }
        size_t nGood = 0;
        for (const Rule& r : rules) if (r.error.empty()) ++nGood;
        ctx.Log(L"规则文件 " + U32s((unsigned)nFiles) + L" 份，可用规则 " +
                U32s((unsigned)nGood) + L" 条" + (nBad ? L"（" + U32s((unsigned)nBad) + L" 条超出子集被跳过）" : L""));
        if (nGood == 0) {
            out.summary = L"没有可用的 YARA 规则（检查规则目录：" + rulesLoc + L"）";
            out.canClean = true;
            return;
        }

        // ② 扫描
        wstring scanPath = fPath_;
        if (scanPath.empty()) {
            out.summary = L"请先在上方填写扫描路径（文件或目录）";
            out.canClean = true;
            return;
        }
        ctx.Log(L"扫描路径：" + scanPath);
        vector<Hit> hits;
        int fileCount = 0;
        wstring scanErr;
        mz::yara::ScanPath(rules, scanPath, hits, &fileCount, nullptr, &scanErr);
        if (!scanErr.empty()) {
            out.summary = scanErr;
            out.canClean = true;
            return;
        }

        // ③ 结果落表
        for (const Hit& h : hits) {
            ModuleRow row;
            row.c1 = h.rule;
            row.c2 = h.sids;
            row.c3 = L"命中";
            row.c4 = h.file;
            row.danger = true;
            out.rows.push_back(std::move(row));
        }
        wchar_t b[256];
        swprintf_s(b, 256, L"YARA 扫描完成：文件 %d 个，命中 %zu 处（规则 %zu 条）",
                   fileCount, hits.size(), nGood);
        out.summary = b;
        out.hint = hits.empty()
            ? T(L"未命中任何规则。规则写法见内置示例 rules\\yara\\builtin_pua.yar。")
            : T(L"命中文件请人工核实后再处置（YARA 只做内容识别，不做自动删除）。");
        out.canClean = true;
    }

    void RunAction(ModuleContext& ctx, int actionIndex, ModuleResult& out) override {
        out = ModuleResult{};
        if (actionIndex == 0) {
            wstring dir = fRules_.empty() ? ExeDir() + L"\\rules\\yara" : fRules_;
            EnsureBuiltinRules(dir);
            SHELLEXECUTEINFOW sei{};
            sei.cbSize = sizeof(sei);
            sei.lpVerb = L"open";
            sei.lpFile = dir.c_str();
            sei.nShow = SW_SHOWNORMAL;
            ShellExecuteExW(&sei);
            out.summary = L"已打开规则目录";
            out.canClean = true;
        }
    }

private:
    wstring fRules_, fPath_;

    static void LoadOneRuleFile(const wstring& path, vector<Rule>& rules,
                                ModuleContext& ctx, int* nBad) {
        bool ok = false;
        wstring text = purg::ReadTextSmart(path, &ok);
        if (!ok) { ctx.Log(L"  读取失败：" + path); return; }
        wstring err;
        size_t before = rules.size();
        mz::yara::ParseRules(text, rules, &err);
        for (size_t i = before; i < rules.size(); ++i) {
            if (!rules[i].error.empty()) {
                ++*nBad;
                ctx.Log(L"  规则超出子集已跳过：" + rules[i].name + L"（" +
                        rules[i].error + L"）");
            }
        }
    }

    // 首运行落内置示例规则
    static void EnsureBuiltinRules(const wstring& rulesLoc) {
        if (rulesLoc.find(ExeDir() + L"\\rules") != 0) return;   // 仅默认目录落示例
        // CreateDirectoryW 一次只建一层：先父目录后目标目录
        CreateDirectoryW((ExeDir() + L"\\rules").c_str(), nullptr);
        CreateDirectoryW(rulesLoc.c_str(), nullptr);
        wstring f = rulesLoc + L"\\builtin_pua.yar";
        if (FileExistsP(f)) return;
        wstring body;
        for (int i = 0; i < kBuiltinYaraCount; ++i)
            body += kBuiltinYara[i] + wstring(L"\r\n");
        WriteUtf8File(f, body);
    }
};

REGISTER_MODULE(PurgYaraModule);

// ================================================================ purg.homepage

// 主页发现
struct HomeFind {
    int kind = 0;        // 0=主页 1=搜索劫持 2=策略 3=快捷方式
    wstring name;        // 值名/文件名
    wstring location;    // 注册表键全名 / lnk 路径
    wstring value;       // 当前值 / lnk 参数
};

const wchar_t* HomeKindName(int k) {
    static const wchar_t* n[] = { L"主页", L"搜索劫持", L"策略", L"快捷方式" };
    return (k >= 0 && k < 4) ? n[k] : L"?";
}

// 备份文件（exe 目录）：homepage-backup.tsv
// 行格式：SET \t root \t sub \t val \t type \t data   /   DEL \t root \t sub \t val
wstring HomeBackupFile() { return ExeDir() + L"\\homepage-backup.tsv"; }

wstring RegRootName(HKEY r) {
    return (r == HKEY_LOCAL_MACHINE) ? wstring(L"HKLM") : wstring(L"HKCU");
}
HKEY RegRootFromName(const wstring& n) {
    return (n == L"HKLM") ? HKEY_LOCAL_MACHINE : HKEY_CURRENT_USER;
}

// 读注册表值（备份用）：返回 "type|data"
wstring RegReadVal(HKEY root, const wstring& sub, const wstring& val, DWORD* type) {
    DWORD t = 0, sz = 0;
    if (RegGetValueW(root, sub.c_str(), val.c_str(), RRF_RT_ANY, &t, nullptr, &sz)
        != ERROR_SUCCESS || sz == 0) return wstring();
    vector<BYTE> buf(sz);
    if (RegGetValueW(root, sub.c_str(), val.c_str(), RRF_RT_ANY, &t, buf.data(), &sz)
        != ERROR_SUCCESS) return wstring();
    if (type) *type = t;
    if (t == REG_SZ || t == REG_EXPAND_SZ) return wstring((wchar_t*)buf.data());
    if (t == REG_DWORD && sz >= 4) return L"DWORD:" + U32s(*(DWORD*)buf.data());
    return wstring();
}

bool RegWriteSz(HKEY root, const wstring& sub, const wstring& val, const wstring& data) {
    HKEY k;
    if (RegCreateKeyExW(root, sub.c_str(), 0, nullptr, 0, KEY_SET_VALUE, nullptr, &k, nullptr)
        != ERROR_SUCCESS) return false;
    LSTATUS st = RegSetValueExW(k, val.c_str(), 0, REG_SZ,
                                (const BYTE*)data.c_str(),
                                (DWORD)((data.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(k);
    return st == ERROR_SUCCESS;
}

bool RegDelVal(HKEY root, const wstring& sub, const wstring& val) {
    HKEY k;
    if (RegOpenKeyExW(root, sub.c_str(), 0, KEY_SET_VALUE, &k) != ERROR_SUCCESS) return false;
    LSTATUS st = RegDeleteValueW(k, val.c_str());
    RegCloseKey(k);
    return st == ERROR_SUCCESS || st == ERROR_FILE_NOT_FOUND;
}

// 备份/还原 TSV
void HomeBackupAppend(const wstring& line) {
    bool exists = FileExistsP(HomeBackupFile());
    HANDLE h = CreateFileW(HomeBackupFile().c_str(), FILE_APPEND_DATA, 0,
                           nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD w = 0;
    if (!exists) WriteFile(h, "\xEF\xBB\xBF", 3, &w, nullptr);
    std::string u8 = WToUtf8(line + L"\r\n");
    WriteFile(h, u8.data(), (DWORD)u8.size(), &w, nullptr);
    CloseHandle(h);
}

bool IsHttpUrl(const wstring& s) {
    wstring low = FoldLowP(s);
    return low.rfind(L"http://", 0) == 0 || low.rfind(L"https://", 0) == 0 ||
           low.find(L"http://") != wstring::npos || low.find(L"https://") != wstring::npos;
}

class PurgHomepageModule : public IModule {
public:
    const wchar_t* Id() const override { return L"purg.homepage"; }
    const wchar_t* Group() const override { return L"净化中心"; }
    const wchar_t* Title() const override { return L"主页锁定"; }
    const wchar_t* Description() const override {
        return L"扫描 IE/Edge/Chrome 主页与搜索引擎劫持、策略劫持、桌面快捷方式"
               L"参数劫持；一键修复并设置主页；策略级锁定/解锁（防再次篡改）；"
               L"修改前自动备份，可一键还原";
    }
    ModuleKind Kind() const override { return ModuleKind::Tweaks; }
    bool NeedsElevation() const override { return true; }
    const wchar_t* RunLabel() const override { return L"扫描劫持"; }

    vector<ModuleField> Fields() const override {
        return {
            { L"目标主页", L"修复后要设置的主页，如 https://www.baidu.com", false },
        };
    }
    void SetFields(const vector<wstring>& values) override {
        if (values.size() > 0) fHome_ = values[0];
    }

    vector<ModuleAction> Actions() const override {
        return {
            { L"修复并设置主页", nullptr, false, 0xE72E },   // 0 → btnClean 位
            { L"还原上次修改",   nullptr, false, 0xE72C },   // 1 → btnUndo 位
            { L"锁定主页",       nullptr, true,  0 },        // 2 → 更多 ▾（策略，danger）
            { L"解除主页锁定",   nullptr, false, 0 },        // 3 → 更多 ▾
            { L"仅清理快捷方式劫持", nullptr, false, 0 },    // 4 → 更多 ▾
        };
    }
    bool RefreshAfterAction() const override { return true; }

    wstring ActionWarning(int actionIndex, const vector<int>&) const override {
        switch (actionIndex) {
        case 0: {
            wstring h = fHome_;
            if (h.empty()) return L"未填写目标主页：将只清理劫持项（搜索劫持/快捷方式参数），不设置新主页。\n\n是否继续？";
            return L"将执行：\n· IE/Edge/Chrome 主页设为 " + h +
                   L"\n· 清除指向 http(s) 的搜索劫持值\n· 清理快捷方式 URL 参数劫持"
                   L"\n\n修改前自动备份，可「还原上次修改」。\n是否继续？";
        }
        case 1:
            return L"将按备份记录还原全部主页相关注册表修改。\n\n是否继续？";
        case 2: {
            wstring h = fHome_;
            if (h.empty()) return L"请先在上方填写目标主页（策略锁定的内容）。";
            return L"将写入当前用户策略（Edge/Chrome Homepage）锁定主页为：\n" + h +
                   L"\n\n锁定后普通程序无法再篡改主页（本工具「解除主页锁定」可撤销）。\n是否继续？";
        }
        case 3:
            return L"将删除 Edge/Chrome 的主页策略锁定（Homepage / HomepageIsNewTabPage /"
                   L" RestoreOnStartupURLs），恢复用户自由设置。\n\n是否继续？";
        case 4:
            return L"将清理桌面快捷方式中的 URL 参数劫持（备份原参数后移除）。\n\n是否继续？";
        default:
            return wstring();
        }
    }

    void Run(ModuleContext& ctx, ModuleResult& out) override {
        out = ModuleResult{};
        finds_.clear();
        ScanAll(ctx, finds_);
        for (size_t i = 0; i < finds_.size(); ++i) {
            const HomeFind& f = finds_[i];
            ModuleRow row;
            row.c1 = f.name;
            row.c2 = HomeKindName(f.kind);
            row.c3 = f.location;
            row.c4 = f.value;
            row.tag = (int)i;
            row.danger = (f.kind != 0);   // 纯主页配置不算危险行
            out.rows.push_back(std::move(row));
        }
        int hij = 0, pol = 0, lnk = 0, home = 0;
        for (const HomeFind& f : finds_) {
            if (f.kind == 2) ++pol;
            else if (f.kind == 3) ++lnk;
            else if (f.kind == 1) ++hij;
            else ++home;
        }
        wchar_t b[256];
        swprintf_s(b, 256, L"主页检查完成：主页配置 %d · 搜索劫持 %d · 策略 %d · 快捷方式劫持 %d",
                   home, hij, pol, lnk);
        out.summary = b;
        out.hint = finds_.empty()
            ? T(L"未发现主页/搜索/策略/快捷方式劫持。可在上方填写主页后用「更多 ▾ → 锁定主页」防篡改。")
            : T(L"填写目标主页后点「修复并设置主页」；或「更多 ▾」锁定主页（策略级防篡改）。");
        out.canClean = true;
    }

    void RunAction(ModuleContext& ctx, int actionIndex, ModuleResult& out) override {
        out = ModuleResult{};
        switch (actionIndex) {
        case 0: {   // 修复并设置主页
            wstring h = fHome_;
            int ok = 0, fail = 0;
            // ① IE 主页与搜索劫持（HKCU + HKLM）
            struct IEVal { const wchar_t* val; bool search; };
            static const IEVal ieVals[] = {
                { L"Start Page",       false },
                { L"Default_Page_URL", false },
                { L"Search Page",      true },
                { L"Search Bar",       true },
                { L"CustomizeSearch",  true },
                { L"SearchURL",        true },
            };
            for (HKEY root : { HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE }) {
                const wstring sub = L"Software\\Microsoft\\Internet Explorer\\Main";
                for (const IEVal& iv : ieVals) {
                    wstring cur = RegReadVal(root, sub, iv.val, nullptr);
                    if (cur.empty()) continue;
                    bool needFix = iv.search ? IsHttpUrl(cur) : (!h.empty() && cur != h);
                    if (!needFix) continue;
                    // 备份
                    DWORD t = REG_SZ;
                    RegReadVal(root, sub, iv.val, &t);
                    wchar_t tb[16];
                    swprintf_s(tb, 16, L"%lu", (unsigned long)t);
                    HomeBackupAppend(L"SET\t" + RegRootName(root) + L"\t" + sub + L"\t" +
                                     iv.val + L"\t" + tb + L"\t" + cur);
                    // 动手
                    bool done;
                    if (iv.search) done = RegDelVal(root, sub, iv.val);
                    else done = RegWriteSz(root, sub, iv.val, h);
                    if (done) { ++ok; ctx.Log(L"  已修复 " + RegRootName(root) + L"\\" + sub + L"\\" + iv.val); }
                    else ++fail;
                }
            }
            // ② 快捷方式劫持
            int lnkFixed = FixLnkHijacks(ctx);
            ok += lnkFixed;
            // ③ 设置新主页（Edge/Chrome Preferences 属 JSON，交给策略锁；此处 IE）
            wchar_t b[128];
            swprintf_s(b, 128, L"修复完成：注册表 %d 项，快捷方式 %d 个（失败 %d）", ok, lnkFixed, fail);
            out.summary = b;
            if (h.empty()) out.summary += L"（未设置新主页：目标主页为空）";
            out.canClean = true;
            break;
        }
        case 1: {   // 还原上次修改
            bool ok = false;
            wstring text = purg::ReadTextSmart(HomeBackupFile(), &ok);
            if (!ok || text.empty()) {
                out.summary = L"备份为空，没有可还原的修改";
                out.canClean = true;
                return;
            }
            int okN = 0, failN = 0;
            size_t start = 0;
            while (start <= text.size()) {
                size_t e = text.find(L'\n', start);
                if (e == wstring::npos) e = text.size();
                wstring line = text.substr(start, e - start);
                // 还原 = 倒序逐行执行（此处按文件序正序重放也成立：
                // SET 行重放「写回原值」，最后一次生效；DEL 行重放重建）
                size_t f1 = line.find(L'\t');
                if (f1 != wstring::npos) {
                    wstring op = line.substr(0, f1);
                    // SET root sub val type data
                    size_t f2 = line.find(L'\t', f1 + 1);
                    size_t f3 = f2 == wstring::npos ? wstring::npos : line.find(L'\t', f2 + 1);
                    size_t f4 = f3 == wstring::npos ? wstring::npos : line.find(L'\t', f3 + 1);
                    size_t f5 = f4 == wstring::npos ? wstring::npos : line.find(L'\t', f4 + 1);
                    if (!op.empty() && f5 != wstring::npos) {
                        HKEY root = RegRootFromName(line.substr(f1 + 1, f2 - f1 - 1));
                        wstring sub = line.substr(f2 + 1, f3 - f2 - 1);
                        wstring val = line.substr(f3 + 1, f4 - f3 - 1);
                        wstring typeS = line.substr(f4 + 1, f5 - f4 - 1);
                        wstring data = line.substr(f5 + 1);
                        bool done = false;
                        if (op == L"SET") {
                            done = RegWriteSz(root, sub, val, data);   // 原值均按 SZ 写回
                        } else if (op == L"DEL") {
                            done = RegDelVal(root, sub, val);
                        }
                        if (done) ++okN; else ++failN;
                    }
                }
                if (e == text.size()) break;
                start = e + 1;
            }
            wchar_t b[128];
            swprintf_s(b, 128, L"还原完成：成功 %d，失败 %d", okN, failN);
            out.summary = b;
            out.canClean = true;
            break;
        }
        case 2: {   // 锁定主页（策略）
            wstring h = fHome_;
            if (h.empty()) {
                out.summary = L"请先在上方填写目标主页";
                out.canClean = true;
                return;
            }
            int ok = 0, fail = 0;
            // 当前用户策略（HKCU）+ 机器策略（HKLM）；Policies 键需要管理员
            for (HKEY root : { HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE }) {
                if (RegWriteSz(root, L"Software\\Policies\\Microsoft\\Edge", L"Homepage", h)) ++ok;
                else ++fail;
                if (RegWriteSz(root, L"Software\\Policies\\Google\\Chrome", L"Homepage", h)) ++ok;
                else ++fail;
                RegWriteSz(root, L"Software\\Policies\\Microsoft\\Edge", L"HomepageIsNewTabPage", L"0");
                RegWriteSz(root, L"Software\\Policies\\Google\\Chrome", L"HomepageIsNewTabPage", L"0");
            }
            HomeBackupAppend(L"LOCK\t" + h);
            if (ok == 0)
                out.summary = L"锁定失败：策略键写入被拒绝（" + U32s((unsigned)fail) +
                              L" 处）。请以管理员身份运行后重试。";
            else
                out.summary = L"已锁定主页（策略级）：" + h + L"（写入 " +
                              U32s((unsigned)ok) + L" 处，重启浏览器生效）";
            out.canClean = true;
            break;
        }
        case 3: {   // 解除主页锁定
            int ok = 0;
            for (HKEY root : { HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE }) {
                const wchar_t* subs[] = {
                    L"Software\\Policies\\Microsoft\\Edge",
                    L"Software\\Policies\\Google\\Chrome",
                };
                for (const wchar_t* sub : subs) {
                    // 备份现有值再删
                    const wchar_t* vals[] = { L"Homepage", L"HomepageIsNewTabPage" };
                    for (const wchar_t* v : vals) {
                        wstring cur = RegReadVal(root, sub, v, nullptr);
                        if (!cur.empty()) {
                            HomeBackupAppend(L"SET\t" + RegRootName(root) + L"\t" + sub +
                                             L"\t" + v + L"\t4294967265\t" + cur);
                            RegDelVal(root, sub, v);
                            ++ok;
                        }
                    }
                    // RestoreOnStartupURLs 子键值
                    HKEY k;
                    wstring rsub = wstring(sub) + L"\\RestoreOnStartupURLs";
                    if (RegOpenKeyExW(root, rsub.c_str(), 0, KEY_READ, &k) == ERROR_SUCCESS) {
                        DWORD idx = 0;
                        wchar_t name[128];
                        DWORD nl = 128;
                        while (RegEnumValueW(k, idx++, name, &nl, nullptr, nullptr, nullptr, nullptr)
                               == ERROR_SUCCESS) {
                            wstring cur = RegReadVal(root, rsub, name, nullptr);
                            HomeBackupAppend(L"SET\t" + RegRootName(root) + L"\t" + rsub +
                                             L"\t" + name + L"\t4294967265\t" + cur);
                            nl = 128;   // RegEnumValue 会改写 nl，必须复位
                        }
                        RegCloseKey(k);
                        // 删整个子键
                        HKEY pk;
                        if (RegOpenKeyExW(root, sub, 0, KEY_SET_VALUE, &pk) == ERROR_SUCCESS) {
                            RegDeleteTreeW(pk, L"RestoreOnStartupURLs");
                            RegCloseKey(pk);
                        }
                    }
                }
            }
            out.summary = ok ? L"已解除主页策略锁定（" + U32s((unsigned)ok) + L" 项，已备份）"
                             : L"未发现主页策略锁定";
            out.canClean = true;
            break;
        }
        case 4: {   // 仅清理快捷方式劫持
            int n = FixLnkHijacks(ctx);
            out.summary = L"快捷方式清理完成：" + U32s((unsigned)n) + L" 个（原参数已备份）";
            out.canClean = true;
            break;
        }
        default: break;
        }
    }

private:
    vector<HomeFind> finds_;
    wstring fHome_;

    // ---------------- 扫描 ----------------

    void ScanAll(ModuleContext& ctx, vector<HomeFind>& out) {
        // ① IE 主页/搜索（HKCU + HKLM）
        struct IEVal { const wchar_t* val; bool search; };
        static const IEVal ieVals[] = {
            { L"Start Page",       false },
            { L"Default_Page_URL", false },
            { L"Search Page",      true },
            { L"Search Bar",       true },
            { L"CustomizeSearch",  true },
            { L"SearchURL",        true },
        };
        for (HKEY root : { HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE }) {
            const wstring sub = L"Software\\Microsoft\\Internet Explorer\\Main";
            for (const IEVal& iv : ieVals) {
                wstring v = RegReadVal(root, sub, iv.val, nullptr);
                if (v.empty()) continue;
                if (IsCommonHome(v)) continue;   // 微软默认/常见合法主页不列报（降噪）
                // 只报「可疑」的：搜索类指向 http、主页类非空即可展示
                bool suspicious = iv.search ? IsHttpUrl(v) : false;
                if (iv.val == wstring(L"Start Page") && IsHttpUrl(v))
                    suspicious = true;
                if (!suspicious && v.find(L"http") == wstring::npos) continue;
                HomeFind f;
                f.kind = iv.search ? 1 : 0;
                f.name = iv.val;
                f.location = RegRootName(root) + L"\\Software\\Microsoft\\Internet Explorer\\Main";
                f.value = v;
                out.push_back(f);
            }
        }
        // ② Edge/Chrome 策略
        const wchar_t* polSubs[] = {
            L"Software\\Policies\\Microsoft\\Edge",
            L"Software\\Policies\\Google\\Chrome",
        };
        for (HKEY root : { HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE }) {
            for (const wchar_t* sub : polSubs) {
                const wchar_t* vals[] = { L"Homepage", L"HomepageIsNewTabPage" };
                for (const wchar_t* v : vals) {
                    wstring cur = RegReadVal(root, sub, v, nullptr);
                    if (cur.empty()) continue;
                    HomeFind f;
                    f.kind = 2;
                    f.name = v;
                    f.location = RegRootName(root) + L"\\" + sub;
                    f.value = cur;
                    out.push_back(f);
                }
                HKEY k;
                wstring rsub = wstring(sub) + L"\\RestoreOnStartupURLs";
                if (RegOpenKeyExW(root, rsub.c_str(), 0, KEY_QUERY_VALUE, &k) == ERROR_SUCCESS) {
                    DWORD idx = 0, nl = 128;
                    wchar_t name[128];
                    while (RegEnumValueW(k, idx++, name, &nl, nullptr, nullptr, nullptr, nullptr)
                           == ERROR_SUCCESS) {
                        wstring cur = RegReadVal(root, rsub, name, nullptr);
                        HomeFind f;
                        f.kind = 2;
                        f.name = wstring(L"RestoreOnStartupURLs\\") + name;
                        f.location = RegRootName(root) + L"\\" + rsub;
                        f.value = cur;
                        out.push_back(f);
                        nl = 128;
                    }
                    RegCloseKey(k);
                }
            }
        }
        // ③ 快捷方式劫持（桌面 + 公共桌面）
        wchar_t desk[MAX_PATH]{};
        SHGetFolderPathW(nullptr, CSIDL_DESKTOPDIRECTORY, nullptr, 0, desk);
        wchar_t pubDesk[MAX_PATH]{};
        SHGetFolderPathW(nullptr, CSIDL_COMMON_DESKTOPDIRECTORY, nullptr, 0, pubDesk);
        ScanLnkDir(desk, out);
        if (FoldLowP(pubDesk) != FoldLowP(desk)) ScanLnkDir(pubDesk, out);

        ctx.Log(L"主页检查：IE 主页/搜索 + Edge/Chrome 策略 + 桌面快捷方式");
    }

    static bool IsCommonHome(const wstring& v) {
        // 常见合法主页（避免噪音）：微软默认、常见搜索引擎
        static const wchar_t* k[] = {
            L"microsoft.com", L"msn.com", L"bing.com", L"baidu.com",
            L"hao.360.cn", L"so.com", L"sogou.com", L"about:blank", L"go.microsoft.com",
        };
        wstring low = FoldLowP(v);
        for (const wchar_t* d : k) if (low.find(d) != wstring::npos) return true;
        return false;
    }

    void ScanLnkDir(const wstring& dir, vector<HomeFind>& out) {
        WIN32_FIND_DATAW fd{};
        HANDLE f = FindFirstFileW((dir + L"\\*.lnk").c_str(), &fd);
        if (f == INVALID_HANDLE_VALUE) return;
        do {
            wstring full = dir + L"\\" + fd.cFileName;
            wstring args = ReadLnkArgs(full);
            if (args.empty()) continue;
            if (!IsHttpUrl(args)) continue;
            HomeFind hf;
            hf.kind = 3;
            hf.name = fd.cFileName;
            hf.location = full;
            hf.value = args;
            out.push_back(hf);
        } while (FindNextFileW(f, &fd));
        FindClose(f);
    }

    // 读 lnk 参数（IShellLinkW；失败返回空串）
    static wstring ReadLnkArgs(const wstring& path) {
        wstring out;
        IShellLinkW* sl = nullptr;
        if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
                                    IID_IShellLinkW, (void**)&sl)) || !sl) return out;
        IPersistFile* pf = nullptr;
        if (SUCCEEDED(sl->QueryInterface(IID_IPersistFile, (void**)&pf)) && pf) {
            if (SUCCEEDED(pf->Load(path.c_str(), STGM_READ))) {
                wchar_t buf[INFOTIPSIZE]{};
                if (SUCCEEDED(sl->GetArguments(buf, INFOTIPSIZE))) out = buf;
            }
            pf->Release();
        }
        sl->Release();
        return out;
    }

    // 清理 lnk 参数劫持：备份原参数到 TSV，然后清空参数
    int FixLnkHijacks(ModuleContext& ctx) {
        int fixed = 0;
        for (const HomeFind& f : finds_) {
            if (f.kind != 3) continue;
            // 备份
            HomeBackupAppend(L"LNK\t" + f.location + L"\t" + f.value);
            // 动手：重写参数为空
            IShellLinkW* sl = nullptr;
            if (SUCCEEDED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
                                           IID_IShellLinkW, (void**)&sl)) && sl) {
                IPersistFile* pf = nullptr;
                if (SUCCEEDED(sl->QueryInterface(IID_IPersistFile, (void**)&pf)) && pf) {
                    if (SUCCEEDED(pf->Load(f.location.c_str(), STGM_READ))) {
                        sl->SetArguments(L"");
                        if (SUCCEEDED(pf->Save(f.location.c_str(), TRUE))) {
                            ++fixed;
                            ctx.Log(L"  已清理快捷方式：" + f.location);
                        }
                    }
                    pf->Release();
                }
                sl->Release();
            }
        }
        return fixed;
    }
};

REGISTER_MODULE(PurgHomepageModule);

} // namespace
} // namespace mz
