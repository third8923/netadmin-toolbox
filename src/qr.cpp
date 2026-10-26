// ============================================================================
//  NetAdmin Toolbox — 自研二维码编码器实现（v2.16，零第三方依赖）
//  细节约定（row=行自上而下，col=列自左而右；bit 索引均为 LSB=0）：
//    * Format info：15 位 = 2 位 ECC(M=00) + 3 位掩码，BCH(15,5) 生成式
//      0x537，异或掩码 0x5412；两份拷贝按标准 L 形排布，dark module 恒黑
//    * Version info（v≥7）：18 位 = 6 位版本 + BCH(18,6) 生成式 0x1F25，
//      两份互为转置（bottom-left / top-right 3×6 块）
//    * Alignment 中心表（v1~10），与 finder 重叠的三个组合跳过
//    * 数据 zigzag 自右下起，跳过 col 6；upward 判定 (right+1)&2==0
//    * 剩余位：v2~6 为 7 bit，补 0
// ============================================================================
#include "qr.h"
#include "common.h"

#include <cstdint>
#include <cstring>
#include <algorithm>

namespace qr {

#ifdef QR_TEST_DUMP
void (*g_testDumpSeq)(const unsigned char* seq, int len) = nullptr;
#endif

namespace {

using std::string;
using std::vector;

// ---------------------------------------------------------------- GF(256)

// 本原多项式 0x11D 的指数/对数表（惰性构建，进程级只建一次）
struct GfTables {
    uint8_t exp[512];
    uint8_t log[256];
    GfTables() {
        uint16_t x = 1;
        for (int i = 0; i < 255; ++i) {
            exp[i] = (uint8_t)x;
            log[x] = (uint8_t)i;
            x <<= 1;
            if (x & 0x100) x ^= 0x11D;
        }
        for (int i = 255; i < 512; ++i) exp[i] = exp[i - 255];
    }
};

const GfTables& Gf() { static const GfTables t; return t; }

uint8_t GfMul(uint8_t a, uint8_t b) {
    if (a == 0 || b == 0) return 0;
    return Gf().exp[Gf().log[a] + Gf().log[b]];
}

// Reed-Solomon 生成多项式系数（降幂数组，去掉首项 1 后的 n 个系数）：
// (x-α^0)(x-α^1)…(x-α^{n-1}) 逐次相乘；按度数缓存（最大用到 26）。
// 乘 (x + α^k)：N[j] = D[j] + α^k·D[j-1]（越界视为 0，D[0] 恒为 1）。
const vector<uint8_t>& RsGenerator(int degree) {
    static vector<vector<uint8_t>> cache = [] {
        vector<vector<uint8_t>> c(64);
        for (int n = 1; n <= 63; ++n) {
            vector<uint8_t> poly{ 1 };              // 降幂：D[j] = x^{deg-j} 系数
            for (int d = 0; d < n; ++d) {
                vector<uint8_t> nx(poly.size() + 1, 0);
                for (size_t j = 0; j < nx.size(); ++j) {
                    uint8_t v = 0;
                    if (j < poly.size()) v ^= poly[j];
                    if (j > 0) v ^= GfMul(poly[j - 1], Gf().exp[d]);
                    nx[j] = v;
                }
                poly = nx;
            }
            poly.erase(poly.begin());   // 去掉首项 1（x^n 系数）
            c[n] = poly;
        }
        return c;
    }();
    return cache[degree];
}

// 对单个数据块算 ECC（degree 个校验字节）
vector<uint8_t> RsEcc(const vector<uint8_t>& data, int degree) {
    const vector<uint8_t>& gen = RsGenerator(degree);
    vector<uint8_t> res(degree, 0);
    for (uint8_t b : data) {
        uint8_t factor = b ^ res[0];
        std::memmove(res.data(), res.data() + 1, (size_t)(degree - 1));
        res[degree - 1] = 0;
        for (int i = 0; i < degree; ++i)
            res[i] ^= GfMul(gen[i], factor);
    }
    return res;
}

// ---------------------------------------------------------------- 版本表

struct VerSpec {
    int ver;        // 版本号
    int side;       // 边长（模块数）
    int totalCw;    // 码字总数
    int ecPerBlock; // 每块 ECC 字节
    int blocks;     // 数据块数
    int dataCw;     // 数据码字总数
};

// ECC M 级 v1~10（ISO/IEC 18004 表，多块结构=先短块后长块均匀分布）
const VerSpec kVers[] = {
    { 1, 21,  26, 10, 1,  16 },
    { 2, 25,  44, 16, 1,  28 },
    { 3, 29,  70, 26, 1,  44 },
    { 4, 33, 100, 18, 2,  64 },
    { 5, 37, 134, 24, 2,  86 },
    { 6, 41, 172, 16, 4, 108 },
    { 7, 45, 196, 18, 4, 124 },
    { 8, 49, 242, 22, 4, 154 },
    { 9, 53, 292, 22, 5, 182 },
    { 10, 57, 346, 26, 5, 216 },
};
const int kVerCount = (int)(sizeof(kVers) / sizeof(kVers[0]));
const int kMaxPayload = 213;   // v10-M byte 容量

// byte mode 计数位宽：v1~9 为 8 bit，v10+ 为 16 bit
int CountBits(int ver) { return ver <= 9 ? 8 : 16; }

// byte mode 容量（字节）
int ByteCapacity(const VerSpec& v) {
    return (v.dataCw * 8 - 4 - CountBits(v.ver)) / 8;
}

// Alignment 图案中心坐标表
vector<int> AlignCenters(int ver) {
    switch (ver) {
    case 1:  return {};
    case 2:  return { 6, 18 };
    case 3:  return { 6, 22 };
    case 4:  return { 6, 26 };
    case 5:  return { 6, 30 };
    case 6:  return { 6, 34 };
    case 7:  return { 6, 22, 38 };
    case 8:  return { 6, 24, 42 };
    case 9:  return { 6, 26, 46 };
    default: return { 6, 28, 50 };
    }
}

// ---------------------------------------------------------------- 工作矩阵

struct QrMatrix {
    int size = 0;
    vector<vector<bool>> dark;       // 模块颜色
    vector<vector<bool>> isFunction; // 功能模块（数据不可占用、掩码不参与）

