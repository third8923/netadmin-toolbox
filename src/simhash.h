// ============================================================================
//  图像感知哈希（dHash 64 位）
//  ---------------------------------------------------------------------------
//  用 WIC 解码，缩放到 9x8 灰度，比较相邻像素生成 64 位 dHash。
//  用于替换「文件名+大小」的相似图片启发式：名字/尺寸/格式不同但画面相同的图
//  也能聚到一组。
//  支持 WIC 原生能解的格式：jpg/jpeg/png/bmp/gif/tiff（webp 需系统装解码器）。
//  单个图片解码失败只跳过，不中断整体。
//
//  链接：-lwindowscodecs（MinGW 有 libwindowscodecs.a）+ -lole32 -luuid
//  COM：本模块内部会调用 CoInitializeEx(APARTMENTTHREADED)，若调用方已初始化
//       会得到 S_FALSE 并正确配对 CoUninitialize；若当前线程是 MTA
//       （RPC_E_CHANGED_MODE），仍可继续使用 WIC。
// ============================================================================
#pragma once

#include "common.h"

namespace mz {

// 计算图片的 dHash（64 位）。失败（文件不存在/格式不支持/解码错误）返回 false。
bool ImageDHash(const wstring& path, unsigned long long* hash);

// Hamming 距离（0..64）
int HammingDistance(unsigned long long a, unsigned long long b);

// 对给定图片路径列表按 dHash 分组（距离 <= threshold 视为相似）。
// 只返回元素数 >= 2 的分组；组内顺序与输入一致，组间按组大小降序。
// progress / cancel 均可为空。
vector<vector<wstring>> GroupSimilarImages(const vector<wstring>& paths, int threshold,
                                           const ProgressFn& progress = nullptr,
                                           const CancelFn& cancel = nullptr);

} // namespace mz
