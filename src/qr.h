// ============================================================================
//  NetAdmin Toolbox — 自研二维码编码器（v2.16，零第三方依赖）
//  ---------------------------------------------------------------------------
//  面向 net.mesh「生成接入二维码」：把组网配置字符串编码为 QR 矩阵。
//  支持范围（刻意收窄、够用即可）：
//    * Byte mode（8-bit 字节，UTF-8 载荷统一转字节）
//    * 纠错级别 M
//    * Version 1 ~ 10（v10-M byte 容量 213 字节，覆盖典型/边界配置串）
//  实现：数据段编码（mode 4 + 计数 + 数据 + terminator + 0xEC/0x11 填充）
//    → Reed-Solomon ECC（GF(256)，本原多项式 0x11D，生成多项式按度数现算）
//    → 分块交织（v4+ 多块，数据列序交织 + ECC 列序交织）
//    → 矩阵布局（finder/separator/timing/alignment/version info/dark module）
//    → 8 种掩码按 penalty 评分（N1~N4 标准公式）选最优
//  输出：bool 矩阵（true = 黑模块，不含 quiet zone——由绘制方按 4 模块留白）。
//  参考：ISO/IEC 18004；布局/掩码公式经 Python OpenCV 解码回归验证。
// ============================================================================
#pragma once

#include <string>
#include <vector>

namespace qr {

// 编码 UTF-8 字节载荷 → QR 矩阵（size×size，true = 黑模块）。
// 载荷超过容量（v10-M = 213 字节）或为空时返回空矩阵。
std::vector<std::vector<bool>> EncodeBytes(const std::string& utf8Payload);

// 便捷入口：宽字符串先转 UTF-8（中文服务器地址等统一按字节编码）
std::vector<std::vector<bool>> Encode(const std::wstring& payload);

// 矩阵 → 控制台文本（■/▀/▄/空格 半块字符，含 4 模块 quiet zone 空白）。
// 返回 UTF-8 编码字节，供终端直出扫码。
std::string ToConsoleUtf8(const std::vector<std::vector<bool>>& m);

} // namespace qr
