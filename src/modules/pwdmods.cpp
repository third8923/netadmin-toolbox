// ============================================================================
//  网管工具箱 (NetAdmin Toolbox) —— 密码工具模块集（v2.11）
//  ---------------------------------------------------------------------------
//  四个模块，Group 均为「密码工具」，定位 = 本机/自有资产的密码恢复与查看：
//    * pwd.reveal  星号密码查看器：枚举可见窗口中的标准 Win32 密码框
//                  （Edit/RichEdit + ES_PASSWORD），SendMessageTimeout(WM_GETTEXT)
//                  跨进程读真实文本（掩码只是显示层，文本本体仍在控件里）；
//                  仅覆盖标准控件，Chrome/Edge 等自绘界面读不到
//    * pwd.zip     ZIP 密码恢复（暴力破解）：传统 ZipCrypto；掩码模式（d/l/u/a/A
//                  或自定义字符集 + 长度区间）与字典模式（dict:文件路径）；
//                  多线程 + 随时可停；命中判定 = 12 字节校验字节粗筛 →
//                  全量解密 + INFLATE 解压 + CRC32 精校（自实现 puff 式
//                  inflate，零第三方依赖），误报率 ~2^-32；AES(0x9901)/
//                  7z/RAR 明确报不支持；密码按 GBK 与 UTF-8 双编码各试一次
//    * pwd.cred    凭据管理器：CredEnumerateW 枚举当前用户已保存凭据，
//                  GENERIC 类型 blob 按 UTF-16/UTF-8 尝试解码出密码
//    * pwd.wifi    WiFi 密码查看：netsh wlan show profile key=clear，
//                  中英双语文案解析（关键内容 / Key Content）
//  合规边界：仅恢复本机/自有文件密码；不做网络服务（SSH/RDP/SMB）爆破。
//  链接：advapi32(CredEnumerate)/user32 —— build.bat 已有，无需新增。
// ============================================================================
#include "../module.h"
#include "../common.h"
#include "../procrun.h"
#include "../cleanresult.h"   // ErrorTextZh（Win32 错误码中文归因）

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cwctype>
#include <thread>

#include <wincred.h>

