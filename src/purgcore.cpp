// ============================================================================
//  NetAdmin Toolbox — 流氓软件净化器 · 查杀引擎实现（v2.13 T02/T03）
// ============================================================================
#include "purgcore.h"
#include "cleanresult.h"    // ErrorTextZh

#include <algorithm>
#include <cstdio>
#include <cwchar>
#include <unordered_map>

#include <softpub.h>        // WinVerifyTrust / WINTRUST_DATA
#include <wincrypt.h>       // CryptQueryObject / CMSG_SIGNER_INFO
#include <winsvc.h>
#include <tlhelp32.h>
#include <shlobj.h>         // SHGetFolderPath / IShellLink
#include <taskschd.h>
#include <oleauto.h>

namespace mz {
namespace purg {

// ---------------------------------------------------------------- 通用小工具

namespace {

wstring U32(unsigned v) { wchar_t b[32]{}; swprintf_s(b, 32, L"%u", v); return b; }

wstring FoldLower(wstring s) {
    for (auto& c : s) {
        if (c >= L'A' && c <= L'Z') c = (wchar_t)(c - L'A' + L'a');
        else if (c >= 192 && c <= 221 && c != 215) c = (wchar_t)(c + 32);
    }
    return s;
}

// 路径分隔统一 \ + 小写（匹配用）
wstring NormPath(wstring s) {
    std::replace(s.begin(), s.end(), L'/', L'\\');
    return FoldLower(std::move(s));
}

// 环境变量展开（%SystemRoot% 等）
wstring ExpandEnv(const wstring& s) {
    wchar_t buf[1024] = {};
    DWORD n = ExpandEnvironmentStringsW(s.c_str(), buf, 1024);
    if (n > 0 && n < 1024) return wstring(buf, n - 1);
    return s;
}

// COM 会话（枚举多个点共用）
struct ComSession {
    bool uninit = false;
    bool Init() {
        HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        if (SUCCEEDED(hr)) { uninit = true; return true; }
        if (hr == RPC_E_CHANGED_MODE) return true;   // 已初始化为其他模式，可用
        return false;
    }
    ~ComSession() { if (uninit) CoUninitialize(); }
};

wstring HrText(HRESULT hr) {
    if (HRESULT_FACILITY(hr) == FACILITY_WIN32)
        return ErrorTextZh(HRESULT_CODE(hr));
    return ErrorTextZh((DWORD)hr);
}

// MinGW taskschd.h 未导出 CLSID_TaskScheduler，本地定义（与 taskmods.cpp 一致）
const CLSID kCLSID_TaskScheduler = {
    0x0f87369f, 0xa4e5, 0x4cfc, { 0xbd, 0x3e, 0x73, 0xe6, 0x15, 0x45, 0x72, 0xdd } };

wstring TakeBstr(BSTR b) {
    wstring out = b ? wstring(b, SysStringLen(b)) : wstring();
    SysFreeString(b);
    return out;
}

// 注册表：读 SZ 值
wstring RegGetSz(HKEY root, const wstring& sub, const wstring& name) {
    wchar_t buf[2048] = {};
    DWORD sz = sizeof(buf), type = 0;
    if (RegGetValueW(root, sub.c_str(), name.c_str(), RRF_RT_REG_SZ, &type, buf, &sz)
        != ERROR_SUCCESS) return wstring();
    return wstring(buf);
}

// 命令行 → exe 路径（引号优先；无引号时对空格切分做存在性探测）
wstring ExeFromCommandLine(wstring cmd) {
    cmd = ExpandEnv(cmd);
    auto exists = [](const wstring& p) {
        DWORD a = GetFileAttributesW(p.c_str());
        return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
    };
    if (!cmd.empty() && cmd[0] == L'"') {
        size_t end = cmd.find(L'"', 1);
        if (end != wstring::npos) return cmd.substr(1, end - 1);
        return cmd.substr(1);
    }
    // 环境变量包裹式 %windir%\xxx（无引号）直接整段
    size_t sp = wstring::npos;
    while ((sp = cmd.find_last_of(L' ')) != wstring::npos) {
        wstring cand = cmd.substr(0, sp);
        if (!cand.empty() && exists(cand)) return cand;
        cmd = cand;   // 去掉最后一个参数继续收窄
        // 若整段最后是转义收尾
        if (cmd.empty()) break;
        if (!cmd.empty() && cmd.back() == L' ') cmd.pop_back();
    }
    return cmd;
}

// 文件存在？
bool FileExists(const wstring& p) {
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

} // namespace

// ---------------------------------------------------------------- KindName

const wchar_t* KindName(int kind) {
    static const wchar_t* k[] = {
        L"进程", L"服务", L"驱动", L"启动项", L"计划任务", L"右键菜单",
        L"输入法", L"文件关联", L"浏览器扩展", L"hosts 劫持", L"系统代理",
        L"内核模块", L"隐藏进程",
    };
    return (kind >= 0 && kind < F_COUNT) ? k[kind] : L"?";
}

const wchar_t* KindActionName(int kind) {
    static const wchar_t* k[] = {
        L"结束进程", L"禁用服务", L"禁用驱动", L"删除启动项", L"删除任务",
        L"移除注册", L"删除布局", L"恢复默认关联", L"隔离扩展", L"清除劫持行",
        L"关闭代理", L"禁用驱动服务", L"结束隐藏进程",
    };
    return (kind >= 0 && kind < F_COUNT) ? k[kind] : L"?";
}

// ---------------------------------------------------------------- 签名验证

namespace {

// 缓存：路径 → 签名信息（进程内，一扫一建）
std::unordered_map<wstring, SignInfo>& SignCache() {
    static std::unordered_map<wstring, SignInfo> c;
    return c;
}

// 证书主体名：O 与 CN（O 缺省时退 CN；两者都有则 "O (CN)"）
wstring CertSubjectNames(PCCERT_CONTEXT cert) {
    if (!cert) return wstring();
    wchar_t buf[512] = {};
    wstring org, cn;
    if (CertGetNameStringW(cert, CERT_NAME_ATTR_TYPE, 0,
                           (void*)szOID_ORGANIZATION_NAME, buf, 512) > 1)
        org = buf;
    ZeroMemory(buf, sizeof(buf));
    if (CertGetNameStringW(cert, CERT_NAME_ATTR_TYPE, 0,
                           (void*)szOID_COMMON_NAME, buf, 512) > 1)
        cn = buf;
    if (org.empty()) return cn;
    if (cn.empty() || FoldLower(cn) == FoldLower(org)) return org;
    // CN 常含 "Inc/Ltd/有限公司" 等更完整信息 → O + (CN)
    return org + L" (" + cn + L")";
}

// 从嵌入签名提取主体名（无签名 / 结构异常返回空）
wstring SignerSubjectFromEmbed(const wstring& file) {
    HCERTSTORE store = nullptr;
    HCRYPTMSG msg = nullptr;
    DWORD encoding = 0, contentType = 0, formatType = 0;
    if (!CryptQueryObject(CERT_QUERY_OBJECT_FILE, file.c_str(),
                          CERT_QUERY_CONTENT_FLAG_PKCS7_SIGNED_EMBED,
                          CERT_QUERY_FORMAT_FLAG_BINARY, 0,
                          &encoding, &contentType, &formatType,
                          &store, &msg, nullptr) || !store || !msg) {
        if (store) CertCloseStore(store, 0);
        if (msg) CryptMsgClose(msg);
        return wstring();
    }
    wstring out;
    DWORD sz = 0;
    if (CryptMsgGetParam(msg, CMSG_SIGNER_INFO_PARAM, 0, nullptr, &sz) && sz >= sizeof(CMSG_SIGNER_INFO)) {
        vector<BYTE> buf(sz);
        if (CryptMsgGetParam(msg, CMSG_SIGNER_INFO_PARAM, 0, buf.data(), &sz)) {
            auto* si = reinterpret_cast<CMSG_SIGNER_INFO*>(buf.data());
            CERT_INFO ci{};
            ci.Issuer = si->Issuer;
            ci.SerialNumber = si->SerialNumber;
            PCCERT_CONTEXT cert = CertFindCertificateInStore(
                store, X509_ASN_ENCODING | PKCS_7_ASN_ENCODING, 0,
                CERT_FIND_SUBJECT_CERT, &ci, nullptr);
            if (cert) {
                out = CertSubjectNames(cert);
                CertFreeCertificateContext(cert);
            }
        }
    }
    CryptMsgClose(msg);
    CertCloseStore(store, 0);
    return out;
}

} // namespace

SignInfo QuerySign(const wstring& file) {
    wstring key = NormPath(file);
    auto it = SignCache().find(key);
    if (it != SignCache().end()) return it->second;

    SignInfo info;
    WINTRUST_FILE_INFO fi{};
    fi.cbStruct = sizeof(fi);
    fi.pcwszFilePath = file.c_str();
    GUID action = WINTRUST_ACTION_GENERIC_VERIFY_V2;
    WINTRUST_DATA wd{};
    wd.cbStruct = sizeof(wd);
    wd.dwUIChoice = WTD_UI_NONE;
    wd.fdwRevocationChecks = WTD_REVOKE_NONE;
    wd.dwUnionChoice = WTD_CHOICE_FILE;
    wd.dwStateAction = WTD_STATEACTION_IGNORE;
    wd.pFile = &fi;
    LONG st = WinVerifyTrust((HWND)INVALID_HANDLE_VALUE, &action, &wd);
    if (st == 0) {
        info.present = true;
        info.trusted = true;
        info.subject = SignerSubjectFromEmbed(file);
    } else if (st == TRUST_E_NOSIGNATURE) {
        info.present = false;   // 无签名（常见）
    } else {
        info.present = true;
        info.trusted = false;   // 有签名但无效（失效/自签）——
        info.subject = SignerSubjectFromEmbed(file);
    }
    SignCache()[key] = info;
    return info;
}

// ---------------------------------------------------------------- 判定

wstring CheckFile(const Ruleset& rs, const wstring& exePath, wstring* signerOut) {
    if (signerOut) signerOut->clear();
    if (exePath.empty() || !FileExists(exePath)) return wstring();

    // ① 白名单最高优先级（whitepath.txt / extra_white.txt）
    if (rs.IsWhitelisted(exePath)) return wstring();

    // ② 微软签名放行（防 SoftCnKiller 式 cmd.exe 计划任务误报）
    SignInfo si = QuerySign(exePath);
    if (signerOut) *signerOut = si.subject;
    if (si.trusted && Ruleset::IsMicrosoftSign(si.subject)) return wstring();

    // ③ 签名黑名单（sign.txt 特征 vs 签名主体双向子串）
    if (si.present && !si.subject.empty()) {
        Hit h = rs.MatchSign(si.subject);
        if (h.ok) return L"签名命中：" + h.rule + L"（主体：" + si.subject + L"）";
    }

    // ④ 目录特征（folder.txt 特征 ⊂ 路径）
    Hit hp = rs.MatchPath(exePath);
    if (hp.ok) {
        wstring extra = si.present
            ? (si.trusted ? L"" : L"（注意：签名无效）")
            : L"（无签名）";
        return L"目录命中：" + hp.rule + extra;
    }
    return wstring();
}

// ---------------------------------------------------------------- 备份 TSV

namespace {

// TSV 行字段转义
constexpr wchar_t kEscTab = L's', kEscNl = L'n';

} // namespace

wstring EscapeField(const wstring& s) {
    wstring out;
    out.reserve(s.size());
    for (wchar_t c : s) {
        if (c == L'\\') out += L"\\\\";
        else if (c == L'\t') out += L"\\s";
        else if (c == L'\n') out += L"\\n";
        else if (c == L'\r') { /* 丢弃 */ }
        else out += c;
    }
    return out;
}

wstring UnescapeField(const wstring& s) {
    wstring out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == L'\\' && i + 1 < s.size()) {
            wchar_t n = s[++i];
            if (n == L'\\') out += L'\\';
            else if (n == kEscTab) out += L'\t';
            else if (n == kEscNl) out += L'\n';
            else { out += L'\\'; out += n; }
        } else out += s[i];
    }
    return out;
}

bool AppendBackup(const vector<BackupEntry>& entries, const wstring& note, wstring* err) {
    if (entries.empty()) return true;
    HANDLE h = CreateFileW(BackupFile().c_str(), FILE_APPEND_DATA, FILE_SHARE_READ,
                           nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        if (err) *err = L"无法写入备份文件：" + ErrorTextZh(GetLastError());
        return false;
    }
    // 文件为空 → 写格式头
    LARGE_INTEGER sz{};
    GetFileSizeEx(h, &sz);
    wstring body;
    if (sz.QuadPart == 0)
        body += L"#PURGBAK\tv1\n";
    SYSTEMTIME st{};
    GetLocalTime(&st);
    wchar_t tb[40]{};
    swprintf_s(tb, 40, L"%04u-%02u-%02u %02u:%02u:%02u", st.wYear, st.wMonth, st.wDay,
               st.wHour, st.wMinute, st.wSecond);
    body += wstring(L"#OP\t") + tb + L"\t" + EscapeField(note) + L"\n";
    for (const BackupEntry& e : entries) {
        body += U32((unsigned)e.kind) + L"\t" + EscapeField(e.item) + L"\t" +
                EscapeField(e.location) + L"\t" + EscapeField(e.valName) + L"\t" +
                EscapeField(e.regType) + L"\t" + EscapeField(e.data) + L"\t" +
                EscapeField(e.extra) + L"\n";
    }
    std::string u8 = WToUtf8(body);
    DWORD w = 0;
    // 追加前确认编码头：首个写入者补 BOM
    if (sz.QuadPart == 0) WriteFile(h, "\xEF\xBB\xBF", 3, &w, nullptr);
    BOOL ok = WriteFile(h, u8.data(), (DWORD)u8.size(), &w, nullptr);
    CloseHandle(h);
    if (err && !ok) *err = L"备份写入失败";
    return ok != FALSE;
}

vector<BackupEntry> ReadBackupAll() {
    vector<BackupEntry> out;
    bool ok = false;
    wstring text = ReadTextSmart(BackupFile(), &ok);
    if (!ok) return out;
    size_t start = 0;
    while (start <= text.size()) {
        size_t end = text.find(L'\n', start);
        if (end == wstring::npos) end = text.size();
        wstring line = text.substr(start, end - start);
        if (!line.empty() && line[0] != L'#') {
            // 拆 7 个字段
            vector<wstring> f;
            size_t p = 0;
            while (f.size() < 7) {
                size_t t = line.find(L'\t', p);
                if (t == wstring::npos) { f.push_back(line.substr(p)); break; }
                f.push_back(line.substr(p, t - p));
                p = t + 1;
            }
            if (f.size() >= 2) {
                BackupEntry e;
                e.kind = _wtoi(f[0].c_str());
                e.item = f.size() > 1 ? UnescapeField(f[1]) : wstring();
                e.location = f.size() > 2 ? UnescapeField(f[2]) : wstring();
                e.valName = f.size() > 3 ? UnescapeField(f[3]) : wstring();
                e.regType = f.size() > 4 ? UnescapeField(f[4]) : wstring();
                e.data = f.size() > 5 ? UnescapeField(f[5]) : wstring();
                e.extra = f.size() > 6 ? UnescapeField(f[6]) : wstring();
                out.push_back(std::move(e));
            }
        }
        if (end == text.size()) break;
        start = end + 1;
    }
    return out;
}

// ---------------------------------------------------------------- 处置

namespace {

// 注册表值备份 helper：读出 kind/type/data 填 entry
bool RegBackupValue(HKEY root, const wstring& sub, const wstring& valName, BackupEntry* e) {
    DWORD type = 0, sz = 0;
    LSTATUS q = RegGetValueW(root, sub.c_str(), valName.c_str(),
                             RRF_RT_ANY, &type, nullptr, &sz);
    if (q == ERROR_FILE_NOT_FOUND) { e->regType = L"ABSENT"; return true; }
    if (q != ERROR_SUCCESS) return false;
    if (type == REG_SZ || type == REG_EXPAND_SZ) {
        wstring v = RegGetSz(root, sub, valName);
        e->regType = (type == REG_EXPAND_SZ) ? L"REG_EXPAND_SZ" : L"REG_SZ";
        e->data = v;
    } else if (type == REG_DWORD) {
        DWORD v = 0, dsz = sizeof(v);
        RegGetValueW(root, sub.c_str(), valName.c_str(), RRF_RT_REG_DWORD, nullptr, &v, &dsz);
        e->regType = L"REG_DWORD";
        e->data = U32(v);
    } else {
        vector<BYTE> b(sz);
        DWORD bs = sz;
        RegGetValueW(root, sub.c_str(), valName.c_str(), RRF_RT_REG_BINARY, nullptr, b.data(), &bs);
        e->regType = L"REG_BINARY";
        wchar_t hex[3];
        for (DWORD i = 0; i < bs; ++i) {
            swprintf_s(hex, 3, L"%02X", b[i]);
            e->data += hex;
        }
    }
    return true;
}

// 还原注册表值
bool RegRestoreValue(HKEY root, const wstring& sub, const BackupEntry& e) {
    HKEY k;
    if (RegCreateKeyExW(root, sub.c_str(), 0, nullptr, REG_OPTION_NON_VOLATILE,
                        KEY_SET_VALUE, nullptr, &k, nullptr) != ERROR_SUCCESS)
        return false;
    bool ok = false;
    if (e.regType == L"ABSENT") {
        ok = RegDeleteValueW(k, e.valName.c_str()) == ERROR_SUCCESS || GetLastError() == ERROR_FILE_NOT_FOUND;
    } else if (e.regType == L"REG_DWORD") {
        DWORD v = (DWORD)_wtoi(e.data.c_str());
        ok = RegSetValueExW(k, e.valName.c_str(), 0, REG_DWORD, (const BYTE*)&v, sizeof(v)) == ERROR_SUCCESS;
    } else if (e.regType == L"REG_BINARY") {
        vector<BYTE> b;
        for (size_t i = 0; i + 1 < e.data.size(); i += 2) {
            wchar_t hex[3] = { e.data[i], e.data[i + 1], 0 };
            b.push_back((BYTE)wcstoul(hex, nullptr, 16));
        }
        ok = RegSetValueExW(k, e.valName.c_str(), 0, REG_BINARY, b.data(), (DWORD)b.size()) == ERROR_SUCCESS;
    } else {
        DWORD type = (e.regType == L"REG_EXPAND_SZ") ? REG_EXPAND_SZ : REG_SZ;
        ok = RegSetValueExW(k, e.valName.c_str(), 0, type,
                            (const BYTE*)e.data.c_str(),
                            (DWORD)((e.data.size() + 1) * sizeof(wchar_t))) == ERROR_SUCCESS;
    }
    RegCloseKey(k);
    return ok;
}

// 服务启动类型备份/还原
bool ServiceSetStart(const wstring& serviceName, DWORD startType, DWORD* oldStart) {
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scm) return false;
    SC_HANDLE svc = OpenServiceW(scm, serviceName.c_str(),
                                 SERVICE_CHANGE_CONFIG | SERVICE_QUERY_CONFIG);
    if (!svc) { CloseServiceHandle(scm); return false; }
    QUERY_SERVICE_CONFIGW* cfg = nullptr;
    DWORD need = 0;
    QueryServiceConfigW(svc, nullptr, 0, &need);
    if (need > 0) {
        vector<BYTE> buf(need);
        if (QueryServiceConfigW(svc, (QUERY_SERVICE_CONFIGW*)buf.data(), need, &need))
            if (oldStart) *oldStart = ((QUERY_SERVICE_CONFIGW*)buf.data())->dwStartType;
    }
    BOOL ok = ChangeServiceConfigW(svc, SERVICE_NO_CHANGE, startType, SERVICE_NO_CHANGE,
                                   nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr);
    CloseServiceHandle(svc);
    CloseServiceHandle(scm);
    return ok != FALSE;
}

// 进程终止（按 PID）
bool KillProcess(DWORD pid) {
    HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, pid);
    if (!h) return false;
    BOOL ok = TerminateProcess(h, 1);
    if (ok) WaitForSingleObject(h, 3000);
    CloseHandle(h);
    return ok != FALSE;
}

