// ============================================================================
//  注册表引擎 - 解析并应用 .reg 文件内容
//  支持: Windows Registry Editor Version 5.00 / REGEDIT4
//         [HKEY_...]、"name"=dword:xxxxxxxx、"name"="text"、hex(2):、@=、
//         -删除值、[] 前置 - 删除键
// ============================================================================
#pragma once

#include "common.h"

namespace mz {

struct RegPatchStats {
    int keysCreated = 0;
    int valuesSet = 0;
    int valuesDeleted = 0;
    int keysDeleted = 0;
    int errors = 0;
    vector<wstring> errorDetail;
};

// 解析并应用一段 .reg 内容到系统注册表
// content: 原始字节（支持 UTF-16LE 带 BOM 与 UTF-8）
RegPatchStats ApplyRegContent(const vector<unsigned char>& content, const LogSink& log);

// 读取一个字符串注册表值（用于状态检测）
bool ReadRegString(HKEY root, const wstring& subkey, const wstring& name, wstring* out);
bool ReadRegDword(HKEY root, const wstring& subkey, const wstring& name, DWORD* out);
bool ReadRegQword(HKEY root, const wstring& subkey, const wstring& name, unsigned long long* out);
// 读取任意类型值的原始字节（REG_BINARY / REG_MULTI_SZ / 含 NUL 的字符串等）
bool ReadRegBinary(HKEY root, const wstring& subkey, const wstring& name, vector<BYTE>* out);
// 写入 DWORD
bool WriteRegDword(HKEY root, const wstring& subkey, const wstring& name, DWORD value);

// 解析 "[HKEY_CURRENT_USER\...]" 这种完整路径到 root + subkey
bool SplitRegPath(const wstring& full, HKEY* root, wstring* subkey);

} // namespace mz