namespace mz {
namespace {

// ---------------------------------------------------------------- 小工具

wstring Trim(const wstring& s) {
    size_t b = s.find_first_not_of(L" \t\r\n");
    if (b == wstring::npos) return wstring();
    size_t e = s.find_last_not_of(L" \t\r\n");
    return s.substr(b, e - b + 1);
}

wstring U64(unsigned long long v) {
    wchar_t b[24] = {};
    swprintf_s(b, L"%llu", v);
    return b;
}

wstring F64(double v) {
    wchar_t b[32] = {};
    swprintf_s(b, L"%.1f", v);
    return b;
}

// 按 \r\n / \n 拆行
vector<wstring> SplitLines(const wstring& text) {
    vector<wstring> lines;
    size_t pos = 0;
    while (pos <= text.size()) {
        size_t nl = text.find(L'\n', pos);
        wstring line = (nl == wstring::npos) ? text.substr(pos)
                                             : text.substr(pos, nl - pos);
        if (!line.empty() && line.back() == L'\r') line.pop_back();
        lines.push_back(line);
        if (nl == wstring::npos) break;
        pos = nl + 1;
    }
    return lines;
}

// 宽字符文本 → 指定代码页字节（936=GBK / CP_UTF8）
bool WToBytes(const wstring& s, unsigned codepage, vector<unsigned char>& out) {
    if (s.empty()) { out.clear(); return true; }
    int need = WideCharToMultiByte(codepage, 0, s.c_str(), (int)s.size(),
                                   nullptr, 0, nullptr, nullptr);
    if (need <= 0) return false;
    out.resize((size_t)need);
    return WideCharToMultiByte(codepage, 0, s.c_str(), (int)s.size(),
                               (char*)out.data(), need, nullptr, nullptr) > 0;
}

// 复制文本到剪贴板（CF_UNICODETEXT）
bool CopyTextToClipboard(HWND hwnd, const wstring& text) {
    if (!OpenClipboard(hwnd)) return false;
    EmptyClipboard();
    size_t bytes = (text.size() + 1) * sizeof(wchar_t);
    HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, bytes);
    bool ok = false;
    if (g) {
        void* p = GlobalLock(g);
        if (p) {
            memcpy(p, text.c_str(), bytes);
            GlobalUnlock(g);
            ok = SetClipboardData(CF_UNICODETEXT, g) != nullptr;
        }
        if (!ok) GlobalFree(g);
    }
    CloseClipboard();
    return ok;
}

// ================================================================ CRC32
// 编译期生成表（工作线程并发读，避免运行期初始化竞态）

constexpr std::array<unsigned, 256> MakeCrcTab() {
    std::array<unsigned, 256> t{};
    for (unsigned i = 0; i < 256; ++i) {
        unsigned c = i;
        for (int k = 0; k < 8; ++k)
            c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        t[i] = c;
    }
    return t;
}
constexpr auto kCrcTab = MakeCrcTab();

unsigned Crc32(const unsigned char* p, size_t n) {
    unsigned c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; ++i)
        c = kCrcTab[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

// ================================================================ ZipCrypto

struct ZipKeys {
    unsigned k0 = 0x12345678u;
    unsigned k1 = 0x23456789u;
    unsigned k2 = 0x34567890u;
};

inline void ZkUpdate(ZipKeys& k, unsigned char c) {
    // key2 用 key1 的高字节（PKWARE 传统加密；7-Zip/CPython 实测一致，
    // 部分网上资料误写为 key0 高字节，会导致全部密码校验失败）
    k.k0 = kCrcTab[(k.k0 ^ c) & 0xFF] ^ (k.k0 >> 8);
    k.k1 = (k.k1 + (k.k0 & 0xFF)) * 134775813u + 1u;
    k.k2 = kCrcTab[(k.k2 ^ (unsigned char)(k.k1 >> 24)) & 0xFF] ^ (k.k2 >> 8);
}

inline unsigned char ZkDecryptByte(const ZipKeys& k) {
    unsigned short t = (unsigned short)((k.k2 | 2u) & 0xFFFF);
    return (unsigned char)(((t * (t ^ 1u)) >> 8) & 0xFF);
}

// ================================================================ INFLATE
// puff 风格极简解压（仅用于 ZipCrypto 命中后的 CRC 精校）。返回 0 = 成功。

struct ZBits {
    const unsigned char* d = nullptr;
    size_t n = 0, pos = 0;
    long long buf = 0;
    int cnt = 0;
};

int ZBits_(ZBits& z, int need) {
    long long val = z.buf;
    while (z.cnt < need) {
        if (z.pos >= z.n) return -1;
        val |= (long long)z.d[z.pos++] << z.cnt;
        z.cnt += 8;
    }
    z.buf = val >> need;
    z.cnt -= need;
    return (int)(val & ((1LL << need) - 1));
}

struct ZHuff {
    short count[16] = {};
    short symbol[288] = {};
};

// 由码长表构建规范 Huffman；返回 <0 = 码表非法（超订）
int ZBuild(ZHuff& h, const unsigned char* len, int n) {
    for (int i = 0; i < 16; ++i) h.count[i] = 0;
    for (int i = 0; i < n; ++i) h.count[len[i]]++;
    if (h.count[0] == n) return 0;   // 全空码表（合法的空距离树）
    int left = 1;
    for (int l = 1; l < 16; ++l) {
        left <<= 1;
        left -= h.count[l];
        if (left < 0) return left;
    }
    short offs[16];
    offs[1] = 0;
    for (int l = 1; l < 15; ++l) offs[l + 1] = (short)(offs[l] + h.count[l]);
    for (int i = 0; i < n; ++i)
        if (len[i]) h.symbol[offs[len[i]]++] = (short)i;
    return left;   // >0 = 不完整码表（距离树允许单码场景）
}

int ZDecodeSym(ZBits& z, const ZHuff& h) {
    int code = 0, first = 0, index = 0;
    for (int l = 1; l < 16; ++l) {
        int b = ZBits_(z, 1);
        if (b < 0) return -10;
        code = (code << 1) | b;
        int count = h.count[l];
        if (code - count < first) return h.symbol[index + (code - first)];
        index += count;
        first = (first + count) << 1;
    }
    return -9;
}

const short kLenBase[29] = {3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,
                            59,67,83,99,115,131,163,195,227,258};
const short kLenExt[29] = {0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0};
const short kDistBase[30] = {1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,
                             513,769,1025,1537,2049,3073,4097,6145,8193,12289,
                             16385,24577};
const short kDistExt[30] = {0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,
                            11,11,12,12,13,13};

int ZCodes(ZBits& z, const ZHuff& lenc, const ZHuff& distc,
           vector<unsigned char>& out, size_t cap) {
    for (;;) {
        int sym = ZDecodeSym(z, lenc);
        if (sym < 0) return sym;
        if (sym < 256) {
            if (out.size() >= cap) return -11;
            out.push_back((unsigned char)sym);
        } else if (sym > 256) {
            sym -= 257;
            if (sym >= 29) return -8;
            int len = kLenBase[sym] + ZBits_(z, kLenExt[sym]);
            if (len < kLenBase[sym]) return len;
            int dsym = ZDecodeSym(z, distc);
            if (dsym < 0) return dsym;
            if (dsym >= 30) return -8;
            int dist = kDistBase[dsym] + ZBits_(z, kDistExt[dsym]);
            if (dist < kDistBase[dsym]) return dist;
            if (out.size() + (size_t)len > cap) return -11;
            if ((size_t)dist > out.size()) return -12;
            size_t from = out.size() - (size_t)dist;
            for (int i = 0; i < len; ++i) out.push_back(out[from + i]);
        } else {
            break;   // 256 = 块结束
        }
    }
    return 0;
}

int ZStored(ZBits& z, vector<unsigned char>& out, size_t cap) {
    z.buf = 0;
    z.cnt = 0;   // 丢弃位缓冲对齐到字节
    if (z.pos + 4 > z.n) return -5;
    unsigned len = (unsigned)z.d[z.pos] | ((unsigned)z.d[z.pos + 1] << 8);
    unsigned nlen = (unsigned)z.d[z.pos + 2] | ((unsigned)z.d[z.pos + 3] << 8);
    z.pos += 4;
    if (len != (~nlen & 0xFFFFu)) return -6;
    if (z.pos + len > z.n) return -5;
    if (out.size() + len > cap) return -11;
    out.insert(out.end(), z.d + z.pos, z.d + z.pos + len);
    z.pos += len;
    return 0;
}

int ZFixedBlock(ZBits& z, vector<unsigned char>& out, size_t cap) {
    static ZHuff* fix = nullptr;
    if (!fix) {
        fix = new ZHuff();
        unsigned char lengths[288];
        int i = 0;
        for (; i < 144; ++i) lengths[i] = 8;
        for (; i < 256; ++i) lengths[i] = 9;
        for (; i < 280; ++i) lengths[i] = 7;
        for (; i < 288; ++i) lengths[i] = 8;
        ZBuild(*fix, lengths, 288);
    }
    ZHuff distc{};
    unsigned char dlen[30];
    for (int i = 0; i < 30; ++i) dlen[i] = 5;
    ZBuild(distc, dlen, 30);
    return ZCodes(z, *fix, distc, out, cap);
}

int ZDynamicBlock(ZBits& z, vector<unsigned char>& out, size_t cap) {
    static const short ordv[19] = {16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15};
    // RFC1951：HLIT/HDIST/HCLEN 均带偏移（+257 / +1 / +4）
    int nlen = ZBits_(z, 5) + 257;    if (nlen < 0) return nlen;
    int ndist = ZBits_(z, 5) + 1;     if (ndist < 0) return ndist;
    int ncode = ZBits_(z, 4) + 4;     if (ncode < 0) return ncode;
    if (nlen > 286 || ndist > 30 || ncode > 19) return -3;
    unsigned char clens[19] = {};
    for (int i = 0; i < ncode; ++i) {
        int c = ZBits_(z, 3);
        if (c < 0) return c;
        clens[ordv[i]] = (unsigned char)c;
    }
    ZHuff clh;
    if (ZBuild(clh, clens, 19) < 0) return -3;
    unsigned char lengths[320] = {};
    int i = 0;
    while (i < nlen + ndist) {
        int sym = ZDecodeSym(z, clh);
        if (sym < 0) return sym;
        if (sym < 16) {
            lengths[i++] = (unsigned char)sym;
        } else {
            int len = 0, rep = 0;
            if (sym == 16) {
                if (i == 0) return -8;
                len = lengths[i - 1];
                rep = 3 + ZBits_(z, 2);
            } else if (sym == 17) {
                rep = 3 + ZBits_(z, 3);
            } else {
                rep = 11 + ZBits_(z, 7);
            }
            if (rep < 0) return rep;
            if (i + rep > nlen + ndist) return -8;
            while (rep-- > 0) lengths[i++] = (unsigned char)len;
        }
    }
    if (lengths[256] == 0) return -7;   // 无块结束码
    ZHuff lenc, distc;
    if (ZBuild(lenc, lengths, nlen) < 0) return -3;
    ZBuild(distc, lengths + nlen, ndist);   // 距离树允许不完整
    return ZCodes(z, lenc, distc, out, cap);
}

int ZInflate(const unsigned char* d, size_t n, vector<unsigned char>& out,
             size_t cap) {
    ZBits z;
    z.d = d;
    z.n = n;
    int last = 0;
    do {
        last = ZBits_(z, 1);
        if (last < 0) return last;
        int type = ZBits_(z, 2);
        if (type < 0) return type;
        int err = 0;
        if (type == 0) err = ZStored(z, out, cap);
        else if (type == 1) err = ZFixedBlock(z, out, cap);
        else if (type == 2) err = ZDynamicBlock(z, out, cap);
        else return -2;
        if (err) return err;
    } while (!last);
    return 0;
}

// ================================================================ ZIP 解析

struct ZipEntry {
    wstring name;
    unsigned gpFlag = 0, method = 0, crc = 0, modTime = 0;
    unsigned long long compSize = 0, uncompSize = 0;
    unsigned long long localOff = 0;
    bool isAes = false;       // method 99 或 0x9901 extra
};

unsigned Rd16(const unsigned char* p) {
    return (unsigned)p[0] | ((unsigned)p[1] << 8);
}
unsigned Rd32(const unsigned char* p) {
    return (unsigned)p[0] | ((unsigned)p[1] << 8) | ((unsigned)p[2] << 16) |
           ((unsigned)p[3] << 24);
}

// extra 字段里是否有 0x9901（AES-WZSE）
bool ExtraHasAes(const unsigned char* p, unsigned n) {
    size_t i = 0;
    while (i + 4 <= n) {
        unsigned id = Rd16(p + i);
        unsigned sz = Rd16(p + i + 2);
        if (id == 0x9901) return true;
        i += 4 + sz;
    }
    return false;
}

// 从整个文件镜像解析中央目录；失败返回 false（msg 归因）
bool ParseZipCentral(const vector<unsigned char>& file, vector<ZipEntry>& out,
                     wstring& msg) {
    const unsigned char* d = file.data();
    size_t n = file.size();
    if (n < 22) { msg = L"文件太小，不是 ZIP"; return false; }

    // 从尾部向前找 EOCD（注释区最长 65535）
    size_t scanMax = n > 66000 ? 66000 : n;
    long long eocd = -1;
    for (long long i = (long long)n - 22; i >= (long long)(n - scanMax); --i) {
        if (Rd32(d + i) == 0x06054b50) { eocd = i; break; }
    }
    if (eocd < 0) { msg = L"找不到 ZIP 中央目录结尾（EOCD）——可能是 7z/RAR 或损坏文件"; return false; }

    unsigned entries = Rd16(d + eocd + 10);
    unsigned cdSize = Rd32(d + eocd + 12);
    unsigned cdOff = Rd32(d + eocd + 16);
    size_t pos = cdOff;
    size_t cdEnd = cdOff + cdSize;
    if (cdEnd > n) { msg = L"中央目录越界，文件不完整或带前置数据"; return false; }

    for (unsigned e = 0; e < entries && pos + 46 <= cdEnd; ++e) {
        if (Rd32(d + pos) != 0x02014b50) { msg = L"中央目录条目签名异常"; return false; }
        ZipEntry en;
        en.gpFlag = Rd16(d + pos + 8);
        en.method = Rd16(d + pos + 10);
        en.modTime = Rd16(d + pos + 12);
        en.crc = Rd32(d + pos + 16);
        en.compSize = Rd32(d + pos + 20);
        en.uncompSize = Rd32(d + pos + 24);
        unsigned nameLen = Rd16(d + pos + 28);
        unsigned extraLen = Rd16(d + pos + 30);
        unsigned cmtLen = Rd16(d + pos + 32);
        en.localOff = Rd32(d + pos + 42);
        if (pos + 46 + nameLen > n) { msg = L"中央目录文件名越界"; return false; }
        // 文件名：优先 UTF-8 标志（bit 11），否则按 GBK（国内工具默认）
        if (en.gpFlag & 0x800) {
            int wl = MultiByteToWideChar(CP_UTF8, 0, (const char*)d + pos + 46,
                                         (int)nameLen, nullptr, 0);
            if (wl > 0) {
                en.name.resize((size_t)wl);
                MultiByteToWideChar(CP_UTF8, 0, (const char*)d + pos + 46,
                                    (int)nameLen, &en.name[0], wl);
            }
        } else {
            int wl = MultiByteToWideChar(936, 0, (const char*)d + pos + 46,
                                         (int)nameLen, nullptr, 0);
            if (wl > 0) {
                en.name.resize((size_t)wl);
                MultiByteToWideChar(936, 0, (const char*)d + pos + 46,
                                    (int)nameLen, &en.name[0], wl);
            }
        }
        if (pos + 46 + nameLen + extraLen <= n)
            en.isAes = (en.method == 99) || ExtraHasAes(d + pos + 46 + nameLen, extraLen);
        out.push_back(en);
        pos += 46 + nameLen + extraLen + cmtLen;
    }
    if (out.empty()) { msg = L"中央目录为空"; return false; }
    return true;
}

// 读本地头，算数据起点
bool LocalDataStart(const vector<unsigned char>& file, const ZipEntry& en,
                    unsigned long long& dataStart, unsigned& checkByteRaw,
                    wstring& msg) {
    size_t off = (size_t)en.localOff;
    if (off + 30 > file.size() || Rd32(file.data() + off) != 0x04034b50) {
        msg = L"本地文件头异常（偏移 " + U64(en.localOff) + L"）";
        return false;
    }
    unsigned nameLen = Rd16(file.data() + off + 26);
    unsigned extraLen = Rd16(file.data() + off + 28);
    dataStart = en.localOff + 30 + nameLen + extraLen;
    if (dataStart + en.compSize > file.size()) {
        msg = L"条目数据越界，文件不完整";
        return false;
    }
    // 校验字节：bit3（数据描述符）时用 modTime 高字节，否则 CRC 高字节
    checkByteRaw = (en.gpFlag & 0x8) ? ((en.modTime >> 8) & 0xFF)
                                     : ((en.crc >> 24) & 0xFF);
    return true;
}

// 尝试一个候选密码：12 字节粗筛 → 通过则全量解密 + 解压 + CRC 精校
// 返回：0=不匹配 1=粗筛通过但精校不过（误报） 2=密码正确
int ZipTryPassword(const unsigned char* data, unsigned long long dataStart,
                   const ZipEntry& en, unsigned checkByte,
                   const unsigned char* pwd, size_t pwdLen) {
    if (en.compSize < 13) return 0;
    ZipKeys k;
    for (size_t i = 0; i < pwdLen; ++i) ZkUpdate(k, pwd[i]);
    unsigned char hdr[12];
    for (int i = 0; i < 12; ++i) {
        unsigned char c = data[dataStart + i] ^ ZkDecryptByte(k);
        ZkUpdate(k, c);
        hdr[i] = c;
    }
    if (hdr[11] != (unsigned char)checkByte) return 0;

    // 粗筛通过（1/256）：全量解密
    vector<unsigned char> plain((size_t)en.compSize - 12);
    size_t outPos = 0;
    for (unsigned long long i = 12; i < en.compSize; ++i) {
        unsigned char c = data[dataStart + i] ^ ZkDecryptByte(k);
        ZkUpdate(k, c);
        plain[outPos++] = c;
    }
    unsigned gotCrc = 0;
    if (en.method == 0) {
        gotCrc = Crc32(plain.data(), plain.size());
    } else {
        vector<unsigned char> out;
        size_t cap = en.uncompSize ? (size_t)en.uncompSize + 4096
                                   : (size_t)(1ull << 30);
        if (ZInflate(plain.data(), plain.size(), out, cap) != 0) return 1;
        gotCrc = Crc32(out.data(), out.size());
    }
    return (gotCrc == en.crc) ? 2 : 1;
}

// ================================================================ 暴力引擎

struct BruteState {
    // ZIP 数据（整个文件镜像 + 选中条目）
    const vector<unsigned char>* file = nullptr;
    ZipEntry entry;
    unsigned long long dataStart = 0;
    unsigned checkByte = 0;

    // 掩码模式
    wstring chars;
    int minLen = 1, maxLen = 6;
    unsigned long long total = 0;    // 候选总数

    // 字典模式（dict 非空时用字典）
    const vector<wstring>* dict = nullptr;

    std::atomic<unsigned long long> next{0};     // 候选游标
    std::atomic<unsigned long long> tried{0};
    std::atomic<bool> found{false};
    std::atomic<bool> stop{false};
    wstring foundPwd;                            // 命中后只写一次（stop 屏障）
    wstring foundEnc;                            // GBK / UTF-8
};

// 掩码下标 → 候选串（变长混合进制解码：短长度段在前）
wstring DecodeMask(const BruteState& st, unsigned long long idx) {
    int n = (int)st.chars.size();
    unsigned long long pw = 1;
    for (int L = st.minLen; L <= st.maxLen; ++L) {
        pw *= (unsigned long long)n;      // pw = n^L
        unsigned long long cnt = pw;
        if (idx < cnt) {
            wstring s((size_t)L, L' ');
            for (int i = L - 1; i >= 0; --i) {
                s[(size_t)i] = st.chars[(size_t)(idx % (unsigned long long)n)];
                idx /= (unsigned long long)n;
            }
            return s;
        }
        idx -= cnt;
    }
    return wstring();   // 越界（不应到达）
}

// 测一个候选（GBK 与 UTF-8 双编码）；命中返回 true
bool TryCandidate(BruteState& st, const wstring& cand) {
    vector<unsigned char> bytes;
    if (WToBytes(cand, 936, bytes) && !bytes.empty()) {
        int r = ZipTryPassword(st.file->data(), st.dataStart, st.entry,
                               st.checkByte, bytes.data(), bytes.size());
        if (r == 2) { st.foundEnc = L"GBK"; return true; }
    }
    if (WToBytes(cand, CP_UTF8, bytes) && !bytes.empty()) {
        int r = ZipTryPassword(st.file->data(), st.dataStart, st.entry,
                               st.checkByte, bytes.data(), bytes.size());
        if (r == 2) { st.foundEnc = L"UTF-8"; return true; }
    }
    return false;
}

DWORD WINAPI BruteWorker(LPVOID param) {
    BruteState* st = (BruteState*)param;
    for (;;) {
        if (st->found.load(std::memory_order_acquire)) break;
        if (st->stop.load(std::memory_order_relaxed)) break;
        unsigned long long idx = st->next.fetch_add(1, std::memory_order_relaxed);
        if (idx >= st->total) break;
        if (st->dict) {
            if (idx >= st->dict->size()) break;
            if (TryCandidate(*st, (*st->dict)[(size_t)idx])) {
                st->foundPwd = (*st->dict)[(size_t)idx];
                st->found.store(true, std::memory_order_release);
                break;
            }
        } else {
            wstring cand = DecodeMask(*st, idx);
            if (cand.empty()) break;
            if (TryCandidate(*st, cand)) {
                st->foundPwd = cand;
                st->found.store(true, std::memory_order_release);
                break;
            }
        }
        st->tried.fetch_add(1, std::memory_order_relaxed);
    }
    return 0;
}

// 解析掩码/字典参数 → BruteState；失败时 err 给中文原因
bool ParseCrackSpec(const wstring& spec, BruteState& st, vector<wstring>* dict,
                    wstring& err) {
    wstring s = Trim(spec);
    if (s.empty()) {
        err = L"NEED_SPEC";   // 调用方转「仅检测」模式
        return false;
    }
    if (s.rfind(L"dict:", 0) == 0 || s.rfind(L"字典:", 0) == 0) {
        wstring path = Trim(s.substr(s.find(L':') + 1));
        if (path.empty()) { err = L"字典模式需要在 dict: 后跟字典文件路径"; return false; }
        HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
                               nullptr, OPEN_EXISTING, 0, nullptr);
        if (h == INVALID_HANDLE_VALUE) {
            err = L"打不开字典文件：" + path;
            return false;
        }
        LARGE_INTEGER sz{};
        GetFileSizeEx(h, &sz);
        if (sz.QuadPart > (long long)512 * 1024 * 1024) {
            CloseHandle(h);
            err = L"字典文件超过 512MB 上限";
            return false;
        }
        vector<unsigned char> buf((size_t)sz.QuadPart);
        DWORD got = 0;
        ReadFile(h, buf.data(), (DWORD)buf.size(), &got, nullptr);
        CloseHandle(h);
        buf.resize(got);
        // 编码：BOM 优先；无 BOM 先按 UTF-8，解出坏字符再按 GBK
        bool utf16le = buf.size() >= 2 && buf[0] == 0xFF && buf[1] == 0xFE;
        bool utf8bom = buf.size() >= 3 && buf[0] == 0xEF && buf[1] == 0xBB && buf[2] == 0xBF;
        wstring text;
        if (utf16le) {
            text.assign((const wchar_t*)(buf.data() + 2), (buf.size() - 2) / 2);
        } else {
            const char* p = (const char*)buf.data();
            int skip = utf8bom ? 3 : 0;
            int wl = MultiByteToWideChar(CP_UTF8, 0, p + skip,
                                         (int)(buf.size() - skip), nullptr, 0);
            unsigned cp = CP_UTF8;
            if (wl <= 0) {
                cp = 936;
                wl = MultiByteToWideChar(936, 0, p + skip,
                                         (int)(buf.size() - skip), nullptr, 0);
            }
            if (wl > 0) {
                text.resize((size_t)wl);
                MultiByteToWideChar(cp, 0, p + skip, (int)(buf.size() - skip),
                                    &text[0], wl);
            }
        }
        for (const wstring& line : SplitLines(text)) {
            wstring t = Trim(line);
            if (!t.empty()) dict->push_back(t);
            if (dict->size() >= 50000000u) break;   // 5000 万行上限
        }
        if (dict->empty()) { err = L"字典文件里没有有效行"; return false; }
        st.dict = dict;
        st.total = dict->size();
        return true;
    }

    // 掩码模式：<字符集>:<min>-<max>
    size_t colon = s.find(L':');
    if (colon == wstring::npos) {
        err = L"参数格式不对。掩码：d:4-8（字符集:最短-最长）；字典：dict:\\path\\list.txt";
        return false;
    }
    wstring cs = s.substr(0, colon);
    wstring range = s.substr(colon + 1);
    if (cs == L"d" || cs == L"D") st.chars = L"0123456789";
    else if (cs == L"l") st.chars = L"abcdefghijklmnopqrstuvwxyz";
    else if (cs == L"u") st.chars = L"ABCDEFGHIJKLMNOPQRSTUVWXYZ";
    else if (cs == L"a" || cs == L"lu" || cs == L"ul" || cs == L"dl" || cs == L"ld")
        st.chars = L"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    else if (cs == L"A") {
        for (wchar_t c = 0x21; c <= 0x7E; ++c) st.chars.push_back(c);
    } else
        st.chars = cs;   // 自定义字符集
    unsigned mn = 0, mx = 0;
    if (swscanf_s(range.c_str(), L"%u-%u", &mn, &mx) != 2 || mn < 1 || mx < mn ||
        mx > 16 || st.chars.empty() || st.chars.size() > 100) {
        err = L"掩码范围非法（需 1-16 位且 最短≤最长），字符集 1-100 个字符";
        return false;
    }
    st.minLen = (int)mn;
    st.maxLen = (int)mx;
    // 总量（带溢出检查；特大空间不阻止，仅由调用方提示）
    unsigned long long total = 0, pw = 1;
    unsigned long long csN = st.chars.size();
    for (int L = st.minLen; L <= st.maxLen; ++L) {
        if (pw > (~0ull) / csN) { err = L"候选空间过大（溢出），请缩小范围"; return false; }
        pw *= csN;
        if (total > (~0ull) - pw) { err = L"候选空间过大（溢出），请缩小范围"; return false; }
        total += pw;
    }
    st.total = total;
    return true;
}

} // namespace

