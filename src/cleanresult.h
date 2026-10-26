// ============================================================================
//  清理结果模型 + 智能删除引擎（v2.8 R5）
//  ---------------------------------------------------------------------------
//  解决 v2.7 的三个核心缺陷（见 dev-plan-v28.md §1.2.2）：
//    ① 失败被吞：RemoveTree/CleanDirectoryContents 忽略全部删除返回值，
//       UI 永远显示成功、释放量用「删除前估值」冒充
//    ② 「删不掉」无原因：占用/权限/路径过长只给错误码不给中文
//    ③ 无占位删除：系统进程占用的文件（DELETE_PENDING 场景）只能反复扫描出来
//  本文件提供：
//    * CleanReport      —— 逐文件回报（真实释放量 + 失败原因分布 + 占位删除计数）
//    * ErrorTextZh      —— Win32 错误码 → 中文原因（集中化，后续模块共用）
//    * DeleteFileSmart  —— 策略矩阵 L1~L4
//    * CleanDirectoryContentsEx —— 逐文件清理，释放量=删除前后实测差值
//    * IsDangerousCleanRoot —— 顶层危险路径终检（防御纵深，对齐 winapp2.cpp
//      IsDangerousPattern 思路：高危根目录拒绝整清，仅统计）
// ============================================================================
#pragma once

#include "common.h"
#include <map>

namespace mz {

// 单个文件的删除结果（code = 0 成功）
struct CleanFileResult {
    wstring path;
    DWORD code = 0;
};

// 逐文件清理回报
struct CleanReport {
    unsigned long long freedBytes = 0;   // 真实释放（删除前后 DirSize 实测差值之和）
    int fileOk = 0;                      // 删除成功（含幂等：目标已不存在）
    int fileFail = 0;                    // 删除失败
    int rebootPending = 0;               // 占位删除（MOVEFILE_DELAY_UNTIL_REBOOT）
    vector<CleanFileResult> fails;       // 失败明细（上限 200 条进 UI）
    std::map<DWORD, int> failCodes;      // 错误码 → 数量（原因分布）

    void AddFail(const wstring& path, DWORD code) {
        ++fileFail;
        ++failCodes[code];
        if ((int)fails.size() < 200) fails.push_back({ path, code });
    }
    // 「失败原因分布」一行文本，如「被其他程序占用 ×3、需要管理员权限 ×2」
    wstring FailSummary() const;
};

// Win32 错误码 → 中文原因（集中化；未知码给原始码数字）
wstring ErrorTextZh(DWORD code);

// 智能删除单文件（策略矩阵，全部成功路径 fileOk+1）：
//   L1  DeleteFileW 常规删；路径超长（>248 字符）自动加 \\?\ 前缀重试
//   L2  错误码 5（拒绝访问）→ 清 文件+父目录 只读/隐藏/系统属性 后重试
//   L3  错误码 32/33（占用/锁定）→ 退避重试 ×3（100/300/600ms，Sleep 可被 cancel 打断）
//   L4  仍失败 → MoveFileExW(DELAY_UNTIL_REBOOT) 占位删除
//       （受 *rebootBudget 限制，单次清理上限 50 条防滥用；成功则 rebootPending+1）
bool DeleteFileSmart(const wstring& path, const CancelFn& cancel,
                     CleanReport& report, int* rebootBudget);

// 清空目录内容（保留目录本身），逐文件走 DeleteFileSmart，支持 \\?\ 长路径。
// 返回本次实际释放字节数（删除前后 DirSize 实测差值），累计写入 report.freedBytes。
// 顶层危险路径终检命中时拒绝执行（返回 0，不上报失败——属设计性跳过）。
unsigned long long CleanDirectoryContentsEx(const wstring& dir, const CancelFn& cancel,
                                            CleanReport& report, int* rebootBudget);

// 顶层危险路径终检：清理根命中系统关键目录 → 拒绝执行（仅统计）。
// 拒绝范围：盘根、Windows 本体与关键子目录（system32/driverstore/winsxs/boot/
// fonts/installer/config/registration）、Program Files 整树、ProgramData 及其
// 第一层子目录、用户根目录（C:\Users\<user> 本身）、回收站/卷信息目录。
// 更深层（如 AppData\Local\Temp、Windows\Logs\CBS、ProgramData\...\Scans）放行。
bool IsDangerousCleanRoot(const wstring& dir);

} // namespace mz