// 计划任务 COM 会话（处置/还原共用）
struct TaskCom {
    ITaskService* svc = nullptr;
    ComSession com;
    bool ok = false;
    wstring error;
    bool Init() {
        if (!com.Init()) { error = L"COM 初始化失败"; return false; }
        HRESULT hr = CoCreateInstance(kCLSID_TaskScheduler, nullptr, CLSCTX_INPROC_SERVER,
                                      IID_ITaskService, (void**)&svc);
        if (FAILED(hr) || !svc) { error = L"任务计划 COM 创建失败：" + HrText(hr); return false; }
        VARIANT v;
        VariantInit(&v);
        hr = svc->Connect(v, v, v, v);
        VariantClear(&v);
        if (FAILED(hr)) { error = L"连接任务计划服务失败：" + HrText(hr); return false; }
        ok = true;
        return true;
    }
    ~TaskCom() { if (svc) svc->Release(); }
};

} // namespace

wstring ActOnFinding(const Finding& f) {
    // ---------- 备份先行 ----------
    BackupEntry be;
    be.kind = f.kind; be.item = f.name; be.location = f.location;
    be.valName = f.valName; be.extra = f.extra;
    vector<BackupEntry> batch;

    wstring err;
    switch (f.kind) {
    case F_PROCESS: {
        // extra 存 PID
        DWORD pid = (DWORD)_wtoi(f.extra.c_str());
        if (!pid) return L"进程 ID 已失效";
        if (!KillProcess(pid)) return L"无法结束进程：" + ErrorTextZh(GetLastError());
        be.extra = f.path;   // 备份记路径（进程不可真还原，记日志用）
        batch.push_back(be);
        break;
    }
    case F_SERVICE:
    case F_DRIVER:
    case F_KMOD: {
        DWORD old = 0;
        if (!ServiceSetStart(f.name, SERVICE_DISABLED, &old))
            return L"禁用失败：" + ErrorTextZh(GetLastError());
        be.data = U32(old);   // 原 start type
        batch.push_back(be);
        break;
    }
    case F_HIDPROC: {
        // extra 存 PID；与 F_PROCESS 同款处置（还原仅记日志）
        DWORD pid = (DWORD)_wtoi(f.extra.c_str());
        if (!pid) return L"进程 ID 已失效";
        if (!KillProcess(pid)) return L"无法结束进程：" + ErrorTextZh(GetLastError());
        be.extra = f.path;
        batch.push_back(be);
        break;
    }
    case F_RUN: {
        // 注册表启动项：location = 完整键名（HKCU\…/HKLM\…），valName = 值名
        // 启动文件夹 lnk：extra 以 "LNK|" 开头（lnk 原路径），走隔离移动
        if (f.extra.rfind(L"LNK|", 0) == 0) {
            wstring src = f.extra.substr(4);
            CreateDirectoryW(QuarantineDir().c_str(), nullptr);
            wstring dst = QuarantineDir() + L"\\startup-" + f.name;
            if (!MoveFileExW(src.c_str(), dst.c_str(), MOVEFILE_REPLACE_EXISTING))
                return L"移入隔离失败：" + ErrorTextZh(GetLastError());
            BackupEntry bl{};
            bl.kind = f.kind; bl.item = f.name; bl.location = src;
            bl.extra = L"LNK|" + dst;   // 记隔离后位置，还原 = 移回
            batch.push_back(bl);
            break;
        }
        HKEY root = (f.location.rfind(L"HKCU\\", 0) == 0) ? HKEY_CURRENT_USER : HKEY_LOCAL_MACHINE;
        wstring sub = f.location.substr(5);   // 去 "HKCU\" / "HKLM\" 5 字符前缀
        RegBackupValue(root, sub, f.valName, &be);
        batch.push_back(be);
        HKEY k;
        if (RegOpenKeyExW(root, sub.c_str(), 0, KEY_SET_VALUE, &k) == ERROR_SUCCESS) {
            LSTATUS d = RegDeleteValueW(k, f.valName.c_str());
            RegCloseKey(k);
            if (d != ERROR_SUCCESS) return L"删除启动项值失败：" + ErrorTextZh(d);
        } else {
            return L"打开注册表键失败：" + ErrorTextZh(GetLastError());
        }
        break;
    }
    case F_TASK: {
        TaskCom tc;
        if (!tc.Init()) return tc.error;
        size_t slash = f.location.find_last_of(L'\\');
        wstring name = (slash == wstring::npos) ? f.location : f.location.substr(slash + 1);
        wstring parent = (slash == wstring::npos) ? L"\\" : f.location.substr(0, slash);
        BSTR pb = SysAllocString(parent.c_str());
        ITaskFolder* folder = nullptr;
        wstring fail;
        if (SUCCEEDED(tc.svc->GetFolder(pb, &folder)) && folder) {
            BSTR nb = SysAllocString(name.c_str());
            // 先取任务 XML（删除后无法再取），删除成功才写备份
            wstring xml;
            IRegisteredTask* task = nullptr;
            if (SUCCEEDED(folder->GetTask(nb, &task)) && task) {
                BSTR xb = nullptr;
                if (SUCCEEDED(task->get_Xml(&xb)) && xb) { xml = TakeBstr(xb); }
                task->Release();
            }
            HRESULT hr = folder->DeleteTask(nb, 0);
            SysFreeString(nb);
            if (FAILED(hr)) {
                fail = L"删除任务失败：" + HrText(hr);
            } else {
                BackupEntry bex = be;
                bex.extra = xml;
                batch.push_back(bex);
            }
            folder->Release();
        } else {
            fail = L"打开任务文件夹失败：" + HrText(GetLastError());
        }
        SysFreeString(pb);
        if (!fail.empty()) return fail;
        break;
    }
    case F_SHELLEX: {
        // location = 键全名 HKLM\Software\...\Shell Extensions\Approved，valName=CLSID
        wstring sub = f.location.substr(5);   // 去 HKLM\
        RegBackupValue(HKEY_LOCAL_MACHINE, sub, f.valName, &be);
        batch.push_back(be);
        HKEY k;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, sub.c_str(), 0, KEY_SET_VALUE, &k) == ERROR_SUCCESS) {
            LSTATUS d = RegDeleteValueW(k, f.valName.c_str());
            RegCloseKey(k);
            if (d != ERROR_SUCCESS) return L"移除扩展注册失败：" + ErrorTextZh(d);
        }
        break;
    }
    case F_IME: {
        // location = HKLM\SYSTEM\...\Keyboard Layouts\<id>；备份 Layout File/Text 后删整键
        wstring sub = f.location.substr(5);
        BackupEntry bf = be;
        bf.valName = L"Layout File";
        RegBackupValue(HKEY_LOCAL_MACHINE, sub, L"Layout File", &bf);
        batch.push_back(bf);
        BackupEntry bt = be;
        bt.valName = L"Layout Text";
        RegBackupValue(HKEY_LOCAL_MACHINE, sub, L"Layout Text", &bt);
        batch.push_back(bt);
        LSTATUS d = RegDeleteTreeW(HKEY_LOCAL_MACHINE, sub.c_str());
        if (d != ERROR_SUCCESS) return L"删除输入法布局失败：" + ErrorTextZh(d);
        break;
    }
    case F_ASSOC: {
        // location = UserChoice 键全名；valName=ProgId
        wstring sub = f.location;
        HKEY root = HKEY_CURRENT_USER;
        if (sub.rfind(L"HKCU\\", 0) == 0) sub = sub.substr(5);
        else if (sub.rfind(L"HKLM\\", 0) == 0) { sub = sub.substr(5); root = HKEY_LOCAL_MACHINE; }
        RegBackupValue(root, sub, f.valName, &be);
        batch.push_back(be);
        LSTATUS d = RegDeleteTreeW(root, sub.c_str());
        if (d != ERROR_SUCCESS) return L"恢复关联失败：" + ErrorTextZh(d);
        break;
    }
    case F_BEXT: {
        // path = 扩展目录；整目录移入隔离区
        CreateDirectoryW(QuarantineDir().c_str(), nullptr);
        wstring qname = L"bext-" + f.name + L"-" + U32(GetTickCount());
        wstring dst = QuarantineDir() + L"\\" + qname;
        if (!MoveFileExW(f.path.c_str(), dst.c_str(), MOVEFILE_REPLACE_EXISTING))
            return L"隔离扩展目录失败：" + ErrorTextZh(GetLastError());
        be.extra = dst;
        batch.push_back(be);
        break;
    }
    case F_HOSTS: {
        // location = 行内容；处置 = 重写 hosts 去掉该行；备份 = 整文件复制
        wchar_t winDir[MAX_PATH]{};
        GetWindowsDirectoryW(winDir, MAX_PATH);
        wstring hosts = wstring(winDir) + L"\\System32\\drivers\\etc\\hosts";
        CreateDirectoryW(QuarantineDir().c_str(), nullptr);
        wstring bak = QuarantineDir() + L"\\hosts-" + U32(GetTickCount()) + L".bak";
        if (!CopyFileW(hosts.c_str(), bak.c_str(), TRUE))
            return L"备份 hosts 失败：" + ErrorTextZh(GetLastError());
        bool ok = false;
        wstring text = ReadTextSmart(hosts, &ok);
        if (!ok) return L"读取 hosts 失败";
        wstring out;
        size_t start = 0;
        int removed = 0;
        while (start <= text.size()) {
            size_t end = text.find(L'\n', start);
            if (end == wstring::npos) end = text.size();
            wstring line = text.substr(start, end - start);
            wstring lineTrim = line;
            while (!lineTrim.empty() && (lineTrim.back() == L'\r' || lineTrim.back() == L' ')) lineTrim.pop_back();
            if (lineTrim == f.location) ++removed;      // 命中行剔除
            else out += line + (end == text.size() ? L"" : L"\n");
            if (end == text.size()) break;
            start = end + 1;
        }
        HANDLE h = CreateFileW(hosts.c_str(), GENERIC_WRITE, 0, nullptr,
                               CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) return L"写 hosts 失败：" + ErrorTextZh(GetLastError());
        std::string u8 = WToUtf8(out);
        DWORD w = 0;
        WriteFile(h, u8.data(), (DWORD)u8.size(), &w, nullptr);
        CloseHandle(h);
        be.extra = bak;
        batch.push_back(be);
        break;
    }
    case F_PROXY: {
        const wchar_t* sub = L"Software\\Microsoft\\Windows\\CurrentVersion\\Internet Settings";
        BackupEntry b1 = be;
        b1.valName = L"ProxyEnable";
        RegBackupValue(HKEY_CURRENT_USER, sub, L"ProxyEnable", &b1);
        batch.push_back(b1);
        BackupEntry b2 = be;
        b2.valName = L"ProxyServer";
        RegBackupValue(HKEY_CURRENT_USER, sub, L"ProxyServer", &b2);
        batch.push_back(b2);
        HKEY k;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, sub, 0, KEY_SET_VALUE, &k) == ERROR_SUCCESS) {
            DWORD zero = 0;
            RegSetValueExW(k, L"ProxyEnable", 0, REG_DWORD, (const BYTE*)&zero, sizeof(zero));
            RegCloseKey(k);
        }
        break;
    }
    default:
        return L"未知发现类型";
    }

    if (!batch.empty()) {
        wstring note = wstring(KindActionName(f.kind)) + L"：" + f.name +
                       (f.hit.empty() ? wstring() : (L"（" + f.hit + L"）"));
        if (!AppendBackup(batch, note, &err)) {
            // 备份失败不阻断（处置已执行），但日志必须显眼
            return L"已执行但备份记录失败：" + err;
        }
    }
    return wstring();
}