// ===========================================================================
//  1) pwd.reveal 星号密码查看器
// ===========================================================================
class PwdRevealModule final : public IModule {
public:
    const wchar_t* Id() const override { return L"pwd.reveal"; }
    const wchar_t* Group() const override { return L"密码工具"; }
    const wchar_t* Title() const override { return L"星号密码查看器"; }
    const wchar_t* Description() const override {
        return L"读出本机可见窗口密码框里的真实文本（仅标准 Win32 控件，自绘界面如浏览器读不到）";
    }
    ModuleKind Kind() const override { return ModuleKind::Action; }
    const wchar_t* RunLabel() const override { return L"扫描密码框"; }

    vector<ModuleAction> Actions() const override {
        return { { L"复制全部读出的密码", nullptr, false, 0xE8C8 } };   // Copy
    }

    wstring ActionWarning(int actionIndex, const vector<int>& tags) const override {
        (void)actionIndex; (void)tags;
        return L"将把本次扫描读出的全部密码以明文复制到剪贴板，是否继续？";
    }

    void RunAction(ModuleContext& ctx, int actionIndex, ModuleResult& out) override {
        out = ModuleResult{};
        if (actionIndex != 0) return;
        if (revealed_.empty()) {
            out.summary = L"没有可复制的密码（请先点「扫描密码框」）";
            ctx.Log(out.summary);
            ctx.Report(100, out.summary);
            return;
        }
        wstring all;
        for (const auto& [title, pwd] : revealed_) {
            all += title + L": " + pwd + L"\r\n";
        }
        if (CopyTextToClipboard(ctx.hwnd, all)) {
            out.summary = L"已复制 " + U64(revealed_.size()) + L" 条密码到剪贴板";
            ctx.Log(out.summary);
        } else {
            out.summary = L"复制失败：打不开剪贴板";
            ctx.Log(out.summary);
        }
        out.canClean = true;
        ctx.Report(100, out.summary);
    }

