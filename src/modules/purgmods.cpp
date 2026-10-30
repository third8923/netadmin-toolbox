// ============================================================================
//  网管工具箱 (NetAdmin Toolbox) —— 净化中心模块集（v2.13 M1）
//  ---------------------------------------------------------------------------
//  purg.scan 全盘查杀（Tweaks 型 + 扩展动作）：
//    Run()        → Ruleset 加载 + 11 类自启点全量枚举 + 四步判定
//    RunAction(0) → 处置选中项（purgcore::ActOnFinding，先备份后动手）
//    RunAction(1) → 还原上次处置（purgcore::UndoAll，倒序还原）
//    RunAction(2) → 在线更新特征库（WinHTTP 双源：SoftCnKiller gitee +
//                   本项目 GitHub 自有规则；SHA256 记录；配置 rules\update.ini）
//    RunAction(3) → 特征库信息（来源/条数/目录落点）
//    RunAction(4) → 打开特征库目录
//  ModuleRow.tag = findings_ 下标（排序安全回溯，与 sys.tasks 同款）。
//  链接：-lwinhttp -lwintrust -lcrypt32（build.bat 统一追加）
// ============================================================================
#include "../module.h"
#include "../common.h"
#include "../ruleset.h"
#include "../purgcore.h"
#include "../cleanresult.h"

#include <algorithm>
#include <cstdio>
#include <cwchar>

#include <winhttp.h>
#include <wincrypt.h>