int UndoAll(ModuleContext& ctx, int* failCount) {
    vector<BackupEntry> all = ReadBackupAll();
    int okCount = 0, fails = 0;
    // 倒序还原
    for (auto it = all.rbegin(); it != all.rend(); ++it) {
        const BackupEntry& e = *it;
        bool ok = false;
        wstring logDetail;
        switch (e.kind) {
        case F_PROCESS:
            ctx.Log(L"  跳过: 进程终结不可自动还原（" + e.item + L"），如需恢复请重装该软件");
            ok = true;   // 视为已处理（无操作）
            break;
        case F_SERVICE:
        case F_DRIVER:
        case F_KMOD: {
            DWORD start = (DWORD)_wtoi(e.data.c_str());
            ok = start && ServiceSetStart(e.item, start, nullptr);
            logDetail = L"恢复服务启动类型=" + e.data;
            break;
        }
        case F_HIDPROC:
            // 隐藏进程结束不可还原（进程已不存在）；仅记录
            ok = true;
            logDetail = L"隐藏进程结束不可逆（已记录）";
            break;
        case F_RUN: {
            if (e.extra.rfind(L"LNK|", 0) == 0) {
                // lnk：extra 是隔离后路径 → location 存的原路径，移回
                wstring q = e.extra.substr(4);
                ok = MoveFileExW(q.c_str(), e.location.c_str(), MOVEFILE_REPLACE_EXISTING) != FALSE;
                logDetail = L"移回启动文件夹";
            } else {
                HKEY root = (e.location.rfind(L"HKCU\\", 0) == 0) ? HKEY_CURRENT_USER
                                                                  : HKEY_LOCAL_MACHINE;
                wstring sub = e.location.substr(5);
                ok = RegRestoreValue(root, sub, e);
                logDetail = L"写回注册表值";
            }
            break;
        }
        case F_TASK: {
            TaskCom tc;
            if (!tc.Init()) { ctx.Log(L"  失败: " + tc.error); ++fails; continue; }
            size_t slash = e.location.find_last_of(L'\\');
            wstring name = (slash == wstring::npos) ? e.location : e.location.substr(slash + 1);
            wstring parent = (slash == wstring::npos) ? L"\\" : e.location.substr(0, slash);
            BSTR pb = SysAllocString(parent.c_str());
            ITaskFolder* folder = nullptr;
            if (SUCCEEDED(tc.svc->GetFolder(pb, &folder)) && folder) {
                BSTR nb = SysAllocString(name.c_str());
                BSTR xb = SysAllocString(e.extra.c_str());
                VARIANT v;
                VariantInit(&v);
                IRegisteredTask* nt = nullptr;
                HRESULT hr = folder->RegisterTask(nb, xb, TASK_CREATE_OR_UPDATE,
                                                  v, v, TASK_LOGON_NONE, v, &nt);
                VariantClear(&v);
                if (nt) nt->Release();
                SysFreeString(nb); SysFreeString(xb);
                ok = SUCCEEDED(hr);
                if (!ok) logDetail = HrText(hr);
                folder->Release();
            }
            SysFreeString(pb);
            break;
        }
        case F_SHELLEX: {
            wstring sub = e.location.substr(5);
            ok = RegRestoreValue(HKEY_LOCAL_MACHINE, sub, e);
            break;
        }
        case F_IME: {
            // 两条备份（Layout File / Layout Text）都写回即视为成功
            wstring sub = e.location.substr(5);
            ok = RegRestoreValue(HKEY_LOCAL_MACHINE, sub, e);
            break;
        }
        case F_ASSOC: {
            wstring sub = e.location;
            HKEY root = HKEY_CURRENT_USER;
            if (sub.rfind(L"HKCU\\", 0) == 0) sub = sub.substr(5);
            else if (sub.rfind(L"HKLM\\", 0) == 0) { sub = sub.substr(5); root = HKEY_LOCAL_MACHINE; }
            ok = RegRestoreValue(root, sub, e);
            break;
        }
        case F_BEXT: {
            ok = MoveFileExW(e.extra.c_str(), e.location.c_str(), MOVEFILE_REPLACE_EXISTING) != FALSE;
            break;
        }
        case F_HOSTS: {
            wchar_t winDir[MAX_PATH]{};
            GetWindowsDirectoryW(winDir, MAX_PATH);
            wstring hosts = wstring(winDir) + L"\\System32\\drivers\\etc\\hosts";
            ok = CopyFileW(e.extra.c_str(), hosts.c_str(), FALSE) != FALSE;
            break;
        }
        case F_PROXY: {
            const wchar_t* sub = L"Software\\Microsoft\\Windows\\CurrentVersion\\Internet Settings";
            ok = RegRestoreValue(HKEY_CURRENT_USER, sub, e);
            break;
        }
        }
        if (ok) {
            ++okCount;
            if (!logDetail.empty())
                ctx.Log(L"  已还原: " + wstring(KindName(e.kind)) + L" " + e.item +
                        L"（" + logDetail + L"）");
        } else {
            ++fails;
            ctx.Log(L"  失败: " + wstring(KindName(e.kind)) + L" " + e.item +
                    (logDetail.empty() ? wstring() : (L" — " + logDetail)));
        }
    }
    if (failCount) *failCount = fails;
    return okCount;
}