    void Run(ModuleContext& ctx, ModuleResult& out) override {
        out = ModuleResult{};
        revealed_.clear();
        ctx.Report(-1, L"正在枚举窗口密码框…");
        const DWORD myPid = GetCurrentProcessId();

        // 枚举可见顶层窗口（跳过本进程）
        vector<HWND> tops;
        EnumWindows([](HWND h, LPARAM lp) -> BOOL {
            auto* v = (vector<HWND>*)lp;
            if (IsWindowVisible(h)) v->push_back(h);
            return TRUE;
        }, (LPARAM)&tops);

        int scanned = 0, okN = 0, emptyN = 0, failN = 0;
        for (HWND top : tops) {
            if (ctx.Cancelled()) break;
            DWORD pid = 0;
            GetWindowThreadProcessId(top, &pid);
            if (pid == myPid) continue;
            wchar_t title[256] = {};
            GetWindowTextW(top, title, 256);
            wstring t = Trim(title);
            if (t.empty()) continue;   // 无标题窗口多半是框架，跳过降噪

            PwdScanCtx sc{ &ctx, t, &out, &okN, &emptyN, &failN, &revealed_, 0 };
            EnumChildWindows(top, [](HWND child, LPARAM lp) -> BOOL {
                auto* c = (PwdScanCtx*)lp;
                if (c->count > 300) return FALSE;   // 单窗口上限防刷屏
                wchar_t cls[64] = {};
                GetClassNameW(child, cls, 64);
                if (!IsEditPwdClass(cls)) return TRUE;
                LONG_PTR style = GetWindowLongPtrW(child, GWL_STYLE);
                if (!(style & ES_PASSWORD)) return TRUE;
                if (!IsWindowVisible(child)) return TRUE;
                c->count++;
                AppendRow(*c, child);
                return TRUE;
            }, (LPARAM)&sc);
            ++scanned;
        }

        out.summary = L"扫描 " + U64((unsigned long long)scanned) +
                      L" 个窗口：读出 " + U64((unsigned long long)okN) +
                      L" 个密码，" + U64((unsigned long long)emptyN) +
                      L" 个为空，" + U64((unsigned long long)failN) + L" 个读取失败";
        out.hint = wstring(T(L"仅覆盖标准 Win32 密码框（Edit/RichEdit）；浏览器、UWP 等自绘界面无法读取。"))
                   + T(L"请仅查看本机自己有权限的窗口。");
        out.canClean = !revealed_.empty();
        ctx.Log(out.summary);
        ctx.Report(100, out.summary);
    }

private:
    struct PwdScanCtx {
        ModuleContext* ctx;
        wstring title;
        ModuleResult* out;
        int* okN; int* emptyN; int* failN;
        vector<std::pair<wstring, wstring>>* revealed;
        int count;
    };

