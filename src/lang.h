// ============================================================================
//  NetAdmin Toolbox — 多语言层（v2.12 R4）
//  ---------------------------------------------------------------------------
//  设计：
//    * 简体中文是「源语言」——全部界面/模块字符串本来就以简体写成，ZhCn 原样返回。
//    * 繁体中文：LCMapStringW(LCMAP_TRADITIONAL_CHINESE) 由操作系统逐字转换，
//      覆盖所有经过本层翻译入口的字符串（OS 内置映射表，零维护成本）。
//    * 英文：内置查表（界面主干 + 分组名 + 模块标题）；表中未登记的字符串
//      回退显示简体原文（v1 范围：模块详情/运行日志暂随中文，后续版本补全）。
//  翻译入口只有两个：
//    lang::T(const wchar_t* zh)  -> const wchar_t*（持久指针，UI 直接绘制）
//    lang::Tx(const wstring& zh) -> wstring（拼接场景）
//  语言选择持久化在 <exe>-theme.ini 的 [ui] lang（zh-CN / zh-TW / en-US）。
// ============================================================================
#pragma once

#include "common.h"

namespace mz {
namespace lang {

enum class Lang { ZhCn, ZhTw, En };

Lang Current();
void SetLang(Lang l);               // 切换并清空翻译缓存（不落盘，落盘用 SaveLang）
const wchar_t* NameOf(Lang l);      // 显示名：简体中文 / 繁體中文 / English
const wchar_t* IniValueOf(Lang l);  // ini 值：zh-CN / zh-TW / en-US
Lang Next(Lang l);                  // 循环切换顺序：简 -> 繁 -> 英 -> 简

void LoadOnStartup();               // 启动恢复（先于任何窗口创建）
void SaveLang();                    // 当前语言落盘

// 翻译入口：zh 必须是稳定的静态字符串或生命周期内的 wstring（查表键）
const wchar_t* T(const wchar_t* zh);
std::wstring Tx(const std::wstring& zh);

} // namespace lang
} // namespace mz