// ---------------------------------------------------------------- 枚举（11 类）

namespace {

// 通用落表：命中 → Finding（一处构造保证字段一致）
void AddHit(vector<Finding>& out, int kind, const wstring& name, const wstring& path,
            const wstring& location, const wstring& valName, const wstring& hitText,
            const wstring& signer, const wstring& extra = wstring(),
            bool advisory = false) {
    Finding f;
    f.kind = kind;
    f.name = name;
    f.path = path;
    f.location = location;
    f.valName = valName;
    f.hit = hitText;
    // hitType：从命中描述提取类别字
    if (hitText.rfind(L"签名命中", 0) == 0) f.hitType = L"签名";
    else if (hitText.rfind(L"目录命中", 0) == 0) f.hitType = L"目录";
    else f.hitType = L"劫持";
    f.signer = signer;
    f.extra = extra;
    f.advisory = advisory;
    out.push_back(std::move(f));
}

// ① 进程（Toolhelp 快照 + QueryFullProcessImageName）
void EnumProcessesPurg(const Ruleset& rs, ModuleContext& ctx, vector<Finding>& out) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) { ctx.Log(L"  进程枚举失败：" + ErrorTextZh(GetLastError())); return; }
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    if (Process32FirstW(snap, &pe)) {
        int checked = 0;
        do {
            if (ctx.Cancelled()) break;
            HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_TERMINATE,
                                   FALSE, pe.th32ProcessID);
            if (!h) continue;
            wchar_t buf[1024] = {};
            DWORD len = 1024;
            wstring path;
            if (QueryFullProcessImageNameW(h, 0, buf, &len) && len > 0) path.assign(buf, len);
            CloseHandle(h);
            if (path.empty()) continue;
            ++checked;
            wstring signer;
            wstring hit = CheckFile(rs, path, &signer);
            if (!hit.empty())
                AddHit(out, F_PROCESS, pe.szExeFile, path, L"PID " + U32(pe.th32ProcessID),
                       wstring(), hit, signer, U32(pe.th32ProcessID));
        } while (Process32NextW(snap, &pe));
        ctx.Log(L"  进程：检查 " + U32(checked) + L" 个");
    }
    CloseHandle(snap);
}