    static bool IsEditPwdClass(const wchar_t* cls) {
        return _wcsicmp(cls, L"Edit") == 0 ||
               _wcsicmp(cls, L"RichEdit20W") == 0 ||
               _wcsicmp(cls, L"RICHEDIT50W") == 0;
    }

    // 跨进程读 edit 文本（WM_GETTEXT 系统自动编组）
    static bool GetEditText(HWND h, wstring& outText) {
        DWORD_PTR len = 0;
        if (!SendMessageTimeoutW(h, WM_GETTEXTLENGTH, 0, 0,
                                 SMTO_ABORTIFHUNG, 400, &len))
            return false;
        UINT n = (UINT)len + 2;   // 密码框跨进程长度有 +1 兼容处理，宁多勿少
        if (n > 4096) n = 4096;
        vector<wchar_t> buf(n);
        DWORD_PTR got = 0;
        if (!SendMessageTimeoutW(h, WM_GETTEXT, (WPARAM)n, (LPARAM)buf.data(),
                                 SMTO_ABORTIFHUNG, 400, &got))
            return false;
        if (got > (DWORD_PTR)n) got = n;
        outText.assign(buf.data(), (size_t)got);
        return true;
    }

    static void AppendRow(PwdScanCtx& c, HWND edit) {
        wstring pwd;
        bool ok = GetEditText(edit, pwd);
        ModuleRow r;
        r.c1 = c.title;
        wchar_t cls[64] = {};
        GetClassNameW(edit, cls, 64);
        r.c2 = cls;
        if (ok) {
            if (pwd.empty()) {
                r.c3 = L"密码框为空";
                r.c4 = L"（空）";
                r.sortKey = 1000000000ull - (unsigned long long)c.out->rows.size();
                ++(*c.emptyN);
            } else {
                r.c3 = L"已读出";
                r.c4 = pwd;
                r.sortKey = 3000000000ull - (unsigned long long)c.out->rows.size();
                ++(*c.okN);
                c.revealed->push_back({ c.title, pwd });
                c.ctx->Log(L"  ✓ " + c.title + L" → " + pwd);
            }
        } else {
            DWORD e = GetLastError();
            r.c3 = L"读取失败";
            r.c4 = (e == ERROR_ACCESS_DENIED)
                       ? L"权限不足（目标窗口以更高权限运行）"
                       : L"窗口无响应或受保护";
            r.sortKey = 0ull + (unsigned long long)c.out->rows.size();
            ++(*c.failN);
        }
        c.out->rows.push_back(r);
    }

    vector<std::pair<wstring, wstring>> revealed_;   // 标题 → 密码（仅读出的）
};

// ===========================================================================
//  2) pwd.zip ZIP 密码恢复
// ===========================================================================
class PwdZipModule final : public IModule {
public:
    const wchar_t* Id() const override { return L"pwd.zip"; }
    const wchar_t* Group() const override { return L"密码工具"; }
    const wchar_t* Title() const override { return L"ZIP 密码恢复"; }
    const wchar_t* Description() const override {
        return L"ZIP 传统加密（ZipCrypto）暴力破解：掩码 d:1-6 / 字典 dict:\\path\\list.txt，仅用于自有文件";
    }
    ModuleKind Kind() const override { return ModuleKind::Action; }
    bool NeedsPath() const override { return true; }
    bool NeedsTarget() const override { return true; }
    const wchar_t* PathHint() const override {
        return L"ZIP 文件完整路径，如 D:\\docs\\backup.zip";
    }
    const wchar_t* TargetHint() const override {
        return L"d:4-8（数字）/ a:1-6（字母数字）/ 0123ab:1-5（自定义）/ dict:\\path\\list.txt";
    }
    const wchar_t* RunLabel() const override { return L"开始破解"; }

    void SetInput(const wstring& path, const wstring& target) override {
        zipPath_ = Trim(path);
        target_ = Trim(target);
    }

    vector<ModuleAction> Actions() const override {
        return { { L"复制找到的密码", nullptr, false, 0xE8C8 } };
    }

    void RunAction(ModuleContext& ctx, int actionIndex, ModuleResult& out) override {
        out = ModuleResult{};
        if (actionIndex != 0) return;
        if (foundPwd_.empty()) {
            out.summary = L"还没有找到密码（请先「开始破解」）";
            ctx.Log(out.summary);
        } else if (CopyTextToClipboard(ctx.hwnd, foundPwd_)) {
            out.summary = L"已复制密码到剪贴板";
            ctx.Log(out.summary + L"（密码本体见日志与结果表）");
        } else {
            out.summary = L"复制失败：打不开剪贴板";
            ctx.Log(out.summary);
        }
        out.canClean = true;
        ctx.Report(100, out.summary);
    }