namespace mz {

using namespace mz::purg;   // Ruleset / RulesDir / Finding / KindName …

namespace {

// ---------------------------------------------------------------- 通用小工具

wstring U32s(unsigned v) { wchar_t b[32]{}; swprintf_s(b, 32, L"%u", v); return b; }

wstring HrTextP(HRESULT hr) {
    if (HRESULT_FACILITY(hr) == FACILITY_WIN32)
        return ErrorTextZh(HRESULT_CODE(hr));
    return ErrorTextZh((DWORD)hr);
}

// ---------------------------------------------------------------- 在线更新

// URL 拆解（仅 https）
bool SplitUrl(const wstring& url, wstring* host, wstring* path) {
    if (url.rfind(L"https://", 0) != 0) return false;
    wstring rest = url.substr(8);
    size_t slash = rest.find(L'/');
    if (slash == wstring::npos) { *host = rest; *path = L"/"; return !rest.empty(); }
    *host = rest.substr(0, slash);
    *path = rest.substr(slash);
    return !host->empty();
}

struct HttpReply {
    bool ok = false;
    long status = 0;
    wstring error;
    std::string body;
};

// HTTPS GET（≤3 跳重定向；15 秒超时；UTF-8 原样字节）
HttpReply HttpsGet(const wstring& url) {
    HttpReply rep;
    wstring host, path;
    if (!SplitUrl(url, &host, &path)) {
        rep.error = L"仅支持 https URL：" + url;
        return rep;
    }
    HINTERNET ses = WinHttpOpen(L"NetAdmin-Toolbox-Purg/2.13",
                                WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!ses) { rep.error = L"WinHttpOpen 失败：" + HrTextP(GetLastError()); return rep; }
    DWORD timeout = 15000;
    WinHttpSetOption(ses, WINHTTP_OPTION_RESOLVE_TIMEOUT, &timeout, sizeof(timeout));
    WinHttpSetOption(ses, WINHTTP_OPTION_CONNECT_TIMEOUT, &timeout, sizeof(timeout));
    WinHttpSetOption(ses, WINHTTP_OPTION_SEND_TIMEOUT, &timeout, sizeof(timeout));
    WinHttpSetOption(ses, WINHTTP_OPTION_RECEIVE_TIMEOUT, &timeout, sizeof(timeout));

    wstring curUrl = url;
    for (int hop = 0; hop < 3; ++hop) {
        SplitUrl(curUrl, &host, &path);
        HINTERNET conn = WinHttpConnect(ses, host.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0);
        if (!conn) { rep.error = L"连接失败：" + host + L"（" + HrTextP(GetLastError()) + L"）"; break; }
        HINTERNET req = WinHttpOpenRequest(conn, L"GET", path.c_str(), nullptr,
                                           WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                           WINHTTP_FLAG_SECURE);
        if (!req) {
            rep.error = L"构造请求失败：" + HrTextP(GetLastError());
            WinHttpCloseHandle(conn);
            break;
        }
        BOOL sent = WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                       WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
                    WinHttpReceiveResponse(req, nullptr);
        if (!sent) {
            rep.error = L"请求发送/接收失败：" + HrTextP(GetLastError());
            WinHttpCloseHandle(req);
            WinHttpCloseHandle(conn);
            break;
        }
        DWORD status = 0, sz = sizeof(status);
        WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &status, &sz, WINHTTP_NO_HEADER_INDEX);
        rep.status = (long)status;
        if (status == 301 || status == 302) {
            // 重定向：取 Location 再跳
            wchar_t loc[1024] = {};
            DWORD ls = sizeof(loc);
            if (WinHttpQueryHeaders(req, WINHTTP_QUERY_LOCATION,
                                    WINHTTP_HEADER_NAME_BY_INDEX, loc, &ls, WINHTTP_NO_HEADER_INDEX)) {
                wstring newLoc(loc);
                if (newLoc.rfind(L"https://", 0) != 0) {
                    // 相对路径 → 拼回
                    newLoc = L"https://" + host + newLoc;
                }
                WinHttpCloseHandle(req);
                WinHttpCloseHandle(conn);
                curUrl = newLoc;
                continue;   // 下一跳
            }
            rep.error = L"重定向但缺少 Location 头";
            WinHttpCloseHandle(req);
            WinHttpCloseHandle(conn);
            break;
        }
        if (status != 200) {
            rep.error = L"HTTP 状态 " + U32s(status) + L"（源未发布或路径错误）";
            WinHttpCloseHandle(req);
            WinHttpCloseHandle(conn);
            break;
        }
        // 读 body
        rep.body.clear();
        for (;;) {
            DWORD avail = 0;
            if (!WinHttpQueryDataAvailable(req, &avail) || avail == 0) break;
            size_t old = rep.body.size();
            rep.body.resize(old + avail);
            DWORD read = 0;
            if (!WinHttpReadData(req, rep.body.data() + old, avail, &read) || read == 0) {
                rep.body.resize(old);
                break;
            }
            if (read < avail) rep.body.resize(old + read);
        }
        rep.ok = true;
        WinHttpCloseHandle(req);
        WinHttpCloseHandle(conn);
        break;
    }
    WinHttpCloseHandle(ses);
    return rep;
}

// SHA256（wincrypt，输出 hex 小写）
std::string Sha256Hex(const std::string& data) {
    HCRYPTPROV prov = 0;
    HCRYPTHASH hash = 0;
    std::string hex;
    if (!CryptAcquireContextW(&prov, nullptr, MS_ENH_RSA_AES_PROV, PROV_RSA_AES,
                              CRYPT_VERIFYCONTEXT)) {
        CryptAcquireContextW(&prov, nullptr, MS_DEF_PROV, PROV_RSA_FULL, CRYPT_VERIFYCONTEXT);
    }
    if (prov && CryptCreateHash(prov, CALG_SHA_256, 0, 0, &hash)) {
        CryptHashData(hash, (const BYTE*)data.data(), (DWORD)data.size(), 0);
        BYTE dig[32] = {};
        DWORD len = 32;
        if (CryptGetHashParam(hash, HP_HASHVAL, dig, &len, 0)) {
            char b[3];
            for (int i = 0; i < 32; ++i) {
                sprintf_s(b, 3, "%02x", dig[i]);
                hex += b;
            }
        }
        CryptDestroyHash(hash);
    }
    if (prov) CryptReleaseContext(prov, 0);
    return hex;
}

// 写字节文件
bool WriteBytes(const wstring& path, const std::string& data) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD w = 0;
    BOOL ok = WriteFile(h, data.data(), (DWORD)data.size(), &w, nullptr);
    CloseHandle(h);
    return ok != FALSE;
}