// 服务/驱动共用：ImagePath 读注册表 + 规范化
void EnumServicesPurg(const Ruleset& rs, ModuleContext& ctx, vector<Finding>& out, bool driver) {
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_ENUMERATE_SERVICE);
    if (!scm) { ctx.Log(L"  服务枚举失败：" + ErrorTextZh(GetLastError())); return; }
    DWORD need = 0, returned = 0, resume = 0;
    EnumServicesStatusExW(scm, SC_ENUM_PROCESS_INFO,
                          driver ? (SERVICE_KERNEL_DRIVER | SERVICE_FILE_SYSTEM_DRIVER | SERVICE_RECOGNIZER_DRIVER)
                                 : SERVICE_WIN32,
                          SERVICE_STATE_ALL, nullptr, 0, &need, &returned, &resume, nullptr);
    if (need == 0) { CloseServiceHandle(scm); return; }
    vector<BYTE> buf(need);
    if (!EnumServicesStatusExW(scm, SC_ENUM_PROCESS_INFO,
                               driver ? (SERVICE_KERNEL_DRIVER | SERVICE_FILE_SYSTEM_DRIVER | SERVICE_RECOGNIZER_DRIVER)
                                      : SERVICE_WIN32,
                               SERVICE_STATE_ALL, buf.data(), need,
                               &need, &returned, &resume, nullptr)) {
        CloseServiceHandle(scm);
        return;
    }
    auto* arr = reinterpret_cast<ENUM_SERVICE_STATUS_PROCESSW*>(buf.data());
    int checked = 0;
    for (DWORD i = 0; i < returned && !ctx.Cancelled(); ++i) {
        const wchar_t* svcName = arr[i].lpServiceName;
        wstring sub = wstring(L"SYSTEM\\CurrentControlSet\\Services\\") + svcName;
        wstring imagePath = RegGetSz(HKEY_LOCAL_MACHINE, sub, L"ImagePath");
        if (imagePath.empty()) continue;
        // 规范化：\??\ 前缀、SystemRoot 展开、引号、参数剥离
        wstring p = ExpandEnv(imagePath);
        if (p.rfind(L"\\??\\", 0) == 0) p = p.substr(4);
        // \SystemRoot\ 与 SystemRoot\ 两种写法（注册表常带前导反斜杠）
        if (p.rfind(L"\\SystemRoot\\", 0) == 0) p = p.substr(1);
        if (p.rfind(L"SystemRoot\\", 0) == 0) {
            wchar_t winDir[MAX_PATH]{};
            GetWindowsDirectoryW(winDir, MAX_PATH);
            p = wstring(winDir) + L"\\" + p.substr(11);
        }
        if (p.rfind(L"\\Windows\\", 0) == 0 || p.rfind(L"Windows\\", 0) == 0) {
            // 部分驱动是相对系统盘根的 Windows\... 写法
            wchar_t sysDrive[MAX_PATH]{};
            if (GetEnvironmentVariableW(L"SystemDrive", sysDrive, MAX_PATH) &&
                p[0] != sysDrive[0]) {
                p = wstring(sysDrive) + L"\\" + p;
            }
        }
        if (!p.empty() && p[0] == L'"') {
            size_t e = p.find(L'"', 1);
            if (e != wstring::npos) p = p.substr(1, e - 1);
        } else {
            p = ExeFromCommandLine(p);
        }
        if (p.empty() || !FileExists(p)) continue;
        ++checked;
        wstring signer;
        wstring hit = CheckFile(rs, p, &signer);
        if (!hit.empty())
            AddHit(out, driver ? F_DRIVER : F_SERVICE, svcName, p, sub, wstring(),
                   hit, signer, wstring());
    }
    ctx.Log(driver ? L"  驱动：检查 " + U32(checked) + L" 个（含未运行的）"
                   : L"  服务：检查 " + U32(checked) + L" 个");
    CloseServiceHandle(scm);
}

// ③ 启动项：Run/RunOnce 注册表（6 键）+ 启动文件夹 lnk
void EnumRunItems(const Ruleset& rs, ModuleContext& ctx, vector<Finding>& out) {
    struct RunKey { HKEY root; const wchar_t* sub; const wchar_t* rootName; };
    static const RunKey keys[] = {
        { HKEY_CURRENT_USER,  L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",      L"HKCU" },
        { HKEY_CURRENT_USER,  L"Software\\Microsoft\\Windows\\CurrentVersion\\RunOnce",  L"HKCU" },
        { HKEY_LOCAL_MACHINE, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",      L"HKLM" },
        { HKEY_LOCAL_MACHINE, L"Software\\Microsoft\\Windows\\CurrentVersion\\RunOnce",  L"HKLM" },
        { HKEY_LOCAL_MACHINE, L"Software\\Wow6432Node\\Microsoft\\Windows\\CurrentVersion\\Run", L"HKLM" },
    };
    int checked = 0;
    for (const RunKey& rk : keys) {
        HKEY k;
        if (RegOpenKeyExW(rk.root, rk.sub, 0, KEY_QUERY_VALUE, &k) != ERROR_SUCCESS) continue;
        wstring fullKey = wstring(rk.rootName) + L"\\" + rk.sub;
        DWORD idx = 0;
        wchar_t valName[256];
        DWORD nameLen = 256;
        BYTE data[4096];
        DWORD dataLen = sizeof(data), type = 0;
        while (RegEnumValueW(k, idx++, valName, &nameLen, nullptr, &type, data, &dataLen) == ERROR_SUCCESS) {
            if (ctx.Cancelled()) break;
            if ((type == REG_SZ || type == REG_EXPAND_SZ) && dataLen >= sizeof(wchar_t)) {
                wstring cmd((wchar_t*)data);
                wstring exe = ExeFromCommandLine(cmd);
                if (!exe.empty()) {
                    ++checked;
                    wstring signer;
                    wstring hit = CheckFile(rs, exe, &signer);
                    if (!hit.empty())
                        AddHit(out, F_RUN, valName, exe, fullKey, valName, hit, signer, cmd);
                }
            }
            nameLen = 256; dataLen = sizeof(data);
        }
        RegCloseKey(k);
    }

    // 启动文件夹（用户 + 公共）
    ComSession com;
    com.Init();
    auto scanFolder = [&](int csidl) {
        wchar_t dir[MAX_PATH]{};
        if (FAILED(SHGetFolderPathW(nullptr, csidl, nullptr, 0, dir))) return;
        wstring pat = wstring(dir) + L"\\*.lnk";
        WIN32_FIND_DATAW fd{};
        HANDLE f = FindFirstFileW(pat.c_str(), &fd);
        if (f == INVALID_HANDLE_VALUE) return;
        do {
            if (ctx.Cancelled()) break;
            wstring lnk = wstring(dir) + L"\\" + fd.cFileName;
            // IShellLink 解析目标
            IShellLinkW* sl = nullptr;
            if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
                                        IID_IShellLinkW, (void**)&sl)) || !sl)
                continue;
            IPersistFile* pf = nullptr;
            wstring target;
            if (SUCCEEDED(sl->QueryInterface(IID_IPersistFile, (void**)&pf)) && pf) {
                if (SUCCEEDED(pf->Load(lnk.c_str(), STGM_READ))) {
                    wchar_t tb[MAX_PATH * 2] = {};
                    if (SUCCEEDED(sl->GetPath(tb, MAX_PATH * 2, nullptr, SLGP_RAWPATH)) && tb[0])
                        target = tb;
                }
                pf->Release();
            }
            sl->Release();
            if (target.empty() || !FileExists(target)) continue;
            wstring signer;
            wstring hit = CheckFile(rs, target, &signer);
            if (!hit.empty())
                AddHit(out, F_RUN, fd.cFileName, target, dir, wstring(), hit, signer, L"LNK|" + lnk);
        } while (FindNextFileW(f, &fd));
        FindClose(f);
    };
    scanFolder(CSIDL_STARTUP);
    scanFolder(CSIDL_COMMON_STARTUP);
    ctx.Log(L"  启动项：检查 " + U32(checked) + L" 个注册表项 + 2 个启动文件夹");
}