    void Run(ModuleContext& ctx, ModuleResult& out) override {
        out = ModuleResult{};
        foundPwd_.clear();
        const ULONG64 t0 = GetTickCount64();

        // ---- 1) 读文件 ----
        wstring path = Trim(zipPath_);
        if (path.empty()) {
            out.summary = L"请先在路径框填写 ZIP 文件完整路径";
            out.hint = TargetHint();
            ctx.Log(out.summary);
            ctx.Report(100, out.summary);
            return;
        }
        HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
                               nullptr, OPEN_EXISTING, 0, nullptr);
        if (h == INVALID_HANDLE_VALUE) {
            out.summary = L"打不开文件：" + path;
            out.hint = ErrorTextZh(GetLastError());
            ctx.Log(out.summary);
            ctx.Report(100, out.summary);
            return;
        }
        LARGE_INTEGER sz{};
        GetFileSizeEx(h, &sz);
        if (sz.QuadPart > (long long)1024 * 1024 * 1024) {
            CloseHandle(h);
            out.summary = L"文件超过 1GB 上限，请先拆分或用 7-Zip 单独压缩目标条目";
            ctx.Log(out.summary);
            ctx.Report(100, out.summary);
            return;
        }
        vector<unsigned char> file((size_t)sz.QuadPart);
        DWORD got = 0;
        ReadFile(h, file.data(), (DWORD)file.size(), &got, nullptr);
        CloseHandle(h);
        file.resize(got);
        ctx.Log(L"已读入 " + U64(file.size()) + L" 字节");

        // ---- 2) 解析中央目录，选第一个可破条目 ----
        vector<ZipEntry> entries;
        wstring msg;
        size_t nameMax = path.find_last_of(L"\\/");
        wstring fname = (nameMax == wstring::npos) ? path : path.substr(nameMax + 1);
        auto addInfo = [&](const wstring& state, const wstring& detail) {
            ModuleRow r;
            r.c1 = fname;
            r.c2 = entries.empty() ? L"-" : entries[0].name;
            r.c3 = state;
            r.c4 = detail;
            r.sortKey = 0xFFFFFFFFFFFFFFFFull;
            out.rows.push_back(r);
        };
        if (!ParseZipCentral(file, entries, msg)) {
            addInfo(L"解析失败", msg);
            out.summary = L"ZIP 解析失败：" + msg;
            ctx.Log(out.summary);
            ctx.Report(100, out.summary);
            return;
        }
        ctx.Log(L"中央目录解析成功：共 " + U64(entries.size()) + L" 个条目");

        const ZipEntry* target = nullptr;
        bool anyEnc = false, anyAes = false;
        for (const ZipEntry& en : entries) {
            if (!(en.gpFlag & 1)) continue;
            anyEnc = true;
            if (en.isAes) { anyAes = true; continue; }
            if (en.compSize < 13) continue;
            if (!en.name.empty() && en.name.back() == L'/') continue;   // 目录
            target = &en;
            break;
        }
        if (!anyEnc) {
            addInfo(L"未加密", L"该 ZIP 没有加密条目，无需破解");
            out.summary = L"该 ZIP 未加密，直接解压即可";
            ctx.Log(out.summary);
            ctx.Report(100, out.summary);
            return;
        }
        if (anyAes && !target) {
            addInfo(L"暂不支持", L"检测到 AES 加密 ZIP（WinZip/7-Zip 新版默认），本模块仅支持传统 ZipCrypto");
            out.hint = T(L"可用 7-Zip 重打包并选择「ZipCrypto 传统加密」后再试");
            out.summary = L"AES 加密的 ZIP 暂不支持";
            ctx.Log(out.summary);
            ctx.Report(100, out.summary);
            return;
        }
        if (!target) {
            addInfo(L"无可破条目", L"加密条目均为目录或空条目");
            out.summary = L"没有可破解的数据条目";
            ctx.Log(out.summary);
            ctx.Report(100, out.summary);
            return;
        }

        unsigned long long dataStart = 0;
        unsigned checkByte = 0;
        if (!LocalDataStart(file, *target, dataStart, checkByte, msg)) {
            addInfo(L"解析失败", msg);
            out.summary = msg;
            ctx.Log(out.summary);
            ctx.Report(100, out.summary);
            return;
        }
        const wchar_t* methodText =
            target->method == 0 ? L"存储（不压缩）"
                                : (target->method == 8 ? L"Deflate" : L"其他压缩方法");
        wchar_t cbHex[8] = {};
        swprintf_s(cbHex, L"%02X", checkByte);
        ctx.Log(L"目标条目：" + target->name + L"（" + U64(target->compSize) +
                L" 字节压缩 / " + U64(target->uncompSize) + L" 字节原文，" +
                methodText + L"，校验字节 0x" + cbHex + L"）");

        // ---- 3) 解析破解参数（空 = 仅检测）----
        BruteState st;
        st.file = &file;
        st.entry = *target;
        st.dataStart = dataStart;
        st.checkByte = checkByte;
        vector<wstring> dict;
        wstring err;
        if (!ParseCrackSpec(target_, st, &dict, err)) {
            if (err == L"NEED_SPEC") {
                ModuleRow r;
                r.c1 = fname;
                r.c2 = target->name;
                r.c3 = L"待破解";
                r.c4 = wstring(methodText) + L" · 请在目标框填掩码或字典";
                r.sortKey = 0xFFFFFFFFFFFFFFFFull;
                out.rows.push_back(r);
                out.summary = L"加密方式已确认（ZipCrypto），可开始破解";
                out.hint = wstring(T(L"掩码：d:4-8（纯数字4-8位）/ l:1-6 / a:1-8 / 0123ab:1-5；"))
                           + T(L"字典：dict:\\path\\list.txt。留空 = 仅检测。");
                ctx.Log(out.summary);
                ctx.Report(100, out.summary);
                return;
            }
            addInfo(L"参数错误", err);
            out.summary = err;
            ctx.Log(out.summary);
            ctx.Report(100, out.summary);
            return;
        }
        if (st.total > 200000000ull && !st.dict) {
            ctx.Log(L"警告：候选空间高达 " + U64(st.total) +
                    L" 个，大空间+大条目时精校会很慢，建议优先字典或缩小范围");
        }

        // ---- 4) 多线程爆破 ----
        int threads = (int)std::thread::hardware_concurrency();
        if (threads < 1) threads = 1;
        if (threads > 16) threads = 16;
        if (threads > (int)st.total) threads = (int)st.total;
        ctx.Log(L"启动 " + U64((unsigned long long)threads) + L" 个线程，候选总数 " +
                U64(st.total));
        ctx.Report(0, L"正在破解…");

        vector<HANDLE> ths;
        for (int i = 0; i < threads; ++i)
            if (HANDLE th = CreateThread(nullptr, 0, BruteWorker, &st, 0, nullptr))
                ths.push_back(th);
        if (ths.empty()) BruteWorker(&st);   // 兜底串行