    void Init(int n) {
        size = n;
        dark.assign(n, vector<bool>(n, false));
        isFunction.assign(n, vector<bool>(n, false));
    }
    void Set(int r, int c, bool isDark) {
        if (r >= 0 && r < size && c >= 0 && c < size) {
            dark[r][c] = isDark;
            isFunction[r][c] = true;
        }
    }
    // 5×5 alignment：外圈黑、内圈白、中心黑
    void DrawAlign(int r, int c) {
        for (int dr = -2; dr <= 2; ++dr)
            for (int dc = -2; dc <= 2; ++dc)
                Set(r + dr, c + dc,
                    dr == 0 && dc == 0 ? true : (std::max(std::abs(dr), std::abs(dc)) != 1));
    }
    void DrawFinder(int r, int c) {   // (r,c) 为 7×7 左上角
        for (int dr = -1; dr <= 7; ++dr)      // -1/7 = 分隔条（白）
            for (int dc = -1; dc <= 7; ++dc) {
                int rr = r + dr, cc = c + dc;
                if (rr < 0 || rr >= size || cc < 0 || cc >= size) continue;
                if (dr == -1 || dr == 7 || dc == -1 || dc == 7) {
                    Set(rr, cc, false);       // 分隔条恒白
                    continue;
                }
                bool edge = dr == 0 || dr == 6 || dc == 0 || dc == 6;
                bool core = dr >= 2 && dr <= 4 && dc >= 2 && dc <= 4;
                Set(rr, cc, edge ? true : core);
            }
    }
};

// Format info 15 位（含 0x5412 掩码异或）
int FormatBits(int maskId) {
    int data = (0 /*ECC M = 00*/ << 3) | maskId;
    int rem = data;
    for (int i = 0; i < 10; ++i)
        rem = (rem << 1) ^ ((rem >> 9) * 0x537);
    return ((data << 10) | (rem & 0x3FF)) ^ 0x5412;
}

// Version info 18 位（v≥7）
int VersionBits(int ver) {
    int rem = ver;
    for (int i = 0; i < 12; ++i)
        rem = (rem << 1) ^ ((rem >> 11) * 0x1F25);
    return (ver << 12) | (rem & 0xFFF);
}

void DrawFunctionPatterns(QrMatrix& m, int ver) {
    const int n = m.size;
    // timing（先行后列，第 6 行/列黑白相间）
    for (int i = 0; i < n; ++i) {
        m.Set(6, i, i % 2 == 0);
        m.Set(i, 6, i % 2 == 0);
    }
    // 三个 finder（左上/右上/左下）
    m.DrawFinder(0, 0);
    m.DrawFinder(0, n - 7);
    m.DrawFinder(n - 7, 0);
    // alignment（跳过与 finder 重叠的三个组合）
    vector<int> ap = AlignCenters(ver);
    for (size_t i = 0; i < ap.size(); ++i)
        for (size_t j = 0; j < ap.size(); ++j) {
            if (i == 0 && (j == 0 || j == ap.size() - 1)) continue;
            if (j == 0 && i == ap.size() - 1) continue;
            m.DrawAlign((int)ap[i], (int)ap[j]);
        }
    // format info 预留位（先全置 0 占位，落掩码后重绘）
    for (int i = 0; i <= 5; ++i) m.Set(8, i, false);
    m.Set(8, 7, false);
    m.Set(8, 8, false);
    m.Set(7, 8, false);
    for (int i = 9; i < 15; ++i) m.Set(14 - i, 8, false);
    for (int i = 0; i < 8; ++i) m.Set(8, n - 1 - i, false);
    for (int i = 8; i < 15; ++i) m.Set(n - 15 + i, 8, false);
    m.Set(n - 8, 8, true);   // dark module（恒黑）
    // version info（v≥7）：两份互为转置
    if (ver >= 7) {
        int bits = VersionBits(ver);
        for (int i = 0; i < 18; ++i) {
            bool b = ((bits >> i) & 1) != 0;
            int a = n - 11 + i % 3, bIdx = i / 3;
            m.Set(bIdx, a, b);
            m.Set(a, bIdx, b);
        }
    }
}

void DrawFormatInfo(QrMatrix& m, int maskId) {
    const int n = m.size;
    int bits = FormatBits(maskId);
    for (int i = 0; i <= 5; ++i) m.Set(8, i, (bits >> i) & 1);
    m.Set(8, 7, (bits >> 6) & 1);
    m.Set(8, 8, (bits >> 7) & 1);
    m.Set(7, 8, (bits >> 8) & 1);
    for (int i = 9; i < 15; ++i) m.Set(14 - i, 8, (bits >> i) & 1);
    for (int i = 0; i < 8; ++i) m.Set(8, n - 1 - i, (bits >> i) & 1);
    for (int i = 8; i < 15; ++i) m.Set(n - 15 + i, 8, (bits >> i) & 1);
    m.Set(n - 8, 8, true);
}

// 掩码条件（返回 true = 该模块颜色取反）
bool MaskBit(int maskId, int r, int c) {
    switch (maskId) {
    case 0:  return (r + c) % 2 == 0;
    case 1:  return r % 2 == 0;
    case 2:  return c % 3 == 0;
    case 3:  return (r + c) % 3 == 0;
    case 4:  return (r / 2 + c / 3) % 2 == 0;
    case 5:  return ((r * c) % 2) + ((r * c) % 3) == 0;
    case 6:  return (((r * c) % 2) + ((r * c) % 3)) % 2 == 0;
    default: return (((r + c) % 2) + ((r * c) % 3)) % 2 == 0;
    }
}

// 掩码 penalty 评分（N1~N4 标准公式）
int MaskPenalty(const vector<vector<bool>>& m) {
    const int n = (int)m.size();
    int penalty = 0;
    // N1：行/列连续同色 ≥5
    for (int dir = 0; dir < 2; ++dir) {
        for (int i = 0; i < n; ++i) {
            int run = 1;
            for (int j = 1; j < n; ++j) {
                bool cur = dir == 0 ? m[i][j] : m[j][i];
                bool prev = dir == 0 ? m[i][j - 1] : m[j - 1][i];
                if (cur == prev) {
                    ++run;
                    if (j == n - 1 && run >= 5) penalty += 3 + run - 5;
                } else {
                    if (run >= 5) penalty += 3 + run - 5;
                    run = 1;
                }
            }
        }
    }
    // N2：2×2 同色块
    for (int r = 0; r < n - 1; ++r)
        for (int c = 0; c < n - 1; ++c) {
            bool v = m[r][c];
            if (m[r][c + 1] == v && m[r + 1][c] == v && m[r + 1][c + 1] == v)
                penalty += 3;
        }
    // N3：1:1:3:1:1 图案两侧 4 白（10111010000 或其反向 00001011101）
    auto ScanLine = [&](const vector<bool>& s) {
        int len = (int)s.size();
        for (int i = 0; i + 11 <= len; ++i) {
            bool okPos = true, okNeg = true;
            for (int k = 0; k < 11; ++k) {
                // 正向 10111010000：暗位 {0,2,3,4,6}；反向 00001011101：暗位 {4,6,7,8,10}
                bool pos = (k == 0 || k == 2 || k == 3 || k == 4 || k == 6);
                bool neg = (k == 4 || k == 6 || k == 7 || k == 8 || k == 10);
                if (s[i + k] != pos) okPos = false;
                if (s[i + k] != neg) okNeg = false;
            }
            if (okPos || okNeg) penalty += 40;
        }
    };
    for (int r = 0; r < n; ++r) {
        vector<bool> line(m[r]);                    // 行
        ScanLine(line);
        vector<bool> col(n);
        for (int i = 0; i < n; ++i) col[i] = m[i][r];
        ScanLine(col);
    }
    // N4：明暗比例
    int dark = 0;
    for (const auto& row : m)
        for (bool b : row) if (b) ++dark;
    int total = n * n;
    int k = (std::abs(dark * 20 - total * 10) + total - 1) / total - 1;
    penalty += k * 10;
    return penalty;
}

} // namespace

// ---------------------------------------------------------------- 编码入口

vector<vector<bool>> EncodeBytes(const string& utf8Payload) {
    if (utf8Payload.empty() || (int)utf8Payload.size() > kMaxPayload)
        return {};

    // ① 选版本
    const VerSpec* spec = nullptr;
    for (int i = 0; i < kVerCount; ++i) {
        if ((int)utf8Payload.size() <= ByteCapacity(kVers[i])) { spec = &kVers[i]; break; }
    }
    if (!spec) return {};
    const int n = spec->side;

    // ② 数据位流：mode(0100) + 计数 + 数据 + terminator + 填充
    vector<bool> bits;
    auto Append = [&](unsigned value, int len) {
        for (int i = len - 1; i >= 0; --i) bits.push_back(((value >> i) & 1) != 0);
    };
    Append(4, 4);                                            // byte mode
    Append((unsigned)utf8Payload.size(), CountBits(spec->ver));
    for (unsigned char ch : utf8Payload) Append(ch, 8);
    int capBits = spec->dataCw * 8;
    Append(0, std::min(4, capBits - (int)bits.size()));      // terminator ≤4 bit
    bits.resize((bits.size() + 7) / 8 * 8, false);           // 对齐字节
    for (int pad = 0; (int)bits.size() < capBits; ++pad)     // 0xEC / 0x11 交替
        Append(pad % 2 == 0 ? 0xEC : 0x11, 8);

    // ③ 分块 + RS ECC + 交织
    int nb = spec->blocks, base = spec->dataCw / nb, longer = spec->dataCw % nb;
    vector<vector<uint8_t>> blocks(nb), eccs(nb);
    int pos = 0;
    for (int b = 0; b < nb; ++b) {
        int len = base + (b < nb - longer ? 0 : 1);
        blocks[b].reserve(len);
        // 从位流按字节取
        for (int k = 0; k < len; ++k) {
            int byteIdx = pos + k;
            unsigned v = 0;
            for (int t = 0; t < 8; ++t) v = (v << 1) | (bits[byteIdx * 8 + t] ? 1u : 0u);
            blocks[b].push_back((uint8_t)v);
        }
        pos += len;
        eccs[b] = RsEcc(blocks[b], spec->ecPerBlock);
    }
    vector<uint8_t> seq;
    seq.reserve(spec->totalCw);
    int maxDc = base + (longer ? 1 : 0);
    for (int k = 0; k < maxDc; ++k)
        for (int b = 0; b < nb; ++b)
            if (k < (int)blocks[b].size()) seq.push_back(blocks[b][k]);
    for (int k = 0; k < spec->ecPerBlock; ++k)
        for (int b = 0; b < nb; ++b) seq.push_back(eccs[b][k]);

#ifdef QR_TEST_DUMP
    if (g_testDumpSeq) g_testDumpSeq(seq.data(), (int)seq.size());
#endif

    // ④ 位流 → 布尔串（含剩余位补 0）
    int totalBits = spec->totalCw * 8 + (spec->ver >= 2 && spec->ver <= 6 ? 7 : 0);
    vector<bool> stream(totalBits, false);
    for (size_t i = 0; i < seq.size() && (int)i * 8 < totalBits; ++i)
        for (int t = 0; t < 8 && (int)i * 8 + t < totalBits; ++t)
            stream[i * 8 + t] = ((seq[i] >> (7 - t)) & 1) != 0;

    // ⑤ 8 种掩码打分选最优
    int bestMask = 0, bestScore = -1;
    vector<vector<bool>> best;
    for (int maskId = 0; maskId < 8; ++maskId) {
        QrMatrix m;
        m.Init(n);
        DrawFunctionPatterns(m, spec->ver);
        int bitIdx = 0;
        for (int right = n - 1; right >= 1; right -= 2) {
            if (right == 6) right = 5;          // 跳过 col 6（timing）；随后自然落到 3
            bool upward = ((right + 1) & 2) == 0;
            for (int vert = 0; vert < n; ++vert) {
                for (int j = 0; j < 2; ++j) {
                    int c = right - j;
                    int r = upward ? n - 1 - vert : vert;
                    if (m.isFunction[r][c]) continue;
                    bool v = bitIdx < (int)stream.size() ? stream[bitIdx] : false;
                    m.dark[r][c] = v ^ MaskBit(maskId, r, c);
                    ++bitIdx;
                }
            }
        }
        DrawFormatInfo(m, maskId);
        int score = MaskPenalty(m.dark);
        if (bestScore < 0 || score < bestScore) {   // penalty 越低越好
            bestScore = score; bestMask = maskId; best = std::move(m.dark);
        }
    }
    (void)bestMask;
    return best;
}

vector<vector<bool>> Encode(const wstring& payload) {
    return EncodeBytes(mz::WToUtf8(payload));
}

// ---------------------------------------------------------------- 控制台文本

string ToConsoleUtf8(const vector<vector<bool>>& m) {
    if (m.empty()) return string();
    const int n = (int)m.size();
    const int Q = 4;   // quiet zone
    string out;
    out.reserve((size_t)(n + Q * 2) * (n / 2 + Q + 1) * 3 + 64);
    auto Blank = [&]() {
        for (int i = 0; i < Q * 2 + n; ++i) out += ' ';
        out += "\r\n";
    };
    // 行配对：▀ 上下双行合一（上黑下白 ▀ / 上白下黑 ▄ / 全黑 █ / 全白 空格）
    for (int i = 0; i < Q; ++i) Blank();
    for (int r = 0; r < n; r += 2) {
        for (int i = 0; i < Q; ++i) out += ' ';
        for (int c = 0; c < n; ++c) {
            bool top = m[r][c];
            bool bot = (r + 1 < n) ? m[r + 1][c] : false;
            if (top && bot)      out += "\xE2\x96\x88";   // █
            else if (top)        out += "\xE2\x96\x80";   // ▀
            else if (bot)        out += "\xE2\x96\x84";   // ▄
            else                 out += ' ';
        }
        for (int i = 0; i < Q; ++i) out += ' ';
        out += "\r\n";
    }
    for (int i = 0; i < Q; ++i) Blank();
    return out;
}

} // namespace qr