// ④ 计划任务（ITaskService 递归；\Microsoft\ 官方跳过）
void EnumTasksPurg(const Ruleset& rs, ModuleContext& ctx, vector<Finding>& out) {
    ComSession com;
    if (!com.Init()) { ctx.Log(L"  计划任务：COM 初始化失败"); return; }
    ITaskService* svc = nullptr;
    if (FAILED(CoCreateInstance(kCLSID_TaskScheduler, nullptr, CLSCTX_INPROC_SERVER,
                                IID_ITaskService, (void**)&svc)) || !svc)
        return;
    VARIANT v;
    VariantInit(&v);
    bool connected = SUCCEEDED(svc->Connect(v, v, v, v));
    VariantClear(&v);
    if (!connected) { svc->Release(); return; }

    int checked = 0;
    // 递归 lambda
    std::function<void(ITaskFolder*, const wstring&)> walk = [&](ITaskFolder* folder, const wstring& prefix) {
        if (ctx.Cancelled()) return;
        // 本层任务
        IRegisteredTaskCollection* tasks = nullptr;
        if (SUCCEEDED(folder->GetTasks(TASK_ENUM_HIDDEN, &tasks)) && tasks) {
            LONG n = 0;
            tasks->get_Count(&n);
            for (LONG i = 1; i <= n; ++i) {
                if (ctx.Cancelled()) break;
                VARIANT vi;
                VariantInit(&vi);
                vi.vt = VT_I4; vi.lVal = i;
                IRegisteredTask* task = nullptr;
                if (FAILED(tasks->get_Item(vi, &task)) || !task) { VariantClear(&vi); continue; }
                VariantClear(&vi);
                BSTR pb = nullptr;
                if (FAILED(task->get_Path(&pb)) || !pb) { task->Release(); continue; }
                wstring fullPath = TakeBstr(pb);
                if (fullPath.rfind(L"\\Microsoft\\", 0) == 0 || fullPath == L"\\") {
                    task->Release(); continue;
                }
                // 动作 → Exec 路径
                ITaskDefinition* def = nullptr;
                if (SUCCEEDED(task->get_Definition(&def)) && def) {
                    IActionCollection* acts = nullptr;
                    if (SUCCEEDED(def->get_Actions(&acts)) && acts) {
                        LONG an = 0;
                        acts->get_Count(&an);
                        for (LONG a = 1; a <= an; ++a) {
                            IAction* action = nullptr;
                            if (SUCCEEDED(acts->get_Item(a, &action)) && action) {
                                TASK_ACTION_TYPE at;
                                if (SUCCEEDED(action->get_Type(&at)) && at == TASK_ACTION_EXEC) {
                                    IExecAction* ex = nullptr;
                                    if (SUCCEEDED(action->QueryInterface(IID_IExecAction, (void**)&ex)) && ex) {
                                        BSTR epb = nullptr;
                                        if (SUCCEEDED(ex->get_Path(&epb)) && epb) {
                                            wstring exe = TakeBstr(epb);
                                            exe = ExpandEnv(exe);
                                            if (!exe.empty() && FileExists(exe)) {
                                                ++checked;
                                                wstring signer;
                                                wstring hit = CheckFile(rs, exe, &signer);
                                                if (!hit.empty())
                                                    AddHit(out, F_TASK, fullPath, exe,
                                                           fullPath, wstring(), hit, signer);
                                            }
                                        }
                                        ex->Release();
                                    }
                                }
                                action->Release();
                            }
                        }
                        acts->Release();
                    }
                    def->Release();
                }
                task->Release();
            }
            tasks->Release();
        }
        // 子文件夹
        ITaskFolderCollection* subs = nullptr;
        if (SUCCEEDED(folder->GetFolders(0, &subs)) && subs) {
            LONG n = 0;
            subs->get_Count(&n);
            for (LONG i = 1; i <= n; ++i) {
                VARIANT vi;
                VariantInit(&vi);
                vi.vt = VT_I4; vi.lVal = i;
                ITaskFolder* sub = nullptr;
                if (SUCCEEDED(subs->get_Item(vi, &sub)) && sub) {
                    walk(sub, prefix + L"\\");
                    sub->Release();
                }
                VariantClear(&vi);
            }
            subs->Release();
        }
    };
    ITaskFolder* root = nullptr;
    BSTR rb = SysAllocString(L"\\");
    if (SUCCEEDED(svc->GetFolder(rb, &root)) && root) {
        walk(root, L"\\");
        root->Release();
    }
    SysFreeString(rb);
    ctx.Log(L"  计划任务：检查 " + U32(checked) + L" 个（官方 \\Microsoft\\ 已跳过）");
    svc->Release();
}

// ⑤ 右键菜单扩展（Approved CLSID → InprocServer32）
void EnumShellEx(const Ruleset& rs, ModuleContext& ctx, vector<Finding>& out) {
    struct Root { const wchar_t* sub; const wchar_t* full; };
    static const Root roots[] = {
        { L"Software\\Microsoft\\Windows\\CurrentVersion\\Shell Extensions\\Approved",
          L"HKLM\\Software\\Microsoft\\Windows\\CurrentVersion\\Shell Extensions\\Approved" },
        { L"Software\\Wow6432Node\\Microsoft\\Windows\\CurrentVersion\\Shell Extensions\\Approved",
          L"HKLM\\Software\\Wow6432Node\\Microsoft\\Windows\\CurrentVersion\\Shell Extensions\\Approved" },
    };
    int checked = 0;
    for (const Root& r : roots) {
        HKEY k;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, r.sub, 0, KEY_QUERY_VALUE, &k) != ERROR_SUCCESS) continue;
        DWORD idx = 0;
        wchar_t valName[128];
        DWORD nameLen = 128;
        wchar_t data[512];
        DWORD dataLen = sizeof(data), type = 0;
        while (RegEnumValueW(k, idx++, valName, &nameLen, nullptr, &type,
                             (BYTE*)data, &dataLen) == ERROR_SUCCESS) {
            if (ctx.Cancelled()) break;
            nameLen = 128; dataLen = sizeof(data);
            // valName 应为 CLSID（{...}）
            if (wcsncmp(valName, L"{", 1) != 0) continue;
            wstring clsid = valName;
            wstring dll = RegGetSz(HKEY_CLASSES_ROOT,
                                   L"CLSID\\" + clsid + L"\\InprocServer32", wstring());
            if (dll.empty())
                dll = RegGetSz(HKEY_CLASSES_ROOT,
                               L"Wow6432Node\\CLSID\\" + clsid + L"\\InprocServer32", wstring());
            dll = ExpandEnv(dll);
            if (dll.empty() || !FileExists(dll)) continue;
            ++checked;
            wstring signer;
            wstring hit = CheckFile(rs, dll, &signer);
            if (!hit.empty())
                AddHit(out, F_SHELLEX, wstring(data), dll, r.full, clsid, hit, signer);
        }
        RegCloseKey(k);
    }
    ctx.Log(L"  右键菜单：检查 " + U32(checked) + L" 个已注册扩展");
}

// ⑥ 输入法布局
void EnumImeLayouts(const Ruleset& rs, ModuleContext& ctx, vector<Finding>& out) {
    const wchar_t* base = L"SYSTEM\\CurrentControlSet\\Control\\Keyboard Layouts";
    HKEY k;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, base, 0, KEY_ENUMERATE_SUB_KEYS, &k) != ERROR_SUCCESS) return;
    int checked = 0;
    DWORD idx = 0;
    wchar_t subName[64];
    DWORD subLen = 64;
    while (RegEnumKeyExW(k, idx++, subName, &subLen, nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS) {
        if (ctx.Cancelled()) break;
        subLen = 64;
        wstring sub = wstring(base) + L"\\" + subName;
        wstring layoutFile = RegGetSz(HKEY_LOCAL_MACHINE, sub, L"Layout File");
        if (layoutFile.empty() || wcsstr(layoutFile.c_str(), L"kbd") != layoutFile.c_str()) {
            // 流氓输入法不一定以 kbd 开头——仍检查；仅空值跳过
            if (layoutFile.empty()) { continue; }
        }
        wchar_t winDir[MAX_PATH]{};
        GetWindowsDirectoryW(winDir, MAX_PATH);
        wstring full = wstring(winDir) + L"\\System32\\" + layoutFile;
        if (!FileExists(full)) continue;
        ++checked;
        wstring signer;
        wstring hit = CheckFile(rs, full, &signer);
        if (!hit.empty()) {
            wstring text = RegGetSz(HKEY_LOCAL_MACHINE, sub, L"Layout Text");
            AddHit(out, F_IME, text.empty() ? subName : text, full, sub, wstring(), hit, signer);
        }
    }
    RegCloseKey(k);
    ctx.Log(L"  输入法：检查 " + U32(checked) + L" 个布局文件");
}