        bool cancelled = false;
        while (!ths.empty()) {
            DWORD wr = WaitForMultipleObjects((DWORD)ths.size(), ths.data(),
                                              FALSE, 150);
            unsigned long long tried = st.tried.load(std::memory_order_relaxed);
            if (st.found.load(std::memory_order_acquire)) {
                ctx.Report(99, L"已命中，正在精校确认…");
                break;
            }
            if (wr == WAIT_TIMEOUT) {
                ctx.Report((int)(tried * 100 / std::max<unsigned long long>(1, st.total)),
                           L"已试 " + U64(tried) + L" / " + U64(st.total) + L" 个候选");
                if (ctx.Cancelled()) {
                    cancelled = true;
                    st.stop.store(true, std::memory_order_relaxed);
                }
                continue;
            }
            break;   // 全部线程退出或出错
        }
        if (!ths.empty()) {
            st.stop.store(true, std::memory_order_relaxed);
            WaitForMultipleObjects((DWORD)ths.size(), ths.data(), TRUE, 5000);
            for (HANDLE th : ths) CloseHandle(th);
        }
        const ULONG64 elapsed = GetTickCount64() - t0;

        // ---- 5) 结果 ----
        ModuleRow r;
        r.c1 = fname;
        r.c2 = target->name;
        if (st.found.load(std::memory_order_acquire)) {
            foundPwd_ = st.foundPwd;
            r.c3 = L"密码找到";
            r.c4 = foundPwd_ + L"（" + st.foundEnc + L" 编码）";
            r.sortKey = 0xFFFFFFFFFFFFFFFFull;
            r.danger = false;
            out.rows.push_back(r);
            out.summary = L"破解成功：密码 = " + foundPwd_ +
                          L"（用时 " + F64(elapsed / 1000.0) + L" 秒，尝试 " +
                          U64(st.tried.load() + 1) + L" 个候选）";
            ctx.Log(L"  ✓ 密码命中：" + foundPwd_ + L"（" + st.foundEnc +
                    L" 编码，已通过解压+CRC 全量校验，无误报）");
            out.hint = T(L"密码已通过整条目解压校验，可直接用于解压软件");
        } else if (cancelled) {
            r.c3 = L"已停止";
            r.c4 = L"已尝试 " + U64(st.tried.load()) + L" 个候选";
            out.rows.push_back(r);
            out.summary = L"破解已停止（可调整掩码/字典后重新开始）";
        } else {
            r.c3 = L"未找到";
            r.c4 = L"已穷尽 " + U64(st.tried.load()) + L" 个候选";
            out.rows.push_back(r);
            out.summary = L"破解完成：候选空间内未找到密码（用时 " +
                          F64(elapsed / 1000.0) + L" 秒）";
            out.hint = T(L"可换更大的字典或扩大掩码范围重试；AES/7z/RAR 加密不在支持范围");
        }
        out.canClean = !foundPwd_.empty();
        ctx.Log(out.summary);
        ctx.Report(100, out.summary);
    }

private:
    wstring zipPath_;    // 路径框：ZIP 文件
    wstring target_;     // 目标框：掩码 / dict:字典
    wstring foundPwd_;
};

// ===========================================================================
//  3) pwd.cred 凭据管理器
// ===========================================================================
class PwdCredModule final : public IModule {
public:
    const wchar_t* Id() const override { return L"pwd.cred"; }
    const wchar_t* Group() const override { return L"密码工具"; }
    const wchar_t* Title() const override { return L"凭据管理器"; }
    const wchar_t* Description() const override {
        return L"读取当前用户已保存的系统凭据（控制面板「凭据管理器」内容），域密码受系统保护不展示";
    }
    ModuleKind Kind() const override { return ModuleKind::Action; }
    const wchar_t* RunLabel() const override { return L"读取凭据"; }

    vector<ModuleAction> Actions() const override {
        return { { L"复制全部可读密码", nullptr, false, 0xE8C8 } };
    }

    wstring ActionWarning(int actionIndex, const vector<int>& tags) const override {
        (void)actionIndex; (void)tags;
        return L"将把可读出的凭据密码以明文复制到剪贴板，是否继续？";
    }

    void RunAction(ModuleContext& ctx, int actionIndex, ModuleResult& out) override {
        out = ModuleResult{};
        if (actionIndex != 0) return;
        if (revealed_.empty()) {
            out.summary = L"没有可复制的密码（请先「读取凭据」）";
            ctx.Log(out.summary);
            ctx.Report(100, out.summary);
            return;
        }
        wstring all;
        for (const auto& [target, pwd] : revealed_)
            all += target + L": " + pwd + L"\r\n";
        if (CopyTextToClipboard(ctx.hwnd, all)) {
            out.summary = L"已复制 " + U64(revealed_.size()) + L" 条密码到剪贴板";
            ctx.Log(out.summary);
        } else {
            out.summary = L"复制失败：打不开剪贴板";
            ctx.Log(out.summary);
        }
        out.canClean = true;
        ctx.Report(100, out.summary);
    }

    void Run(ModuleContext& ctx, ModuleResult& out) override {
        out = ModuleResult{};
        revealed_.clear();
        ctx.Report(-1, L"正在枚举凭据…");
        DWORD count = 0;
        CREDENTIALW** creds = nullptr;
        if (!CredEnumerateW(nullptr, 0, &count, &creds)) {
            DWORD e = GetLastError();
            out.summary = (e == ERROR_NOT_FOUND)
                              ? L"当前用户没有保存任何凭据"
                              : L"枚举凭据失败：" + ErrorTextZh(e);
            ctx.Log(out.summary);
            ctx.Report(100, out.summary);
            return;
        }

        int readable = 0;
        for (DWORD i = 0; i < count; ++i) {
            const CREDENTIALW& c = *creds[i];
            ModuleRow r;
            r.c1 = c.TargetName ? c.TargetName : L"（无目标名）";
            r.c2 = TypeText(c.Type);
            r.c3 = c.UserName ? c.UserName : L"（空）";
            r.c4 = BlobText(c);
            r.sortKey = (unsigned long long)(count - i);
            if (r.c4.rfind(L"（", 0) != 0) {   // 非括号说明 = 真密码
                ++readable;
                r.sortKey = 1000000000ull + (unsigned long long)(count - i);
                revealed_.push_back({ r.c1, r.c4 });
            }
            out.rows.push_back(r);
        }
        CredFree(creds);

        out.summary = L"共 " + U64(count) + L" 条凭据，" +
                      U64((unsigned long long)readable) + L" 条可读出密码";
        out.hint = wstring(T(L"域密码（SSO）由系统加密保护，工具按设计不展示；"))
                   + T(L"通用凭据的密码以明文复制时请留意周围环境。");
        out.canClean = readable > 0;
        ctx.Log(out.summary);
        ctx.Report(100, out.summary);
    }

private:
    static const wchar_t* TypeText(DWORD t) {
        switch (t) {
        case CRED_TYPE_GENERIC:                return L"通用凭据";
        case CRED_TYPE_DOMAIN_PASSWORD:        return L"域密码";
        case CRED_TYPE_DOMAIN_CERTIFICATE:     return L"域证书";
        case CRED_TYPE_DOMAIN_VISIBLE_PASSWORD: return L"域可见密码";
        case 5:  /*CRED_TYPE_GENERIC_CERTIFICATE*/ return L"通用证书";
        case 8:  /*CRED_TYPE_CERTIFICATE*/         return L"证书";
        default:                                   return L"其他";
        }
    }

