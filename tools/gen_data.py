"""从内置规则集生成 C++ 可用的嵌入式数据文件

输出:
  1. data/features.tsv   - 40 个特性的元数据
  2. data/regfiles.h     - 内联的 .reg 内容（供编译进 exe）
"""

import json
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
UP = os.path.join(ROOT, "upstream")
OUT = os.path.join(ROOT, "data")

# 选定的 40 个高频特性（按分类分组，顺序即 UI 顺序）
SELECTED = [
    # ── 隐私与建议内容 (8) ──
    "DisableTelemetry", "DisableSuggestions", "DisableNotifications",
    "DisableLocationServices", "DisableFindMyDevice", "DisableLockscreenTips",
    "DisableEdgeAds", "DisableSettings365Ads",
    # ── AI (8) ──
    "DisableCopilot", "DisableRecall", "DisableClickToDo", "DisableAISvcAutoStart",
    "DisableEdgeAI", "DisableChromeAI", "DisablePaintAI", "DisableNotepadAI",
    # ── 外观 (3) ──
    "EnableDarkMode", "DisableTransparency", "DisableAnimations",
    # ── 游戏 (2) ──
    "DisableDVR", "DisableGameBarIntegration",
    # ── 开始菜单与搜索 (2) ──
    "DisableBing", "DisableStartPhoneLink",
    # ── 系统 (6) ──
    "DisableSettingsHome", "RevertContextMenu", "DisableMouseAcceleration",
    "DisableStickyKeys", "DisableFastStartup", "DisableBitlockerAutoEncryption",
    # ── 任务栏 (6) ──
    "TaskbarAlignLeft", "HideSearchTb", "HideTaskview", "HideChat",
    "EnableEndTask", "EnableLastActiveClick",
    # ── 文件资源管理器 (5) ──
    "ShowKnownFileExt", "ShowHiddenFolders", "HideGallery", "HideHome", "HideOnedrive",
]

# 中文分类名
CAT_ZH = {
    "Privacy & Suggested Content": "隐私与建议内容",
    "AI": "AI 功能",
    "Appearance": "外观效果",
    "Gaming": "游戏与录制",
    "Start Menu & Search": "开始菜单与搜索",
    "System": "系统行为",
    "Taskbar": "任务栏",
    "File Explorer": "文件资源管理器",
    "Windows Update": "Windows 更新",
    "Multi-tasking": "多任务",
    "Other": "其他",
}

# 需要重启资源管理器的特性
NEEDS_EXPLORER = {
    "TaskbarAlignLeft", "HideSearchTb", "HideTaskview", "HideChat",
    "EnableEndTask", "EnableLastActiveClick", "ShowKnownFileExt",
    "ShowHiddenFolders", "HideGallery", "HideHome", "HideOnedrive",
    "RevertContextMenu", "DisableStickyKeys",
}

# 需要重启系统才完全生效
NEEDS_REBOOT = {"DisableFastStartup", "DisableBitlockerAutoEncryption"}


def build_reg_index():
    regdir = os.path.join(UP, "Regfiles")
    idx = {}
    for dp, _dn, fs in os.walk(regdir):
        for fn in fs:
            if fn.lower().endswith(".reg"):
                idx[fn.lower()] = os.path.join(dp, fn)
    return idx