// 更新源配置（rules\update.ini 可覆盖；默认双源）
struct UpdateSources {
    wstring softcn;   // SoftCnKiller 数据仓库 raw 根
    wstring custom;   // 本项目自有规则 raw 根（GitHub）
};
UpdateSources LoadSources() {
    UpdateSources s;
    s.softcn = L"https://gitee.com/softcnkiller/data/raw/master";
    s.custom = L"https://raw.githubusercontent.com/biohacker-fun/netadmin-toolbox-rules/main/purg";
    wchar_t buf[512] = {};
    wstring ini = RulesDir() + L"\\update.ini";
    GetPrivateProfileStringW(L"source", L"softcn", s.softcn.c_str(), buf, 512, ini.c_str());
    s.softcn = buf;
    GetPrivateProfileStringW(L"source", L"custom", s.custom.c_str(), buf, 512, ini.c_str());
    s.custom = buf;
    return s;
}

// 下载并覆盖一个规则文件；返回空串成功，否则错误描述
wstring UpdateOneFile(ModuleContext& ctx, const wstring& base, const wstring& relName,
                      const wstring& dstPath, const wstring& label, size_t minLines) {
    ctx.Log(L"下载 " + label + L"：" + base + L"/" + relName);
    HttpReply rep = HttpsGet(base + L"/" + relName);
    if (!rep.ok) return L"[" + label + L"] " + rep.error;
    // 校验：非空 + 可解析行数达标（防半截/错误页落库）
    if (rep.body.size() < 64) return L"[" + label + L"] 内容过短（" +
                                     U32s((unsigned)rep.body.size()) + L" 字节），已丢弃";
    std::string tmpPath8 = WToUtf8(dstPath + L".tmp");
    HANDLE h = CreateFileW((dstPath + L".tmp").c_str(), GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return L"[" + label + L"] 写临时文件失败：" + ErrorTextZh(GetLastError());
    DWORD w = 0;
    WriteFile(h, rep.body.data(), (DWORD)rep.body.size(), &w, nullptr);
    CloseHandle(h);
    // 解析校验
    bool okText = false;
    wstring text = ReadTextSmart(dstPath + L".tmp", &okText);
    size_t lines = 0;
    if (okText) {
        size_t p = 0;
        while (p <= text.size()) {
            size_t e = text.find(L'\n', p);
            if (e == wstring::npos) e = text.size();
            if (e > p) ++lines;
            if (e == text.size()) break;
            p = e + 1;
        }
    }
    if (!okText || lines < minLines) {
        // 诊断预览：前 32 字节 hex（区分 HTML 挑战页/编码错误/空响应）
        wstring preview;
        for (size_t i = 0; i < rep.body.size() && i < 32; ++i) {
            wchar_t hex[8];
            swprintf_s(hex, 8, L"%02X ", (unsigned char)rep.body[i]);
            preview += hex;
        }
        // 读回诊断：okText 与解码后字符数
        bool dbg = false;
        wstring dbgText = ReadTextSmart(dstPath + L".tmp", &dbg);
        wchar_t db[64];
        swprintf_s(db, 64, L"读回: ok=%d chars=%zu", dbg ? 1 : 0, dbgText.size());
        DeleteFileW((dstPath + L".tmp").c_str());
        return L"[" + label + L"] 内容校验失败（行数 " + U32s((unsigned)lines) +
               L" < 门槛 " + U32s((unsigned)minLines) + L"，大小 " +
               U32s((unsigned)rep.body.size()) + L" 字节，" + db +
               L"，头部: " + preview + L"），已丢弃";
    }
    if (!MoveFileExW((dstPath + L".tmp").c_str(), dstPath.c_str(), MOVEFILE_REPLACE_EXISTING))
        return L"[" + label + L"] 替换失败：" + ErrorTextZh(GetLastError());
    // SHA256 记录（供后续完整性核对）
    std::string hex = Sha256Hex(rep.body);
    wstring ini = RulesDir() + L"\\update.ini";
    WritePrivateProfileStringW(L"hashes", label.c_str(),
                               wstring(hex.begin(), hex.end()).c_str(), ini.c_str());
    ctx.Log(L"  已更新 " + label + L"（" + U32s((unsigned)lines) + L" 行，sha256 " +
            wstring(hex.begin(), hex.end()).substr(0, 16) + L"…）");
    return wstring();
}

// ---------------------------------------------------------------- 模块

class PurgScanModule : public IModule {
public:
    const wchar_t* Id() const override { return L"purg.scan"; }
    const wchar_t* Group() const override { return L"净化中心"; }
    const wchar_t* Title() const override { return L"全盘查杀"; }
    const wchar_t* Description() const override {
        return L"SoftCnKiller 特征库查杀：进程/服务/驱动/启动项/计划任务/右键菜单/"
               L"输入法/文件关联/浏览器扩展/hosts/代理 11 类点全量检测，"
               L"先备份后处置，可一键还原";
    }
    ModuleKind Kind() const override { return ModuleKind::Tweaks; }
    bool NeedsElevation() const override { return true; }

    // 主按钮（btnRun）文案
    const wchar_t* RunLabel() const override { return L"开始查杀"; }

    // 动作 0 → btnClean 位；1 → btnUndo 位；2+ → 「更多 ▾」菜单
    vector<ModuleAction> Actions() const override {
        return {
            { L"处置选中项",   nullptr, true,  0xE74D },  // 0 danger
            { L"还原上次处置", nullptr, false, 0xE72C },  // 1
            { L"在线更新特征库", L"将联网下载 SoftCnKiller 数据仓库（gitee.com/"
              L"softcnkiller/data）与本项目的自有补充规则，覆盖本地特征库文件。\n\n"
              L"是否继续？", false, 0xE895 },            // 2
            { L"特征库信息", nullptr, false, 0 },        // 3 → 更多 ▾
            { L"打开特征库目录", nullptr, false, 0 },    // 4 → 更多 ▾
        };
    }
    bool RefreshAfterAction() const override { return true; }
    void SetSelection(const vector<int>& rows) override { selection_ = rows; }

    wstring ActionWarning(int actionIndex, const vector<int>& tags) const override {
        switch (actionIndex) {
        case 0: {
            if (tags.empty()) return L"未选中任何条目。\n\n请先在列表中勾选要处置的可疑项。";
            wstring names;
            int adv = 0;
            for (int t : tags) {
                if (t >= 0 && t < (int)findings_.size()) {
                    const Finding& f = findings_[t];
                    names += wstring(L"【") + KindActionName(f.kind) + L"】" +
                             (f.name.empty() ? f.path : f.name) + L"\n";
                    if (f.advisory) ++adv;
                }
            }
            wstring body = L"将按各项语义处置（先备份、可还原）：\n" + names;
            if (adv > 0)
                body += L"\n注意：其中 " + U32s(adv) + L" 项为提示信息（如系统代理），"
                        L"处置=关闭该设置。\n";
            body += L"\n是否继续？";
            return body;
        }
        case 1: {
            return L"将按备份记录倒序还原全部已处置项（写回注册表值/重建任务/"
                   L"移回隔离文件/恢复服务启动类型）。\n\n是否继续？";
        }
        default:
            return wstring();
        }
    }

    void Run(ModuleContext& ctx, ModuleResult& out) override {
        out = ModuleResult{};
        findings_.clear();

        // ① 特征库
        Ruleset rs;
        wstring stat;
        if (!rs.Load(&stat)) {
            out.summary = L"特征库加载失败";
            ctx.Log(L"特征库加载失败（内嵌基线不可用）");
            return;
        }
        ctx.Log(stat);
        ctx.Log(L"特征库来源：SoftCnKiller 数据仓库（四海一叶秋，致谢）+ 本项目补充");
        ctx.Log(L"本地目录：" + RulesDir());

        // ② 全量枚举
        ctx.Log(L"");
        EnumAll(rs, ctx, findings_);
        if (ctx.Cancelled()) {
            out.summary = L"已取消（部分结果如下）";
        }

        // ③ 结果落表
        int signHit = 0, folderHit = 0, other = 0, advisory = 0;
        for (size_t i = 0; i < findings_.size(); ++i) {
            const Finding& f = findings_[i];
            ModuleRow r;
            r.c1 = (f.name.empty() ? f.path : f.name);
            r.c2 = KindName(f.kind);
            r.c3 = f.hitType + L"命中" + wstring(f.advisory ? L"（提示）" : L"");
            r.c4 = f.hit;
            if (!f.path.empty() && f.path != f.name)
                r.c4 += (r.c4.empty() ? wstring() : L" | ") + f.path;
            r.tag = (int)i;
            r.danger = !f.advisory;
            if (f.advisory) ++advisory;
            else if (f.hitType == L"签名") ++signHit;
            else if (f.hitType == L"目录") ++folderHit;
            else ++other;
            out.rows.push_back(std::move(r));
        }
        wchar_t b[256];
        swprintf_s(b, 256, L"发现可疑项 %zu 个（签名 %d · 目录 %d · 劫持/其他 %d · 提示 %d）",
                   findings_.size(), signHit, folderHit, other, advisory);
        out.summary = b;
        out.hint = findings_.empty()
            ? T(L"未发现命中特征库的项。特征库来源见「更多 ▾ → 特征库信息」。")
            : wstring(T(L"勾选后点「处置选中项」；处置前自动备份，可用「还原上次处置」一键还原。"))
              + T(L"微软签名文件已自动放行。");
        out.canClean = true;
    }

    void RunAction(ModuleContext& ctx, int actionIndex, ModuleResult& out) override {
        out = ModuleResult{};
        switch (actionIndex) {
        case 0: {   // 处置选中项
            if (selection_.empty()) {
                out.summary = L"未选中任何条目";
                out.canClean = true;
                return;
            }
            int ok = 0, fail = 0, skip = 0;
            for (int tag : selection_) {
                if (ctx.Cancelled()) break;
                if (tag < 0 || tag >= (int)findings_.size()) continue;
                const Finding& f = findings_[tag];
                wstring err = ActOnFinding(f);
                if (err.empty()) {
                    ++ok;
                    ctx.Log(L"  已" + wstring(KindActionName(f.kind)) + L": " +
                            (f.name.empty() ? f.path : f.name));
                } else if (err.find(L"备份") != wstring::npos) {
                    ++ok;   // 已执行但备份记录失败——仍计成功，错误显式提示
                    ctx.Log(L"  已" + wstring(KindActionName(f.kind)) + L": " +
                            (f.name.empty() ? f.path : f.name) + L"（注意：" + err + L"）");
                } else {
                    ++fail;
                    ctx.Log(L"  失败: " + (f.name.empty() ? f.path : f.name) + L" — " + err);
                }
            }
            (void)skip;
            wchar_t b[128];
            swprintf_s(b, 128, L"处置完成：成功 %d，失败 %d", ok, fail);
            out.summary = b;
            out.canClean = true;
            break;
        }
        case 1: {   // 还原上次处置
            ctx.Log(L"开始还原（读取备份：" + BackupFile() + L"）");
            int fails = 0;
            int ok = UndoAll(ctx, &fails);
            wchar_t b[128];
            swprintf_s(b, 128, L"还原完成：成功 %d，失败 %d", ok, fails);
            out.summary = b;
            if (ok == 0 && fails == 0) out.summary = L"备份为空，没有可还原的处置记录";
            out.canClean = true;
            break;
        }
        case 2: {   // 在线更新特征库
            out = RunUpdate(ctx);
            break;
        }
        case 3: {   // 特征库信息
            Ruleset rs;
            wstring stat;
            rs.Load(&stat);
            ctx.Log(stat);
            ctx.Log(L"特征库目录：" + RulesDir());
            ctx.Log(L"数据来源：SoftCnKiller 数据仓库（作者：四海一叶秋）");
            ctx.Log(L"  https://gitee.com/softcnkiller/data（致谢：特征内容版权归原作者）");
            ctx.Log(L"自有补充：rules\\extra_sign.txt / extra_folder.txt / extra_white.txt");
            ctx.Log(L"在线更新源配置：rules\\update.ini（[source] softcn / custom）");
            out.summary = L"特征库信息已输出到日志";
            out.canClean = true;
            break;
        }
        case 4: {   // 打开特征库目录
            Ruleset rs;
            rs.Load(nullptr);   // 确保目录存在
            SHELLEXECUTEINFOW sei{};
            sei.cbSize = sizeof(sei);
            sei.lpVerb = L"open";
            sei.lpFile = RulesDir().c_str();
            sei.nShow = SW_SHOWNORMAL;
            ShellExecuteExW(&sei);
            out.summary = L"已打开特征库目录";
            out.canClean = true;
            break;
        }
        default: break;
        }
    }

private:
    vector<int> selection_;
    vector<Finding> findings_;

    ModuleResult RunUpdate(ModuleContext& ctx) {
        ModuleResult out;
        UpdateSources src = LoadSources();
        ctx.Log(L"在线更新特征库");
        ctx.Log(L"SoftCnKiller 源：" + src.softcn);
        ctx.Log(L"自有规则源：" + src.custom);
        ctx.Log(L"");

        const wstring dir = RulesDir();
        CreateDirectoryW(dir.c_str(), nullptr);
        int okCount = 0, skipCount = 0, failCount = 0;

        // SoftCnKiller 三件套（门槛：sign ≥ 200 行 / folder ≥ 500 行 / 白名单 ≥ 1 行）
        struct Upd { const wchar_t* rel; const wchar_t* dst; const wchar_t* label; size_t minLines; };
        const Upd softcnFiles[] = {
            { L"sign.txt",     L"sign.txt",     L"签名黑名单", 200 },
            { L"folder.txt",   L"folder.txt",   L"目录特征",   500 },
            { L"whitepath.txt", L"whitepath.txt", L"白名单",    1 },
        };
        for (const Upd& u : softcnFiles) {
            wstring err = UpdateOneFile(ctx, src.softcn, u.rel, dir + L"\\" + u.dst,
                                        u.label, u.minLines);
            if (err.empty()) ++okCount;
            else {
                // 源 404/网络失败：白名单这类小文件 404 不算致命
                ctx.Log(L"  " + err);
                if (rep_status_missing(err)) ++skipCount; else ++failCount;
            }
        }

        // 自有规则源（extra_* 三件套；未发布时 404 静默跳过）
        const Upd customFiles[] = {
            { L"extra_sign.txt",   L"extra_sign.txt",   L"自有签名补充", 1 },
            { L"extra_folder.txt", L"extra_folder.txt", L"自有目录补充", 1 },
            { L"extra_white.txt",  L"extra_white.txt",  L"自有白名单",   1 },
        };
        for (const Upd& u : customFiles) {
            wstring err = UpdateOneFile(ctx, src.custom, u.rel, dir + L"\\" + u.dst,
                                        u.label, u.minLines);
            if (err.empty()) ++okCount;
            else {
                ctx.Log(L"  " + err);
                ++skipCount;   // 自有源未发布属预期，一律跳过不记失败
            }
        }

        // 汇总 + 重新加载验证
        ctx.Log(L"");
        Ruleset rs;
        wstring stat;
        rs.Load(&stat);
        ctx.Log(stat);
        wchar_t b[256];
        swprintf_s(b, 256, L"更新完成：成功 %d，跳过 %d，失败 %d（下次查杀即生效）",
                   okCount, skipCount, failCount);
        out.summary = b;
        out.canClean = true;
        return out;
    }

    // 错误描述里是否为「源缺失」类（HTTP 404 等）——跳过而非失败
    static bool rep_status_missing(const wstring& err) {
        return err.find(L"HTTP 状态 4") != wstring::npos;
    }
};

REGISTER_MODULE(PurgScanModule);

} // namespace
} // namespace mz