    // blob → 密码文本；读不出给「（…）」说明
    static wstring BlobText(const CREDENTIALW& c) {
        if (c.Type != CRED_TYPE_GENERIC || !c.CredentialBlob || c.CredentialBlobSize == 0)
            return L"（系统保护，不可直接读取）";
        const unsigned char* b = c.CredentialBlob;
        size_t n = c.CredentialBlobSize;
        // UTF-16LE 判定要严格：仅认 BOM，或「偶对高字节过半为 0」的典型
        // UTF-16 模式；否则 Base64/二进制 token 会被强解成一串乱码 CJK
        bool utf16 = false;
        if (n >= 2 && n % 2 == 0)
            utf16 = (b[0] == 0xFF && b[1] == 0xFE) || (b[0] == 0xFE && b[1] == 0xFF);
        if (!utf16 && n >= 4 && n % 2 == 0) {
            size_t zeros = 0;
            for (size_t i = 1; i < n; i += 2)
                if (b[i] == 0) ++zeros;
            if (zeros * 2 >= n / 2) utf16 = true;
        }
        if (utf16) {
            wstring s((const wchar_t*)b, n / 2);
            while (!s.empty() && s.back() == L'\0') s.pop_back();
            if (!s.empty() && IsPrintable(s)) return s;
        }
        // UTF-8 / ASCII（token、Base64、明文均落此路径）
        {
            int wl = MultiByteToWideChar(CP_UTF8, 0, (const char*)b, (int)n,
                                         nullptr, 0);
            if (wl > 0) {
                wstring s((size_t)wl, 0);
                MultiByteToWideChar(CP_UTF8, 0, (const char*)b, (int)n, &s[0], wl);
                while (!s.empty() && (s.back() == L'\0' || s.back() == L'\n' ||
                                      s.back() == L'\r'))
                    s.pop_back();
                if (!s.empty() && IsPrintable(s)) return s;
            }
        }
        return L"（二进制 " + U64(n) + L" 字节，非文本）";
    }

    static bool IsPrintable(const wstring& s) {
        int printable = 0;
        for (wchar_t ch : s) {
            if (ch == L'\t' || ch == L'\r' || ch == L'\n') continue;
            if (ch < 0x20 || ch == 0x7F) return false;
            if (iswprint(ch)) ++printable;
        }
        return printable > 0;
    }

    vector<std::pair<wstring, wstring>> revealed_;
};

// ===========================================================================
//  4) pwd.wifi WiFi 密码查看
// ===========================================================================
class PwdWifiModule final : public IModule {
public:
    const wchar_t* Id() const override { return L"pwd.wifi"; }
    const wchar_t* Group() const override { return L"密码工具"; }
    const wchar_t* Title() const override { return L"WiFi 密码查看"; }
    const wchar_t* Description() const override {
        return L"读取本机已保存无线网络配置文件中的连接密码（netsh key=clear，需 WLAN 服务）";
    }
    ModuleKind Kind() const override { return ModuleKind::Action; }
    const wchar_t* RunLabel() const override { return L"读取密码"; }

    vector<ModuleAction> Actions() const override {
        return { { L"复制全部 WiFi 密码", nullptr, false, 0xE8C8 } };
    }

    wstring ActionWarning(int actionIndex, const vector<int>& tags) const override {
        (void)actionIndex; (void)tags;
        return L"将把本机已保存的 WiFi 密码以明文复制到剪贴板，是否继续？";
    }

    void RunAction(ModuleContext& ctx, int actionIndex, ModuleResult& out) override {
        out = ModuleResult{};
        if (actionIndex != 0) return;
        if (revealed_.empty()) {
            out.summary = L"没有可复制的密码（请先「读取密码」）";
            ctx.Log(out.summary);
            ctx.Report(100, out.summary);
            return;
        }
        wstring all;
        for (const auto& [ssid, pwd] : revealed_)
            all += ssid + L": " + pwd + L"\r\n";
        if (CopyTextToClipboard(ctx.hwnd, all)) {
            out.summary = L"已复制 " + U64(revealed_.size()) + L" 条 WiFi 密码到剪贴板";
            ctx.Log(out.summary);
        } else {
            out.summary = L"复制失败：打不开剪贴板";
            ctx.Log(out.summary);
        }
        out.canClean = true;
        ctx.Report(100, out.summary);
    }

    void Run(ModuleContext& ctx, ModuleResult& out) override {
        out = ModuleResult{};
        revealed_.clear();
        ctx.Report(-1, L"正在查询 WLAN 配置文件…");

        wstring full;
        ProcRunOptions opt;
        opt.timeoutMs = 60000;
        opt.cancel = [&ctx]() { return ctx.Cancelled(); };
        int code = RunProcess(L"netsh.exe wlan show profiles", opt, &full);
        if (code != 0) {
            out.summary = L"netsh 查询失败（退出码 " + U64((unsigned long long)code) +
                          L"）——请确认 WLAN AutoConfig 服务已启动且本机有无线网卡";
            ctx.Log(out.summary);
            ctx.Report(100, out.summary);
            return;
        }

        // 配置文件名：中英文系统双语解析
        vector<wstring> names;
        for (const wstring& line : SplitLines(full)) {
            if (line.find(L"所有用户配置文件") != wstring::npos ||
                line.find(L"All User Profile") != wstring::npos) {
                wstring name = Trim(AfterColon(line));
                if (!name.empty()) names.push_back(name);
            }
        }
        if (names.empty()) {
            out.summary = L"本机没有已保存的 WiFi 配置文件";
            ctx.Log(out.summary);
            ctx.Report(100, out.summary);
            return;
        }

        int withPwd = 0, idx = 0;
        for (const wstring& name : names) {
            if (ctx.Cancelled()) break;
            ++idx;
            ctx.Report(idx * 100 / (int)names.size(), L"读取 " + name);
            wstring safe = name;
            for (auto& ch : safe)
                if (ch == L'"' || ch == L'\\') ch = L' ';
            wstring profFull;
            ProcRunOptions opt2;
            opt2.timeoutMs = 60000;
            opt2.cancel = [&ctx]() { return ctx.Cancelled(); };
            RunProcess(L"netsh.exe wlan show profile name=\"" + safe +
                           L"\" key=clear",
                       opt2, &profFull);

            ModuleRow r;
            r.c1 = name;
            r.c2 = U64((unsigned long long)idx) + L" / " + U64(names.size());
            wstring pwd = L"";
            bool open = false;
            for (const wstring& line : SplitLines(profFull)) {
                if (line.find(L"关键内容") != wstring::npos ||
                    line.find(L"Key Content") != wstring::npos) {
                    wstring v = Trim(AfterColon(line));
                    if (!v.empty() && v != L"无" && v != L"Absent") pwd = v;
                }
                if (line.find(L"身份验证") != wstring::npos ||
                    line.find(L"Authentication") != wstring::npos) {
                    if (line.find(L"开放") != wstring::npos ||
                        line.find(L"Open") != wstring::npos)
                        open = true;
                }
            }
            if (pwd.empty() && open) pwd = L"（开放式网络，无密码）";
            if (pwd.empty()) {
                r.c3 = L"未取得";
                r.c4 = L"（受保护或非本用户配置文件）";
            } else {
                r.c3 = L"已读出";
                r.c4 = pwd;
                ++withPwd;
                revealed_.push_back({ name, pwd });
                ctx.Log(L"  ✓ " + name + L" → " + pwd);
            }
            r.sortKey = (unsigned long long)(names.size() - idx);
            out.rows.push_back(r);
        }

        out.summary = L"共 " + U64(names.size()) + L" 个配置文件，" +
                      U64((unsigned long long)withPwd) + L" 个读出密码";
        out.hint = T(L"仅本机当前用户的已保存网络；企业级（802.1X）证书认证不出明文密码");
        out.canClean = withPwd > 0;
        ctx.Log(out.summary);
        ctx.Report(100, out.summary);
    }

private:
    static wstring AfterColon(const wstring& line) {
        size_t p = line.rfind(L':');
        return (p == wstring::npos) ? wstring() : line.substr(p + 1);
    }

    vector<std::pair<wstring, wstring>> revealed_;
};

// ===========================================================================
//  注册（注册顺序即界面中的显示顺序）
// ===========================================================================
REGISTER_MODULE(PwdRevealModule);
REGISTER_MODULE(PwdZipModule);
REGISTER_MODULE(PwdCredModule);
REGISTER_MODULE(PwdWifiModule);

} // namespace mz