def main():
    os.makedirs(OUT, exist_ok=True)
    feats_json = json.load(open(os.path.join(UP, "Config", "Features.json"), encoding="utf-8-sig"))
    zh_json = json.load(open(os.path.join(UP, "Config", "Languages", "zh-CN", "Features.json"),
                             encoding="utf-8-sig"))
    F = {f["FeatureId"]: f for f in feats_json["Features"]}
    # 中文语言文件的 Features 是「字典」(id -> {Label, ToolTip})
    raw_zh = zh_json["Features"]
    ZH = raw_zh if isinstance(raw_zh, dict) else {f["FeatureId"]: f for f in raw_zh}
    idx = build_reg_index()

    rows = []
    reg_blobs = {}     # key -> bytes
    missing = []

    for fid in SELECTED:
        f = F.get(fid)
        if not f:
            missing.append(fid)
            continue
        zh = ZH.get(fid, {})
        rk = f.get("RegistryKey")
        uk = f.get("RegistryUndoKey")

        # 定位 apply 文件
        ap = idx.get(rk.lower()) if rk else None
        if not ap:
            missing.append(f"{fid}:apply({rk})")
            continue

        # 定位 undo 文件：优先 Undo__ 前缀，否则用原名
        up = None
        if uk:
            up = idx.get(("undo__" + uk).lower()) or idx.get(uk.lower())
        if not up:
            missing.append(f"{fid}:undo({uk})")
            continue

        label, tip = f.get("Label", ""), f.get("ToolTip", "")
        zlab = zh.get("Label") or label
        ztip = zh.get("ToolTip") or tip

        apply_key = f"apply_{fid}"
        undo_key = f"undo_{fid}"
        reg_blobs[apply_key] = open(ap, "rb").read()
        reg_blobs[undo_key] = open(up, "rb").read()

        cat = f.get("Category") or "Other"
        flags = []
        if fid in NEEDS_EXPLORER:
            flags.append("EXPLORER")
        if fid in NEEDS_REBOOT:
            flags.append("REBOOT")

        rows.append({
            "id": fid,
            "cat": cat,
            "catZh": CAT_ZH.get(cat, cat),
            "label": label,
            "labelZh": zlab,
            "tip": tip,
            "tipZh": ztip,
            "applyKey": apply_key,
            "undoKey": undo_key,
            "applyText": f.get("ApplyText", ""),
            "undoLabel": f.get("UndoLabel") or f"恢复: {zlab}",
            "flags": "|".join(flags),
        })

    if missing:
        print("缺失项:", missing)

    # TSV（制表符分隔，字段内不得有 tab/换行）
    def clean(s):
        return (s or "").replace("\t", " ").replace("\r", " ").replace("\n", " ").strip()

    tsv = os.path.join(OUT, "features.tsv")
    with open(tsv, "w", encoding="utf-8", newline="\n") as fh:
        fh.write("#id\tcat\tcatZh\tlabel\tlabelZh\ttipZh\tapplyKey\tundoKey\tapplyText\tundoLabel\tflags\n")
        for r in rows:
            fh.write("\t".join(clean(r[k]) for k in
                               ("id", "cat", "catZh", "label", "labelZh", "tipZh",
                                "applyKey", "undoKey", "applyText", "undoLabel", "flags")) + "\n")

    # C++ 头文件：原始字节数组（reg 内容）
    h = os.path.join(OUT, "regfiles.h")
    with open(h, "w", encoding="utf-8", newline="\n") as fh:
        fh.write("// 自动生成，请勿手改 —— 内置 Windows 优化规则库\n")
        fh.write("#pragma once\n#include <cstddef>\n\n")
        fh.write("struct RegBlob { const char* key; const unsigned char* data; unsigned int size; };\n\n")
        for key, data in reg_blobs.items():
            fh.write(f"static const unsigned char {key}[] = {{")
            for i, b in enumerate(data):
                if i % 24 == 0:
                    fh.write("\n    ")
                fh.write(f"0x{b:02x},")
            fh.write("\n};\n\n")
        fh.write("static const RegBlob REG_BLOBS[] = {\n")
        for key, data in reg_blobs.items():
            fh.write(f'    {{"{key}", {key}, (unsigned int)sizeof({key})}},\n')
        fh.write("};\n")
        fh.write(f"static const size_t REG_BLOB_COUNT = {len(reg_blobs)};\n")

    # C++ 头文件：内嵌 TSV 元数据（UTF-8 字节）
    tsv_bytes = open(tsv, "rb").read()
    th = os.path.join(OUT, "features_tsv.h")
    with open(th, "w", encoding="utf-8", newline="\n") as fh:
        fh.write("// 自动生成，请勿手改\n")
        fh.write("#pragma once\n#include <cstddef>\n\n")
        fh.write("static const unsigned char FEATURES_TSV[] = {")
        for i, b in enumerate(tsv_bytes):
            if i % 24 == 0:
                fh.write("\n    ")
            fh.write(f"0x{b:02x},")
        fh.write("\n    0x00\n};\n")
        fh.write(f"static const size_t FEATURES_TSV_SIZE = {len(tsv_bytes)};\n")

    total = sum(len(v) for v in reg_blobs.values())
    print(f"特性: {len(rows)} 个")
    print(f"reg 文件: {len(reg_blobs)} 个, 共 {total/1024:.1f} KB")
    print(f"输出: {tsv}")
    print(f"输出: {h}")
    return 0 if not missing else 1


if __name__ == "__main__":
    sys.exit(main())