// ⑦ 文件关联劫持（FileExts UserChoice + 常用扩展 HKCR open command）
void EnumAssociations(const Ruleset& rs, ModuleContext& ctx, vector<Finding>& out) {
    static const wchar_t* kExts[] = {
        L".txt", L".htm", L".html", L".url", L".pdf", L".jpg", L".png",
        L".mp4", L".mp3", L".doc", L".xls", L".zip",
    };
    int checked = 0;
    for (const wchar_t* ext : kExts) {
        if (ctx.Cancelled()) break;
        // 1) HKCU UserChoice → ProgId → open command
        wstring uc = wstring(L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\FileExts\\") + ext + L"\\UserChoice";
        wstring progId = RegGetSz(HKEY_CURRENT_USER, uc, L"ProgId");
        if (!progId.empty()) {
            wstring cmd = RegGetSz(HKEY_CLASSES_ROOT, progId + L"\\shell\\open\\command", wstring());
            wstring exe = ExeFromCommandLine(cmd);
            if (!exe.empty() && FileExists(exe)) {
                ++checked;
                wstring signer;
                wstring hit = CheckFile(rs, exe, &signer);
                if (!hit.empty())
                    AddHit(out, F_ASSOC, wstring(ext) + L" → " + progId, exe,
                           wstring(L"HKCU\\") + uc, L"ProgId", hit, signer, progId);
            }
        }
        // 2) HKCR\<ext>\shell\open\command 全局默认
        wstring gcmd = RegGetSz(HKEY_CLASSES_ROOT, wstring(ext) + L"\\shell\\open\\command", wstring());
        wstring gexe = ExeFromCommandLine(gcmd);
        if (!gexe.empty() && FileExists(gexe)) {
            ++checked;
            wstring signer;
            wstring hit = CheckFile(rs, gexe, &signer);
            if (!hit.empty())
                AddHit(out, F_ASSOC, wstring(ext) + L"（全局）", gexe,
                       wstring(L"HKCR\\") + ext + L"\\shell\\open\\command", wstring(), hit, signer, gcmd);
        }
    }
    ctx.Log(L"  文件关联：检查 " + U32(checked) + L" 个扩展名的打开命令");
}

// ⑧ 浏览器扩展（Chrome / Edge，命中特征才列报）
void EnumBrowserExts(const Ruleset& rs, ModuleContext& ctx, vector<Finding>& out) {
    wchar_t local[MAX_PATH]{};
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, local))) return;
    struct Browser { const wchar_t* rel; const wchar_t* label; };
    static const Browser browsers[] = {
        { L"Google\\Chrome\\User Data",  L"Chrome" },
        { L"Microsoft\\Edge\\User Data", L"Edge" },
    };
    int total = 0;
    for (const Browser& b : browsers) {
        wstring userData = wstring(local) + L"\\" + b.rel;
        // 枚举 profile 目录（Default / Profile 1 / ...）
        wstring pat = userData + L"\\*";
        WIN32_FIND_DATAW fdp{};
        HANDLE fp = FindFirstFileW(pat.c_str(), &fdp);
        if (fp == INVALID_HANDLE_VALUE) continue;
        do {
            if (!(fdp.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
            if (fdp.cFileName[0] == L'.') continue;
            wstring extBase = userData + L"\\" + fdp.cFileName + L"\\Extensions";
            DWORD a = GetFileAttributesW(extBase.c_str());
            if (a == INVALID_FILE_ATTRIBUTES || !(a & FILE_ATTRIBUTE_DIRECTORY)) continue;
            // 枚举扩展 ID
            wstring pid = extBase + L"\\*";
            WIN32_FIND_DATAW fdi{};
            HANDLE fi = FindFirstFileW(pid.c_str(), &fdi);
            if (fi == INVALID_HANDLE_VALUE) continue;
            do {
                if (ctx.Cancelled()) break;
                if (!(fdi.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
                if (fdi.cFileName[0] == L'.') continue;
                wstring extDir = extBase + L"\\" + fdi.cFileName;
                // 取最新版本目录
                wstring verPat = extDir + L"\\*";
                WIN32_FIND_DATAW fdv{};
                HANDLE fv = FindFirstFileW(verPat.c_str(), &fdv);
                wstring newest;
                if (fv != INVALID_HANDLE_VALUE) {
                    do {
                        if (!(fdv.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
                        wstring ver = fdv.cFileName;
                        if (ver > newest) newest = ver;
                    } while (FindNextFileW(fv, &fdv));
                    FindClose(fv);
                }
                if (newest.empty()) continue;
                wstring extFull = extDir + L"\\" + newest;
                ++total;
                // 目录路径特征 + manifest 内容特征（读文本做目录特征匹配）
                wstring hit = CheckFile(rs, extFull, nullptr);
                if (hit.empty()) {
                    // manifest.json 里的 name/update_url 也可能是特征（如推广域名）
                    wstring manifest = extFull + L"\\manifest.json";
                    bool ok = false;
                    wstring text = ReadTextSmart(manifest, &ok);
                    if (ok) {
                        // 目录特征对 manifest 内容无意义；只做签名类特征不适用。
                        // M1：跳过内容匹配（避免中文扩展名误报），仅目录命中。
                    }
                }
                if (!hit.empty()) {
                    // 从 manifest 提取显示名
                    wstring name = wstring(b.label) + L" 扩展 " + fdi.cFileName;
                    wstring manifest = extFull + L"\\manifest.json";
                    bool ok = false;
                    wstring text = ReadTextSmart(manifest, &ok);
                    if (ok) {
                        size_t q = text.find(L"\"name\"");
                        if (q != wstring::npos) {
                            size_t c1 = text.find(L'"', q + 6);
                            if (c1 != wstring::npos) {
                                size_t c2 = text.find(L'"', c1 + 1);
                                if (c2 != wstring::npos && c2 > c1 + 1)
                                    name = wstring(b.label) + L" " + text.substr(c1 + 1, c2 - c1 - 1);
                            }
                        }
                    }
                    AddHit(out, F_BEXT, name, extDir, extFull, wstring(), hit, wstring());
                }
            } while (FindNextFileW(fi, &fdi));
            FindClose(fi);
        } while (FindNextFileW(fp, &fdp));
        FindClose(fp);
    }
    ctx.Log(L"  浏览器扩展：共 " + U32(total) + L" 个，命中特征者列出");
}

// ⑨ hosts 劫持（内置知名流氓域名 + 特征库目录规则不适用 → 专用小表）
void EnumHosts(const Ruleset& rs, ModuleContext& ctx, vector<Finding>& out) {
    static const wchar_t* kHijackDomains[] = {
        L"2345.com", L"hao.360.cn", L"hao.360.com", L"114la.com", L"dh.sogou.com",
        L"hao.qqu.cc", L"123coco", L"6789.com", L"5w.com", L"jiedai.cn",
        L"taobao.com.baidushurufa", L"www.2345", L"2345.cc", L"lantern.so",
        L"my123.com", L"9420.cc", L"0888.win", L"qjkk.cc",
    };
    wchar_t winDir[MAX_PATH]{};
    GetWindowsDirectoryW(winDir, MAX_PATH);
    wstring hosts = wstring(winDir) + L"\\System32\\drivers\\etc\\hosts";
    bool ok = false;
    wstring text = ReadTextSmart(hosts, &ok);
    if (!ok) { ctx.Log(L"  hosts：读取失败（可能无权限）"); return; }
    int checked = 0, lines = 0;
    size_t start = 0;
    while (start <= text.size()) {
        size_t end = text.find(L'\n', start);
        if (end == wstring::npos) end = text.size();
        wstring line = text.substr(start, end - start);
        wstring lineTrim = line;
        while (!lineTrim.empty() && (lineTrim.back() == L'\r' || lineTrim.back() == L' ')) lineTrim.pop_back();
        ++lines;
        if (!lineTrim.empty() && lineTrim[0] != L'#') {
            wstring low = FoldLower(lineTrim);
            for (const wchar_t* dom : kHijackDomains) {
                if (low.find(dom) != wstring::npos) {
                    AddHit(out, F_HOSTS, L"hosts 第 " + U32(lines) + L" 行", wstring(),
                           wstring(L"System32\\drivers\\etc\\hosts"), wstring(),
                           wstring(L"劫持域名：") + dom, wstring(), wstring());
                    ++checked;
                    break;
                }
            }
        }
        if (end == text.size()) break;
        start = end + 1;
    }
    ctx.Log(L"  hosts：检查 " + U32(lines) + L" 行，命中 " + U32(checked) + L" 条劫持");
}

// ⑩ 系统代理（提示行：全部列出，启用中可勾选关闭）
void EnumProxy(const Ruleset& rs, ModuleContext& ctx, vector<Finding>& out) {
    const wchar_t* sub = L"Software\\Microsoft\\Windows\\CurrentVersion\\Internet Settings";
    DWORD enable = 0, sz = sizeof(enable);
    RegGetValueW(HKEY_CURRENT_USER, sub, L"ProxyEnable", RRF_RT_REG_DWORD, nullptr, &enable, &sz);
    wstring server = RegGetSz(HKEY_CURRENT_USER, sub, L"ProxyServer");
    wstring pac = RegGetSz(HKEY_CURRENT_USER, sub, L"AutoConfigURL");
    if (!enable && server.empty() && pac.empty()) return;
    Finding f;
    f.kind = F_PROXY;
    f.advisory = true;
    f.name = L"系统代理（IE/WinINET）";
    f.hitType = L"提示";
    f.hit = enable ? L"已启用" : L"已配置未启用";
    f.location = L"HKCU\\Software\\Microsoft\\Windows\\CurrentVersion\\Internet Settings";
    f.path = !pac.empty() ? pac : server;
    out.push_back(std::move(f));
    ctx.Log(L"  代理：ProxyEnable=" + U32(enable) +
            (server.empty() ? wstring() : L" ProxyServer=" + server) +
            (pac.empty() ? wstring() : L" PAC=" + pac));
}

// ⑫ 已加载内核模块（v2.14 驱动增强）：NtQuerySystemInformation(SystemModuleInformation)
//    与注册表服务枚举互补 —— 服务注册表被隐藏/ImagePath 被清掉的驱动仍会出现在
//    内核模块表里。Win10/11 x64 布局。
#pragma pack(push, 8)
struct PurgRtlModuleInfo {
    HANDLE  Section;
    PVOID   MappedBase;
    PVOID   ImageBase;
    ULONG   ImageSize;
    ULONG   Flags;
    USHORT  LoadCount;
    USHORT  OffsetToFileName;
    UCHAR   FullPathName[256];
};
struct PurgModuleTable {
    ULONG NumberOfModules;
    PurgRtlModuleInfo Modules[1];
};
#pragma pack(pop)

void EnumKernelModules(const Ruleset& rs, ModuleContext& ctx, vector<Finding>& out) {
    typedef LONG(WINAPI* NtQSI_t)(HANDLE, ULONG, PVOID, ULONG, PULONG);
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    NtQSI_t NtQSI = ntdll ? (NtQSI_t)(void*)GetProcAddress(ntdll, "NtQuerySystemInformation") : nullptr;
    if (!NtQSI) { ctx.Log(L"  内核模块：ntdll 不可用，跳过"); return; }

    // 已被驱动/服务步骤报告过的路径（去重）
    std::unordered_map<wstring, bool> reported;
    for (const Finding& f : out)
        if (f.kind == F_DRIVER || f.kind == F_SERVICE) reported[FoldLower(f.path)] = true;

    ULONG need = 0;
    for (int retry = 0; retry < 3; ++retry) {
        vector<BYTE> buf(need == 0 ? (1 << 20) : need);
        ULONG got = 0;
        LONG st = NtQSI(GetCurrentProcess(), 11 /*SystemModuleInformation*/,
                        buf.data(), (ULONG)buf.size(), &got);
        if (st == 0) {
            auto* tbl = reinterpret_cast<PurgModuleTable*>(buf.data());
            int checked = 0, hitCount = 0;
            wchar_t winDir[MAX_PATH]{};
            GetWindowsDirectoryW(winDir, MAX_PATH);
            for (ULONG i = 0; i < tbl->NumberOfModules && !ctx.Cancelled(); ++i) {
                const PurgRtlModuleInfo& m = tbl->Modules[i];
                // FullPathName 是 ANSI 单字节数组，以 \0 结尾逐字节转宽
                wstring p;
                for (int ci = 0; ci < 256 && m.FullPathName[ci]; ++ci)
                    p += (wchar_t)m.FullPathName[ci];
                if (p.empty()) continue;
                if (p.rfind(L"\\SystemRoot\\", 0) == 0) p = wstring(winDir) + p.substr(11);
                else if (p.rfind(L"\\??\\", 0) == 0) p = p.substr(4);
                if (!FileExists(p)) continue;
                wstring low = FoldLower(p);
                if (reported.count(low)) continue;   // 服务枚举已覆盖
                ++checked;
                wstring signer;
                wstring hit = CheckFile(rs, p, &signer);
                if (!hit.empty()) {
                    ++hitCount;
                    size_t slash = p.find_last_of(L'\\');
                    wstring base = (slash == wstring::npos) ? p : p.substr(slash + 1);
                    wstring svcName = base;
                    size_t dot = svcName.rfind(L'.');
                    if (dot != wstring::npos) svcName = svcName.substr(0, dot);
                    // 服务键核对：注册表里连同名服务键都没有 → 更可疑
                    HKEY svcKey = nullptr;
                    bool noSvc = RegOpenKeyExW(HKEY_LOCAL_MACHINE,
                        (L"SYSTEM\\CurrentControlSet\\Services\\" + svcName).c_str(),
                        0, KEY_QUERY_VALUE, &svcKey) != ERROR_SUCCESS;
                    if (svcKey) RegCloseKey(svcKey);
                    wstring extra = L"服务名:" + svcName + (noSvc ? L"（注册表无服务键）" : L"");
                    AddHit(out, F_KMOD, base, p, L"内核模块表", wstring(),
                           hit + L"［NT 内核模块表］", signer, extra);
                    reported[low] = true;
                }
            }
            ctx.Log(L"  内核模块表：增量检查 " + U32(checked) + L" 个");
            return;
        }
        if (got == 0) break;
        need = got + (got >> 1);   // 缓冲不足 → 按 NT 返回的所需大小重试
    }
    ctx.Log(L"  内核模块：枚举失败（缓冲重试 3 次未成功）");
}

// ⑬ 隐藏进程检测（v2.14 驱动增强）：三源对比。
//    候选集 = NT 进程链表 ∪ 扩展句柄表 PID 集（SystemExtendedHandleInformation）；
//    快照集 = Toolhelp32。DKOM 摘链手法把进程从 EPROCESS 链摘掉后，
//    NtQuerySystemInformation(5) 与 Toolhelp 都看不到，但该进程持有的
//    内核句柄仍在句柄表里 —— 句柄表源是抓这类 rootkit 的关键。
//    候选集有、快照没有 = 疑似隐藏。
#pragma pack(push, 8)
struct PurgUS {
    USHORT Length;
    USHORT MaximumLength;
    ULONG  pad_;        // x64 对齐：UNICODE_STRING = 4 字节头 + 4 pad + 指针
    wchar_t* Buffer;
};
struct PurgSysProcInfo {
    ULONG NextEntryOffset;
    ULONG NumberOfThreads;
    LARGE_INTEGER WorkingSetPrivateSize;
    ULONG NumberOfThreadsHighWatermark;
    ULONG Spare0;
    LARGE_INTEGER CreateTime;
    LARGE_INTEGER UserTime;
    LARGE_INTEGER KernelTime;
    PurgUS ImageName;
    LONG BasePriority;
    HANDLE UniqueProcessId;
    HANDLE InheritedFromUniqueProcessId;
    ULONG HandleCount;
    ULONG SessionId;
};
struct PurgHandleEntryEx {
    PVOID    Object;
    ULONG_PTR ProcessId;
    ULONG_PTR Handle;
    ULONG     GrantedAccess;
    USHORT    CreatorBackTraceIndex;
    USHORT    ObjectTypeIndex;
    ULONG     HandleAttributes;
    ULONG     Reserved;
};
struct PurgHandleTableEx {
    ULONG_PTR NumberOfHandles;
    ULONG_PTR Reserved;
    PurgHandleEntryEx Entries[1];
};
#pragma pack(pop)

void EnumHiddenProcs(const Ruleset& rs, ModuleContext& ctx, vector<Finding>& out) {
    typedef LONG(WINAPI* NtQSI_t)(HANDLE, ULONG, PVOID, ULONG, PULONG);
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    NtQSI_t NtQSI = ntdll ? (NtQSI_t)(void*)GetProcAddress(ntdll, "NtQuerySystemInformation") : nullptr;
    if (!NtQSI) return;

    // 源 ①：NT 进程链表（PID → 名称）
    std::unordered_map<DWORD, wstring> ntPids;
    {
        ULONG need = 1 << 20;
        for (int retry = 0; retry < 3 && ntPids.empty(); ++retry) {
            vector<BYTE> buf(need);
            ULONG got = 0;
            if (NtQSI(GetCurrentProcess(), 5 /*SystemProcessInformation*/,
                      buf.data(), need, &got) != 0) {
                if (got == 0) break;
                need = got + (got >> 1);
                continue;
            }
            BYTE* p = buf.data();
            for (;;) {
                if (p + sizeof(PurgSysProcInfo) > buf.data() + buf.size()) break;
                auto* pi = reinterpret_cast<PurgSysProcInfo*>(p);
                DWORD pid = (DWORD)(ULONG_PTR)pi->UniqueProcessId;
                wstring name = (pi->ImageName.Buffer && pi->ImageName.Length)
                    ? wstring(pi->ImageName.Buffer, pi->ImageName.Length / 2) : wstring();
                if (pid > 4) ntPids[pid] = name;
                if (!pi->NextEntryOffset) break;
                p += pi->NextEntryOffset;
            }
        }
    }

    // 源 ②：扩展句柄表 PID 集
    std::unordered_map<DWORD, bool> handlePids;
    {
        ULONG need = 1 << 22;
        for (int retry = 0; retry < 3 && handlePids.empty(); ++retry) {
            vector<BYTE> buf(need);
            ULONG got = 0;
            if (NtQSI(GetCurrentProcess(), 64 /*SystemExtendedHandleInformation*/,
                      buf.data(), need, &got) != 0) {
                if (got == 0) break;
                need = got + (got >> 1);
                continue;
            }
            auto* tbl = reinterpret_cast<PurgHandleTableEx*>(buf.data());
            ULONG_PTR n = tbl->NumberOfHandles;
            // 防御：句柄数异常大直接截断（脏数据/布局不符保护）
            if (n > 2000000) n = 2000000;
            for (ULONG_PTR i = 0; i < n; ++i) {
                DWORD pid = (DWORD)tbl->Entries[i].ProcessId;
                if (pid > 4) handlePids[pid] = true;
            }
        }
    }
    if (ntPids.empty() && handlePids.empty()) {
        ctx.Log(L"  隐藏进程：NT 数据源不可用，跳过");
        return;
    }

    // 快照集：Toolhelp32
    std::unordered_map<DWORD, bool> snapPids;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap != INVALID_HANDLE_VALUE) {
        PROCESSENTRY32W pe{};
        pe.dwSize = sizeof(pe);
        if (Process32FirstW(snap, &pe)) {
            do { snapPids[pe.th32ProcessID] = true; } while (Process32NextW(snap, &pe));
        }
        CloseHandle(snap);
    }
    if (snapPids.empty()) { ctx.Log(L"  隐藏进程：Toolhelp 快照失败，跳过"); return; }

    int hidden = 0;
    auto checkPid = [&](DWORD pid, const wstring& name) {
        if (snapPids.count(pid)) return;
        ++hidden;
        wchar_t b[64];
        swprintf_s(b, 64, L"%lu", (unsigned long)pid);
        AddHit(out, F_HIDPROC,
               name.empty() ? wstring(L"(名称不可读)") : name,
               wstring(), wstring(L"PID ") + b, wstring(),
               L"系统内部可见但 Toolhelp 快照不可见（疑似 rootkit 隐藏）",
               wstring(), b);
    };
    for (const auto& kv : ntPids) checkPid(kv.first, kv.second);
    for (const auto& kv : handlePids)
        if (!ntPids.count(kv.first)) checkPid(kv.first, wstring());

    ctx.Log(L"  隐藏进程检测：NT 链表 " + U32((unsigned)ntPids.size()) +
            L" · 句柄表 " + U32((unsigned)handlePids.size()) +
            L" · 快照 " + U32((unsigned)snapPids.size()) +
            L" · 疑似隐藏 " + U32(hidden) + L" 个");
}

} // namespace

void EnumAll(const Ruleset& rs, ModuleContext& ctx, vector<Finding>& out) {
    struct Step { const wchar_t* label; void (*fn)(const Ruleset&, ModuleContext&, vector<Finding>&); };
    static const Step steps[] = {
        { L"正在检查运行中的进程…",   EnumProcessesPurg },
        { L"正在检查服务…",           [](const Ruleset& r, ModuleContext& c, vector<Finding>& o) { EnumServicesPurg(r, c, o, false); } },
        { L"正在检查内核驱动…",       [](const Ruleset& r, ModuleContext& c, vector<Finding>& o) { EnumServicesPurg(r, c, o, true); } },
        { L"正在检查内核模块表…",     EnumKernelModules },
        { L"正在检测隐藏进程…",       EnumHiddenProcs },
        { L"正在检查启动项…",         EnumRunItems },
        { L"正在检查计划任务…",       EnumTasksPurg },
        { L"正在检查右键菜单扩展…",   EnumShellEx },
        { L"正在检查输入法布局…",     EnumImeLayouts },
        { L"正在检查文件关联…",       EnumAssociations },
        { L"正在检查浏览器扩展…",     EnumBrowserExts },
        { L"正在检查 hosts 劫持…",    EnumHosts },
        { L"正在检查系统代理…",       EnumProxy },
    };
    const int total = (int)(sizeof(steps) / sizeof(steps[0]));
    for (int i = 0; i < total; ++i) {
        if (ctx.Cancelled()) break;
        ctx.Report(i * 100 / total, steps[i].label);
        steps[i].fn(rs, ctx, out);
    }
    ctx.Report(100, L"查杀扫描完成");
}

} // namespace purg
} // namespace mz
