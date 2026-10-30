// ============================================================================
//  icons.cpp — 自研 24×24 矢量线性图标库（v2.18）
//  ---------------------------------------------------------------------------
//  数据驱动：每个图标 = 一组图元（El）。图元在 24×24 设计网格上以浮点坐标
//  描述，渲染时按目标边长等比缩放并居中，GDI+ 抗锯齿输出。
//
//  图元类型（n 字段仅 Poly/PolyFill 使用 = 点数，其余填 0）：
//    kPoly     折线描边（a = x0,y0, x1,y1, ...，不闭合）
//    kPolyFill 闭合多边形（先填充再同宽描边，保证轮廓粗细一致）
//    kCircle   圆环描边（a = cx,cy,r）
//    kDot      实心圆点（a = cx,cy,r）—— 仅用于视觉配重的小点
//    kRect     圆角矩形描边（a = x,y,w,h,corner）
//    kArc      椭圆弧描边（a = cx,cy,rx,ry,start°,sweep°；屏幕坐标系，
//              0°=3 点钟方向，角度顺时针为正，与 GDI+ DrawArc 一致）
// ============================================================================
#include "ui/icons.h"
#include "ui/gdi.h"

#include <objidl.h>
#include <gdiplus.h>

#include <algorithm>
#include <cstdint>

namespace mz {
namespace ui {
namespace icon {

namespace {

enum ElKind : uint8_t { kPoly = 0, kPolyFill, kCircle, kDot, kRect, kArc };

struct El {
    uint8_t kind;
    uint8_t n;          // Poly/PolyFill 的点数，其余 0
    float a[14];
};

struct IconDef {
    const El* els;
    uint8_t n;
};

// ---------------------------------------------------------------- 图标定义
// 命名习惯：按模块 ID 分段注释。坐标均为 24 网格浮点值。

// 通用回落：应用方块（圆角方 + 中心点）
static const El kGenericEls[] = {
    { kRect, 0, { 5, 5, 14, 14, 3 } },
    { kDot,  0, { 12, 12, 1.6f } },
};

// ---- 磁盘清理族：垃圾桶母形 ------------------------------------------------
// clean.basic 基础清理：垃圾桶
static const El kCleanBasic[] = {
    { kPoly, 2, { 4.5f, 6.2f, 19.5f, 6.2f } },                        // 盖沿
    { kPoly, 4, { 9.5f, 6.0f, 9.5f, 3.6f, 14.5f, 3.6f, 14.5f, 6.0f } }, // 提手
    { kRect, 0, { 5.8f, 6.2f, 12.4f, 13.8f, 2 } },                    // 桶身
    { kPoly, 2, { 10.2f, 9.8f, 10.2f, 16.6f } },                      // 竖纹 L
    { kPoly, 2, { 13.8f, 9.8f, 13.8f, 16.6f } },                      // 竖纹 R
};
// clean.system 系统清理：垃圾桶 + 齿轮（深扫）
static const El kCleanSystem[] = {
    { kPoly, 2, { 3.8f, 7.0f, 17.0f, 7.0f } },
    { kPoly, 4, { 8.0f, 6.8f, 8.0f, 4.8f, 12.5f, 4.8f, 12.5f, 6.8f } },
    { kRect, 0, { 5.0f, 7.2f, 11.0f, 13.0f, 2 } },
    { kPoly, 2, { 8.8f, 10.5f, 8.8f, 16.8f } },
    { kPoly, 2, { 12.2f, 10.5f, 12.2f, 16.8f } },
    { kCircle, 0, { 18.7f, 5.5f, 2.6f } },                            // 齿轮盘
    { kPoly, 2, { 18.7f, 1.9f, 18.7f, 2.9f } },                       // 齿 N
    { kPoly, 2, { 18.7f, 8.1f, 18.7f, 9.1f } },                       // 齿 S
    { kPoly, 2, { 15.1f, 5.5f, 16.1f, 5.5f } },                       // 齿 W
    { kPoly, 2, { 21.3f, 5.5f, 22.3f, 5.5f } },                       // 齿 E
};
// clean.wechat 微信清理：双气泡
static const El kCleanWechat[] = {
    { kRect, 0, { 3.5f, 4.5f, 11.0f, 8.5f, 3 } },
    { kPoly, 2, { 6.2f, 13.2f, 5.2f, 16.4f } },                       // 后气泡尾
    { kRect, 0, { 9.0f, 9.5f, 11.5f, 8.5f, 3 } },
    { kPoly, 2, { 16.8f, 18.2f, 18.0f, 21.0f } },                     // 前气泡尾
};
// clean.qq QQ 清理：气泡 + 闪星
static const El kCleanQq[] = {
    { kRect, 0, { 3.5f, 4.0f, 15.0f, 11.5f, 4 } },
    { kPoly, 3, { 8.0f, 15.7f, 6.8f, 19.8f, 13.0f, 15.9f } },         // 气泡尾
    { kPoly, 2, { 20.5f, 3.3f, 20.5f, 6.3f } },                       // 星竖
    { kPoly, 2, { 19.0f, 4.8f, 22.0f, 4.8f } },                       // 星横
};
// clean.appcache 应用缓存：层叠
static const El kCleanAppcache[] = {
    { kPolyFill, 4, { 12.0f, 3.5f, 20.5f, 8.0f, 12.0f, 12.5f, 3.5f, 8.0f } },
    { kPoly, 3, { 3.5f, 12.5f, 12.0f, 17.0f, 20.5f, 12.5f } },
    { kPoly, 3, { 3.5f, 17.0f, 12.0f, 21.5f, 20.5f, 17.0f } },
};

// ---- 空间分析族 ------------------------------------------------------------
// scan.large 大文件查找：文档 + 放大镜
static const El kScanLarge[] = {
    { kRect,   0, { 4.0f, 3.5f, 10.5f, 14.0f, 2 } },
    { kPoly,   2, { 6.8f, 7.5f, 11.5f, 7.5f } },
    { kPoly,   2, { 6.8f, 10.5f, 11.5f, 10.5f } },
    { kCircle, 0, { 16.0f, 15.5f, 4.0f } },
    { kPoly,   2, { 19.0f, 18.5f, 21.5f, 21.0f } },
};
// scan.dup 重复文件：双叠方块
static const El kScanDup[] = {
    { kRect, 0, { 4.0f, 4.0f, 11.5f, 11.5f, 2 } },
    { kRect, 0, { 8.5f, 8.5f, 11.5f, 11.5f, 2 } },
};
// scan.similar 相似文件：后块带点 + 前块
static const El kScanSimilar[] = {
    { kRect, 0, { 4.0f, 4.0f, 11.5f, 11.5f, 2 } },
    { kDot,  0, { 7.5f, 7.5f, 1.2f } },
    { kRect, 0, { 8.5f, 8.5f, 11.5f, 11.5f, 2 } },
};
// scan.similarimg 相似图片：图片 + 放大镜
static const El kScanSimilarimg[] = {
    { kRect,   0, { 3.5f, 4.0f, 12.0f, 10.5f, 2 } },
    { kPoly,   5, { 5.5f, 13.5f, 8.5f, 9.8f, 10.5f, 12.0f, 12.5f, 9.2f, 14.0f, 13.5f } },
    { kDot,    0, { 6.5f, 7.0f, 1.0f } },
    { kCircle, 0, { 16.5f, 16.0f, 4.0f } },
    { kPoly,   2, { 19.3f, 18.8f, 21.5f, 21.0f } },
};
// scan.dirs 目录大小：矩形树图
static const El kScanDirs[] = {
    { kRect, 0, { 3.5f, 3.5f, 17.0f, 17.0f, 2 } },
    { kPoly, 2, { 11.5f, 4.5f, 11.5f, 19.5f } },
    { kPoly, 2, { 11.5f, 12.0f, 19.5f, 12.0f } },
    { kPoly, 2, { 4.5f, 14.5f, 11.5f, 14.5f } },
};
// scan.apps 软件占用：2×2 应用格
static const El kScanApps[] = {
    { kRect, 0, { 4.0f, 4.0f, 7.0f, 7.0f, 1.8f } },
    { kRect, 0, { 13.0f, 4.0f, 7.0f, 7.0f, 1.8f } },
    { kRect, 0, { 4.0f, 13.0f, 7.0f, 7.0f, 1.8f } },
    { kRect, 0, { 13.0f, 13.0f, 7.0f, 7.0f, 1.8f } },
};

// ---- 网络管理族 ------------------------------------------------------------
// net.sharemap 共享扫描与网盘映射：文件夹 + 扫描点
static const El kNetSharemap[] = {
    { kPolyFill, 6, { 3.5f, 18.5f, 3.5f, 6.5f, 9.2f, 6.5f, 11.2f, 9.0f, 20.5f, 9.0f, 20.5f, 18.5f } },
    { kDot, 0, { 8.0f, 13.8f, 1.1f } },
    { kDot, 0, { 11.5f, 13.8f, 1.1f } },
    { kDot, 0, { 15.0f, 13.8f, 1.1f } },
};
// net.sharehost 共享部署：双舱服务器
static const El kNetSharehost[] = {
    { kRect, 0, { 4.0f, 4.0f, 16.0f, 6.5f, 2 } },
    { kDot,  0, { 7.0f, 7.2f, 1.1f } },
    { kPoly, 2, { 10.0f, 7.2f, 17.0f, 7.2f } },
    { kRect, 0, { 4.0f, 13.5f, 16.0f, 6.5f, 2 } },
    { kDot,  0, { 7.0f, 16.7f, 1.1f } },
    { kPoly, 2, { 10.0f, 16.7f, 17.0f, 16.7f } },
};
// net.dnsopt DNS 优化：地球 + 上行箭头
static const El kNetDnsopt[] = {
    { kCircle, 0, { 11.0f, 10.5f, 7.0f } },
    { kPoly,   2, { 4.0f, 10.5f, 18.0f, 10.5f } },
    { kPoly,   2, { 11.0f, 3.5f, 11.0f, 17.5f } },
    { kPoly,   2, { 19.5f, 20.5f, 19.5f, 16.5f } },
    { kPoly,   3, { 17.7f, 18.3f, 19.5f, 16.5f, 21.3f, 18.3f } },
};
// net.adapters 网络适配器：以太网插头
static const El kNetAdapters[] = {
    { kRect, 0, { 7.0f, 3.5f, 10.0f, 9.0f, 2 } },
    { kPoly, 2, { 9.5f, 12.5f, 9.5f, 15.5f } },
    { kPoly, 2, { 12.0f, 12.5f, 12.0f, 15.5f } },
    { kPoly, 2, { 14.5f, 12.5f, 14.5f, 15.5f } },
    { kPoly, 2, { 12.0f, 15.5f, 12.0f, 20.5f } },
};
// net.conn 网络连接：链环
static const El kNetConn[] = {
    { kCircle, 0, { 7.5f, 12.0f, 3.6f } },
    { kCircle, 0, { 16.5f, 12.0f, 3.6f } },
    { kPoly,   2, { 11.1f, 12.0f, 12.9f, 12.0f } },
};
// net.route 路由表：分支路径 + 节点
static const El kNetRoute[] = {
    { kDot,  0, { 4.5f, 18.5f, 1.7f } },
    { kPoly, 4, { 4.5f, 18.5f, 10.5f, 18.5f, 15.0f, 6.5f, 20.0f, 6.5f } },
    { kDot,  0, { 20.0f, 6.5f, 1.7f } },
};
// net.arp ARP 表：表格行
static const El kNetArp[] = {
    { kRect, 0, { 3.5f, 5.0f, 17.0f, 14.0f, 2 } },
    { kPoly, 2, { 4.5f, 9.7f, 19.5f, 9.7f } },
    { kPoly, 2, { 4.5f, 14.3f, 19.5f, 14.3f } },
    { kDot,  0, { 7.0f, 7.4f, 1.0f } },
    { kDot,  0, { 7.0f, 12.0f, 1.0f } },
    { kDot,  0, { 7.0f, 16.6f, 1.0f } },
};
// net.flushdns 刷新 DNS 缓存：循环箭头
static const El kNetFlushdns[] = {
    { kArc, 0, { 12.0f, 11.5f, 7.2f, 7.2f, 120.0f, 300.0f } },        // 循环弧
    { kPolyFill, 3, { 5.8f, 16.2f, 9.2f, 16.3f, 7.6f, 19.1f } },      // 箭头
};
// net.wifi 无线网络：双弧 + 点
static const El kNetWifi[] = {
    { kDot, 0, { 12.0f, 17.8f, 1.8f } },
    { kArc, 0, { 12.0f, 17.8f, 4.6f, 4.6f, 225.0f, 90.0f } },
    { kArc, 0, { 12.0f, 17.8f, 8.0f, 8.0f, 225.0f, 90.0f } },
};
// net.mesh 组网助手：网状四节点
static const El kNetMesh[] = {
    { kPoly, 5, { 5.5f, 5.5f, 18.5f, 5.5f, 18.5f, 18.5f, 5.5f, 18.5f, 5.5f, 5.5f } },
    { kPoly, 2, { 5.5f, 5.5f, 18.5f, 18.5f } },
    { kPoly, 2, { 5.5f, 18.5f, 18.5f, 5.5f } },
    { kDot,  0, { 5.5f, 5.5f, 1.8f } },
    { kDot,  0, { 18.5f, 5.5f, 1.8f } },
    { kDot,  0, { 5.5f, 18.5f, 1.8f } },
    { kDot,  0, { 18.5f, 18.5f, 1.8f } },
    { kDot,  0, { 12.0f, 12.0f, 1.8f } },
};

// ---- 远程终端族 ------------------------------------------------------------
// net.sessions 远程会话：显示器 + 人
static const El kNetSessions[] = {
    { kRect,   0, { 3.5f, 4.0f, 17.0f, 11.5f, 2 } },
    { kPoly,   2, { 12.0f, 15.5f, 12.0f, 19.5f } },
    { kPoly,   2, { 8.5f, 19.5f, 15.5f, 19.5f } },
    { kCircle, 0, { 9.5f, 9.0f, 1.8f } },
    { kArc,    0, { 9.5f, 14.2f, 3.0f, 2.4f, 180.0f, 180.0f } },
    { kPoly,   2, { 14.5f, 8.0f, 17.5f, 8.0f } },
    { kPoly,   2, { 14.5f, 11.0f, 17.5f, 11.0f } },
};
// net.tunnel 端口转发隧道：拱门 + 虚线
static const El kNetTunnel[] = {
    { kPoly, 2, { 5.0f, 20.5f, 5.0f, 12.0f } },
    { kPoly, 2, { 19.0f, 20.5f, 19.0f, 12.0f } },
    { kArc,  0, { 12.0f, 12.0f, 7.0f, 8.0f, 180.0f, 180.0f } },
    { kPoly, 2, { 12.0f, 20.2f, 12.0f, 18.2f } },
    { kPoly, 2, { 12.0f, 15.6f, 12.0f, 13.6f } },
};
// net.serial 串口终端：D-SUB 连接器
static const El kNetSerial[] = {
    { kPoly, 5, { 3.5f, 7.5f, 20.5f, 7.5f, 18.0f, 16.5f, 6.0f, 16.5f, 3.5f, 7.5f } },
    { kPoly, 2, { 9.0f, 10.0f, 9.0f, 14.0f } },
    { kPoly, 2, { 12.0f, 10.0f, 12.0f, 14.0f } },
    { kPoly, 2, { 15.0f, 10.0f, 15.0f, 14.0f } },
};

// ---- 网络诊断族 ------------------------------------------------------------
// net.portscan 端口扫描：端口面板 + 放大镜
static const El kNetPortscan[] = {
    { kRect,   0, { 3.5f, 7.5f, 8.5f, 9.0f, 2 } },
    { kDot,    0, { 7.7f, 10.3f, 1.0f } },
    { kDot,    0, { 7.7f, 13.7f, 1.0f } },
    { kCircle, 0, { 16.0f, 14.0f, 4.4f } },
    { kPoly,   2, { 19.1f, 17.1f, 21.5f, 19.5f } },
};
// net.lanscan 局域网扫描：节点网 + 放大镜
static const El kNetLanscan[] = {
    { kDot,    0, { 5.5f, 5.5f, 1.6f } },
    { kDot,    0, { 12.5f, 4.5f, 1.6f } },
    { kDot,    0, { 6.5f, 11.5f, 1.6f } },
    { kPoly,   2, { 5.5f, 5.5f, 12.5f, 4.5f } },
    { kPoly,   2, { 5.5f, 5.5f, 6.5f, 11.5f } },
    { kCircle, 0, { 15.5f, 15.0f, 4.8f } },
    { kPoly,   2, { 18.4f, 17.9f, 21.0f, 20.5f } },
};
// net.diag 网络诊断：心电脉冲
static const El kNetDiag[] = {
    { kRect, 0, { 3.0f, 5.0f, 18.0f, 14.0f, 2.5f } },
    { kPoly, 6, { 5.5f, 12.0f, 8.5f, 12.0f, 10.5f, 8.0f, 13.0f, 16.0f, 14.5f, 12.0f, 18.5f, 12.0f } },
};

// ---- 系统管理族 ------------------------------------------------------------
// sys.tasks 计划任务：剪贴板 + 勾
static const El kSysTasks[] = {
    { kRect, 0, { 4.5f, 4.5f, 15.0f, 16.5f, 2 } },
    { kRect, 0, { 9.0f, 2.8f, 6.0f, 3.4f, 1.4f } },
    { kPoly, 3, { 8.5f, 12.5f, 11.0f, 15.0f, 15.5f, 9.5f } },
};
// sys.process 进程管理：芯片
static const El kSysProcess[] = {
    { kRect, 0, { 6.5f, 6.5f, 11.0f, 11.0f, 2 } },
    { kPoly, 2, { 3.5f, 9.5f, 6.5f, 9.5f } },
    { kPoly, 2, { 3.5f, 14.5f, 6.5f, 14.5f } },
    { kPoly, 2, { 17.5f, 9.5f, 20.5f, 9.5f } },
    { kPoly, 2, { 17.5f, 14.5f, 20.5f, 14.5f } },
    { kPoly, 2, { 9.5f, 3.5f, 9.5f, 6.5f } },
    { kPoly, 2, { 14.5f, 3.5f, 14.5f, 6.5f } },
    { kPoly, 2, { 9.5f, 17.5f, 9.5f, 20.5f } },
    { kPoly, 2, { 14.5f, 17.5f, 14.5f, 20.5f } },
    { kDot,  0, { 12.0f, 12.0f, 1.4f } },
};
// sys.services 服务管理：双齿轮
static const El kSysServices[] = {
    { kCircle, 0, { 9.5f, 9.5f, 4.0f } },
    { kPoly,   2, { 9.5f, 3.5f, 9.5f, 4.5f } },
    { kPoly,   2, { 9.5f, 14.5f, 9.5f, 15.5f } },
    { kPoly,   2, { 3.5f, 9.5f, 4.5f, 9.5f } },
    { kPoly,   2, { 14.5f, 9.5f, 15.5f, 9.5f } },
    { kDot,    0, { 9.5f, 9.5f, 1.3f } },
    { kCircle, 0, { 17.5f, 17.5f, 2.6f } },
    { kPoly,   2, { 17.5f, 13.9f, 17.5f, 14.9f } },
    { kPoly,   2, { 17.5f, 20.1f, 17.5f, 21.1f } },
    { kPoly,   2, { 13.9f, 17.5f, 14.9f, 17.5f } },
    { kPoly,   2, { 20.1f, 17.5f, 21.1f, 17.5f } },
    { kDot,    0, { 17.5f, 17.5f, 1.0f } },
};
// sys.acct 账户部署：人 + 加号
static const El kSysAcct[] = {
    { kCircle, 0, { 9.5f, 8.0f, 3.2f } },
    { kArc,    0, { 9.5f, 17.5f, 4.5f, 3.6f, 180.0f, 180.0f } },
    { kPoly,   2, { 18.5f, 6.5f, 18.5f, 12.5f } },
    { kPoly,   2, { 15.5f, 9.5f, 21.5f, 9.5f } },
};
// sys.domain 计算机名与域：楼宇
static const El kSysDomain[] = {
    { kRect, 0, { 5.0f, 4.0f, 14.0f, 17.0f, 1.5f } },
    { kDot,  0, { 9.0f, 8.0f, 1.0f } },
    { kDot,  0, { 12.0f, 8.0f, 1.0f } },
    { kDot,  0, { 15.0f, 8.0f, 1.0f } },
    { kDot,  0, { 9.0f, 11.5f, 1.0f } },
    { kDot,  0, { 12.0f, 11.5f, 1.0f } },
    { kDot,  0, { 15.0f, 11.5f, 1.0f } },
    { kPoly, 4, { 10.0f, 21.0f, 10.0f, 17.5f, 14.0f, 17.5f, 14.0f, 21.0f } },
};

// ---- 系统信息族 ------------------------------------------------------------
// sys.info 系统概览：显示器 + i
static const El kSysInfo[] = {
    { kRect, 0, { 3.0f, 4.0f, 18.0f, 12.0f, 2 } },
    { kPoly, 2, { 12.0f, 16.0f, 12.0f, 19.5f } },
    { kPoly, 2, { 8.5f, 19.5f, 15.5f, 19.5f } },
    { kDot,  0, { 12.0f, 7.6f, 1.1f } },
    { kPoly, 2, { 12.0f, 10.0f, 12.0f, 13.0f } },
};
// sys.disk 磁盘信息：硬盘
static const El kSysDisk[] = {
    { kRect, 0, { 3.5f, 7.5f, 17.0f, 9.0f, 2.5f } },
    { kDot,  0, { 17.0f, 12.0f, 1.2f } },
    { kPoly, 2, { 6.5f, 12.0f, 13.0f, 12.0f } },
};
// sys.startup 启动项：火箭
static const El kSysStartup[] = {
    { kPoly,   6, { 12.0f, 3.0f, 15.5f, 7.5f, 15.5f, 13.5f, 8.5f, 13.5f, 8.5f, 7.5f, 12.0f, 3.0f } },
    { kCircle, 0, { 12.0f, 8.5f, 1.7f } },
    { kPoly,   2, { 8.5f, 11.5f, 5.8f, 15.8f } },                     // 左翼
    { kPoly,   2, { 15.5f, 11.5f, 18.2f, 15.8f } },                   // 右翼
    { kPoly,   2, { 10.3f, 16.8f, 12.0f, 20.3f } },                   // 尾焰 L
    { kPoly,   2, { 13.7f, 16.8f, 12.0f, 20.3f } },                   // 尾焰 R
};
// sys.hotfix 已安装补丁：包裹箱
static const El kSysHotfix[] = {
    { kRect, 0, { 4.0f, 7.0f, 16.0f, 13.5f, 2 } },
    { kPoly, 4, { 9.5f, 7.0f, 9.5f, 4.5f, 14.5f, 4.5f, 14.5f, 7.0f } }, // 提手
    { kPoly, 2, { 12.0f, 7.0f, 12.0f, 12.0f } },                      // 封箱带竖
    { kPoly, 2, { 4.5f, 12.0f, 19.5f, 12.0f } },                      // 封箱带横
};
// sys.env 环境变量：终端
static const El kSysEnv[] = {
    { kRect, 0, { 3.0f, 4.5f, 18.0f, 15.0f, 2 } },
    { kPoly, 3, { 7.0f, 9.0f, 10.0f, 12.0f, 7.0f, 15.0f } },          // 提示符 >
    { kPoly, 2, { 12.0f, 15.0f, 17.0f, 15.0f } },                     // 光标行
};

// ---- 系统维护族 ------------------------------------------------------------
// sys.hosts hosts 文件：文档 + 铅笔
static const El kSysHosts[] = {
    { kRect, 0, { 4.5f, 3.5f, 11.5f, 17.0f, 2 } },
    { kPoly, 2, { 7.5f, 8.0f, 13.0f, 8.0f } },
    { kPoly, 2, { 7.5f, 11.5f, 13.0f, 11.5f } },
    { kPoly, 2, { 7.5f, 15.0f, 10.5f, 15.0f } },
    { kPoly, 2, { 14.8f, 18.2f, 20.8f, 12.2f } },                     // 笔杆上沿
    { kPoly, 2, { 16.6f, 20.0f, 22.6f, 14.0f } },                     // 笔杆下沿
    { kPolyFill, 3, { 12.4f, 20.6f, 14.8f, 18.2f, 16.6f, 20.0f } },   // 笔尖
};

// ---- 软件管家族 ------------------------------------------------------------
// soft.rec 装机必备·推荐总榜：奖章绶带
static const El kSoftRec[] = {
    { kCircle, 0, { 12.0f, 9.0f, 5.0f } },
    { kPoly,   5, { 9.2f, 13.0f, 7.5f, 20.5f, 12.0f, 17.8f, 16.5f, 20.5f, 14.8f, 13.0f } },
};
// soft.rec.browser 浏览器上网：地球
static const El kSoftBrowser[] = {
    { kCircle, 0, { 12.0f, 12.0f, 8.0f } },
    { kPoly,   2, { 4.0f, 12.0f, 20.0f, 12.0f } },
    { kPoly,   2, { 12.0f, 4.0f, 12.0f, 20.0f } },
};
// soft.rec.file 压缩与文件管理：压缩包
static const El kSoftFile[] = {
    { kRect, 0, { 6.0f, 3.0f, 12.0f, 18.0f, 2 } },
    { kPoly, 2, { 12.0f, 5.5f, 12.0f, 6.5f } },
    { kPoly, 2, { 12.0f, 8.5f, 12.0f, 9.5f } },
    { kPoly, 2, { 12.0f, 11.5f, 12.0f, 12.5f } },
    { kDot,  0, { 12.0f, 14.5f, 1.3f } },
    { kPoly, 2, { 12.0f, 16.0f, 12.0f, 17.5f } },
};
// soft.rec.office 办公与效率：公文包
static const El kSoftOffice[] = {
    { kRect, 0, { 3.5f, 8.0f, 17.0f, 11.5f, 2 } },
    { kPoly, 4, { 9.0f, 8.0f, 9.0f, 5.0f, 15.0f, 5.0f, 15.0f, 8.0f } },
    { kPoly, 2, { 12.0f, 12.5f, 12.0f, 14.5f } },
};
// soft.rec.dev 开发与运维：代码括号
static const El kSoftDev[] = {
    { kPoly, 3, { 9.0f, 7.5f, 4.5f, 12.0f, 9.0f, 16.5f } },
    { kPoly, 3, { 15.0f, 7.5f, 19.5f, 12.0f, 15.0f, 16.5f } },
    { kPoly, 2, { 13.0f, 5.5f, 11.0f, 18.5f } },
};
// soft.rec.media 影音与图像：播放键
static const El kSoftMedia[] = {
    { kCircle,   0, { 12.0f, 12.0f, 8.5f } },
    { kPolyFill, 3, { 10.0f, 8.5f, 10.0f, 15.5f, 15.8f, 12.0f } },
};
// soft.rec.chat 通讯与社交：气泡 + 三点
static const El kSoftChat[] = {
    { kRect, 0, { 3.5f, 4.5f, 17.0f, 11.5f, 4 } },
    { kPoly, 3, { 9.2f, 16.2f, 8.0f, 20.2f, 14.0f, 16.3f } },
    { kDot,  0, { 8.5f, 10.2f, 1.1f } },
    { kDot,  0, { 12.0f, 10.2f, 1.1f } },
    { kDot,  0, { 15.5f, 10.2f, 1.1f } },
};
// soft.rec.sys 系统与安全：盾牌
static const El kSoftSysshield[] = {
    { kPoly, 7, { 12.0f, 3.0f, 19.5f, 5.8f, 19.5f, 11.5f, 12.0f, 21.0f, 4.5f, 11.5f, 4.5f, 5.8f, 12.0f, 3.0f } },
};
// soft.search 软件搜索：窗格 + 放大镜
static const El kSoftSearch[] = {
    { kRect,   0, { 3.5f, 4.0f, 11.0f, 11.0f, 2 } },
    { kPoly,   2, { 6.5f, 7.5f, 11.5f, 7.5f } },
    { kPoly,   2, { 6.5f, 10.5f, 10.0f, 10.5f } },
    { kCircle, 0, { 15.5f, 15.5f, 4.5f } },
    { kPoly,   2, { 18.7f, 18.7f, 21.3f, 21.3f } },
};
// soft.installed 已安装软件：列表 + 勾
static const El kSoftInstalled[] = {
    { kRect, 0, { 4.0f, 4.0f, 16.0f, 16.0f, 2 } },
    { kPoly, 2, { 8.0f, 9.0f, 16.0f, 9.0f } },
    { kPoly, 3, { 8.0f, 14.5f, 10.5f, 17.0f, 16.0f, 11.0f } },
};
// soft.upgrade 软件升级：箱 + 上行箭头
static const El kSoftUpgrade[] = {
    { kRect, 0, { 4.5f, 13.5f, 15.0f, 7.5f, 2 } },
    { kPoly, 2, { 12.0f, 13.5f, 12.0f, 4.0f } },
    { kPoly, 3, { 8.5f, 7.5f, 12.0f, 4.0f, 15.5f, 7.5f } },
};

// ---- 安全体检族 ------------------------------------------------------------
// sec.scan 一键体检：盾牌 + 勾
static const El kSecScan[] = {
    { kPoly, 7, { 12.0f, 3.0f, 19.5f, 5.8f, 19.5f, 11.5f, 12.0f, 21.0f, 4.5f, 11.5f, 4.5f, 5.8f, 12.0f, 3.0f } },
    { kPoly, 3, { 8.7f, 11.5f, 11.0f, 13.8f, 15.5f, 8.8f } },
};
// sec.repair 系统修复：扳手
static const El kSecRepair[] = {
    { kArc,  0, { 9.0f, 9.0f, 4.6f, 4.6f, 95.0f, 260.0f } },          // 开口朝右下
    { kPoly, 2, { 12.3f, 12.3f, 20.0f, 20.0f } },                     // 手柄
};

// ---- 常用管理族 ------------------------------------------------------------
// sys.restore 系统还原点：时光倒流 + 指针
static const El kSysRestore[] = {
    { kArc,      0, { 12.0f, 12.0f, 7.5f, 7.5f, 315.0f, 330.0f } },
    { kPolyFill, 3, { 16.8f, 5.5f, 13.5f, 6.3f, 14.3f, 3.2f } },
    { kPoly,     2, { 12.0f, 12.0f, 12.0f, 8.2f } },
    { kPoly,     2, { 12.0f, 12.0f, 14.8f, 13.6f } },
};
// sys.users 本地用户：双人
static const El kSysUsers[] = {
    { kCircle, 0, { 8.8f, 8.5f, 2.9f } },
    { kArc,    0, { 8.8f, 17.3f, 4.3f, 3.4f, 180.0f, 180.0f } },
    { kCircle, 0, { 16.2f, 8.5f, 2.9f } },
    { kArc,    0, { 16.2f, 17.3f, 4.3f, 3.4f, 180.0f, 180.0f } },
};
// sys.shares 共享目录：文件夹 + 上行箭头
static const El kSysShares[] = {
    { kPolyFill, 6, { 3.5f, 18.5f, 3.5f, 6.5f, 9.2f, 6.5f, 11.2f, 9.0f, 20.5f, 9.0f, 20.5f, 18.5f } },
    { kPoly, 2, { 12.0f, 16.0f, 12.0f, 11.5f } },
    { kPoly, 3, { 9.8f, 13.5f, 12.0f, 11.3f, 14.2f, 13.5f } },
};
// sys.events 事件日志分析：日志 + 旗标
static const El kSysEvents[] = {
    { kRect, 0, { 4.0f, 3.5f, 11.0f, 17.0f, 2 } },
    { kPoly, 2, { 7.0f, 8.0f, 12.0f, 8.0f } },
    { kPoly, 2, { 7.0f, 11.5f, 12.0f, 11.5f } },
    { kPoly, 2, { 7.0f, 15.0f, 10.0f, 15.0f } },
    { kPoly, 2, { 17.5f, 4.0f, 17.5f, 20.0f } },                      // 旗杆
    { kPolyFill, 3, { 17.5f, 4.5f, 21.5f, 6.5f, 17.5f, 8.5f } },      // 旗面
};
// sys.power 电源计划：闪电
static const El kSysPower[] = {
    { kPolyFill, 6, { 13.5f, 3.0f, 6.5f, 13.5f, 10.8f, 13.5f, 10.0f, 21.0f, 17.5f, 10.2f, 13.0f, 10.2f } },
};
// sys.update 缺失更新检查：下行箭头 + 托盘
static const El kSysUpdate[] = {
    { kPoly, 2, { 12.0f, 3.5f, 12.0f, 13.0f } },
    { kPoly, 3, { 8.0f, 9.5f, 12.0f, 13.5f, 16.0f, 9.5f } },
    { kPoly, 4, { 4.0f, 15.5f, 4.0f, 19.5f, 20.0f, 19.5f, 20.0f, 15.5f } },
};

// ---- 高级工具族 ------------------------------------------------------------
// tool.move 软件搬家：方块 + 双向箭头
static const El kToolMove[] = {
    { kRect, 0, { 8.5f, 8.5f, 7.0f, 7.0f, 1.5f } },
    { kPoly, 2, { 8.5f, 12.0f, 3.5f, 12.0f } },
    { kPoly, 3, { 6.0f, 9.8f, 3.5f, 12.0f, 6.0f, 14.2f } },
    { kPoly, 2, { 15.5f, 12.0f, 20.5f, 12.0f } },
    { kPoly, 3, { 18.0f, 9.8f, 20.5f, 12.0f, 18.0f, 14.2f } },
};
// tool.usermigrate 用户目录迁移：人 + 箭头
static const El kToolUsermigrate[] = {
    { kCircle, 0, { 8.0f, 8.5f, 3.0f } },
    { kArc,    0, { 8.0f, 18.0f, 4.2f, 3.4f, 180.0f, 180.0f } },
    { kPoly,   2, { 13.5f, 13.0f, 20.5f, 13.0f } },
    { kPoly,   3, { 18.0f, 10.5f, 20.5f, 13.0f, 18.0f, 15.5f } },
};

// ---- 系统优化族 ------------------------------------------------------------
// tweak.winopt Windows 优化：调节滑杆
static const El kTweakWinopt[] = {
    { kPoly, 2, { 4.0f, 7.0f, 20.0f, 7.0f } },
    { kPoly, 2, { 4.0f, 12.0f, 20.0f, 12.0f } },
    { kPoly, 2, { 4.0f, 17.0f, 20.0f, 17.0f } },
    { kDot,  0, { 9.0f, 7.0f, 2.0f } },
    { kDot,  0, { 15.5f, 12.0f, 2.0f } },
    { kDot,  0, { 7.0f, 17.0f, 2.0f } },
};

// ---- 净化中心族 ------------------------------------------------------------
// purg.wulock 系统更新锁定：锁 + 禁用斜杠
static const El kPurgWulock[] = {
    { kArc,  0, { 12.0f, 10.5f, 4.0f, 4.0f, 180.0f, 180.0f } },       // 锁梁
    { kRect, 0, { 6.0f, 10.5f, 12.0f, 9.5f, 2 } },                    // 锁体
    { kPoly, 2, { 4.5f, 4.5f, 19.5f, 19.5f } },                       // 斜杠
};
// purg.rules 规则库生成：文档 + 勾
static const El kPurgRules[] = {
    { kRect, 0, { 4.0f, 3.5f, 12.5f, 17.0f, 2 } },
    { kPoly, 2, { 7.0f, 8.5f, 13.5f, 8.5f } },
    { kPoly, 2, { 7.0f, 12.0f, 13.5f, 12.0f } },
    { kPoly, 3, { 14.0f, 16.5f, 16.5f, 19.0f, 21.0f, 12.5f } },
};
// purg.yara YARA 扫描：准星
static const El kPurgYara[] = {
    { kCircle, 0, { 12.0f, 12.0f, 7.0f } },
    { kPoly,   2, { 12.0f, 2.5f, 12.0f, 7.0f } },
    { kPoly,   2, { 12.0f, 17.0f, 12.0f, 21.5f } },
    { kPoly,   2, { 2.5f, 12.0f, 7.0f, 12.0f } },
    { kPoly,   2, { 17.0f, 12.0f, 21.5f, 12.0f } },
    { kDot,    0, { 12.0f, 12.0f, 1.4f } },
};
// purg.homepage 主页锁定：房屋
static const El kPurgHomepage[] = {
    { kPoly, 3, { 3.5f, 11.5f, 12.0f, 4.0f, 20.5f, 11.5f } },         // 屋顶
    { kPoly, 4, { 6.0f, 10.5f, 6.0f, 20.0f, 18.0f, 20.0f, 18.0f, 10.5f } }, // 墙体
    { kPoly, 4, { 10.5f, 20.0f, 10.5f, 15.5f, 13.5f, 15.5f, 13.5f, 20.0f } }, // 门
};
// purg.scan 全盘查杀：虫 + 触角腿
static const El kPurgScan[] = {
    { kCircle, 0, { 12.0f, 13.2f, 4.4f } },
    { kPoly,   2, { 10.0f, 9.4f, 8.3f, 6.5f } },                      // 触角 L
    { kPoly,   2, { 14.0f, 9.4f, 15.7f, 6.5f } },                     // 触角 R
    { kPoly,   2, { 12.0f, 8.8f, 12.0f, 17.6f } },                    // 背线
    { kPoly,   2, { 7.6f, 11.0f, 4.4f, 9.6f } },                      // 腿 1L
    { kPoly,   2, { 7.4f, 13.4f, 4.0f, 13.4f } },                     // 腿 2L
    { kPoly,   2, { 7.6f, 15.8f, 4.4f, 17.2f } },                     // 腿 3L
    { kPoly,   2, { 16.4f, 11.0f, 19.6f, 9.6f } },                    // 腿 1R
    { kPoly,   2, { 16.6f, 13.4f, 20.0f, 13.4f } },                   // 腿 2R
    { kPoly,   2, { 16.4f, 15.8f, 19.6f, 17.2f } },                   // 腿 3R
};

// ---- 密码工具族 ------------------------------------------------------------
// pwd.reveal 星号密码查看器：眼睛
static const El kPwdReveal[] = {
    { kArc,    0, { 12.0f, 12.0f, 9.5f, 6.0f, 180.0f, 180.0f } },     // 上睑
    { kArc,    0, { 12.0f, 12.0f, 9.5f, 6.0f, 0.0f, 180.0f } },       // 下睑
    { kCircle, 0, { 12.0f, 12.0f, 2.8f } },                           // 虹膜
    { kDot,    0, { 12.0f, 12.0f, 1.2f } },                           // 瞳孔
};
// pwd.zip ZIP 密码恢复：挂锁 + 钥匙孔
static const El kPwdZip[] = {
    { kArc,  0, { 12.0f, 10.0f, 4.2f, 4.2f, 180.0f, 180.0f } },
    { kRect, 0, { 6.0f, 10.0f, 12.0f, 10.0f, 2 } },
    { kDot,  0, { 12.0f, 14.2f, 1.3f } },
    { kPoly, 2, { 12.0f, 14.2f, 12.0f, 16.8f } },
};
// pwd.cred 凭据管理器：证件卡
static const El kPwdCred[] = {
    { kRect,   0, { 3.0f, 5.5f, 18.0f, 13.5f, 2 } },
    { kCircle, 0, { 8.3f, 10.3f, 1.9f } },
    { kArc,    0, { 8.3f, 15.8f, 2.8f, 2.1f, 180.0f, 180.0f } },
    { kPoly,   2, { 14.0f, 10.0f, 18.5f, 10.0f } },
    { kPoly,   2, { 14.0f, 13.5f, 18.5f, 13.5f } },
};
// pwd.wifi WiFi 密码查看：电波 + 钥匙
static const El kPwdWifi[] = {
    { kArc,  0, { 12.0f, 13.0f, 7.5f, 7.5f, 225.0f, 90.0f } },
    { kArc,  0, { 12.0f, 13.0f, 4.6f, 4.6f, 225.0f, 90.0f } },
    { kDot,  0, { 12.0f, 16.5f, 1.5f } },
    { kPoly, 2, { 12.0f, 16.5f, 12.0f, 20.8f } },                     // 钥柄
    { kPoly, 2, { 12.0f, 18.6f, 14.2f, 18.6f } },                     // 钥齿
};

// ---------------------------------------------------------------- 索引表
enum {
    IC_GENERIC = 0,
    IC_CLEAN_BASIC, IC_CLEAN_SYSTEM, IC_CLEAN_WECHAT, IC_CLEAN_QQ, IC_CLEAN_APPCACHE,
    IC_SCAN_LARGE, IC_SCAN_DUP, IC_SCAN_SIMILAR, IC_SCAN_SIMILARIMG, IC_SCAN_DIRS, IC_SCAN_APPS,
    IC_NET_SHAREMAP, IC_NET_SHAREHOST, IC_NET_DNSOPT, IC_NET_ADAPTERS, IC_NET_CONN,
    IC_NET_ROUTE, IC_NET_ARP, IC_NET_FLUSHDNS, IC_NET_WIFI, IC_NET_MESH,
    IC_NET_SESSIONS, IC_NET_TUNNEL, IC_NET_SERIAL,
    IC_NET_PORTSCAN, IC_NET_LANSCAN, IC_NET_DIAG,
    IC_SYS_TASKS, IC_SYS_PROCESS, IC_SYS_SERVICES, IC_SYS_ACCT, IC_SYS_DOMAIN,
    IC_SYS_INFO, IC_SYS_DISK, IC_SYS_STARTUP, IC_SYS_HOTFIX, IC_SYS_ENV,
    IC_SYS_HOSTS,
    IC_SOFT_REC, IC_SOFT_BROWSER, IC_SOFT_FILE, IC_SOFT_OFFICE, IC_SOFT_DEV,
    IC_SOFT_MEDIA, IC_SOFT_CHAT, IC_SOFT_SYSSHIELD, IC_SOFT_SEARCH, IC_SOFT_INSTALLED,
    IC_SOFT_UPGRADE,
    IC_SEC_SCAN, IC_SEC_REPAIR,
    IC_SYS_RESTORE, IC_SYS_USERS, IC_SYS_SHARES, IC_SYS_EVENTS, IC_SYS_POWER,
    IC_SYS_UPDATE,
    IC_TOOL_MOVE, IC_TOOL_USERMIGRATE,
    IC_TWEAK_WINOPT,
    IC_PURG_WULOCK, IC_PURG_RULES, IC_PURG_YARA, IC_PURG_HOMEPAGE, IC_PURG_SCAN,
    IC_PWD_REVEAL, IC_PWD_ZIP, IC_PWD_CRED, IC_PWD_WIFI,
    IC_COUNT
};

static const IconDef kIcons[IC_COUNT] = {
    { kGenericEls, 2 },
    { kCleanBasic, 5 }, { kCleanSystem, 10 }, { kCleanWechat, 4 }, { kCleanQq, 4 },
    { kCleanAppcache, 3 },
    { kScanLarge, 5 }, { kScanDup, 2 }, { kScanSimilar, 3 }, { kScanSimilarimg, 5 },
    { kScanDirs, 4 }, { kScanApps, 4 },
    { kNetSharemap, 4 }, { kNetSharehost, 6 }, { kNetDnsopt, 5 }, { kNetAdapters, 5 },
    { kNetConn, 3 }, { kNetRoute, 3 }, { kNetArp, 6 }, { kNetFlushdns, 2 },
    { kNetWifi, 3 }, { kNetMesh, 8 },
    { kNetSessions, 7 }, { kNetTunnel, 5 }, { kNetSerial, 4 },
    { kNetPortscan, 5 }, { kNetLanscan, 7 }, { kNetDiag, 2 },
    { kSysTasks, 3 }, { kSysProcess, 10 }, { kSysServices, 12 }, { kSysAcct, 4 },
    { kSysDomain, 8 },
    { kSysInfo, 5 }, { kSysDisk, 3 }, { kSysStartup, 6 }, { kSysHotfix, 4 },
    { kSysEnv, 3 },
    { kSysHosts, 7 },
    { kSoftRec, 2 }, { kSoftBrowser, 3 }, { kSoftFile, 6 }, { kSoftOffice, 3 },
    { kSoftDev, 3 }, { kSoftMedia, 2 }, { kSoftChat, 5 }, { kSoftSysshield, 1 },
    { kSoftSearch, 5 }, { kSoftInstalled, 3 }, { kSoftUpgrade, 3 },
    { kSecScan, 2 }, { kSecRepair, 2 },
    { kSysRestore, 4 }, { kSysUsers, 4 }, { kSysShares, 3 }, { kSysEvents, 6 },
    { kSysPower, 1 }, { kSysUpdate, 3 },
    { kToolMove, 5 }, { kToolUsermigrate, 4 },
    { kTweakWinopt, 6 },
    { kPurgWulock, 3 }, { kPurgRules, 4 }, { kPurgYara, 6 }, { kPurgHomepage, 3 },
    { kPurgScan, 10 },
    { kPwdReveal, 4 }, { kPwdZip, 4 }, { kPwdCred, 5 }, { kPwdWifi, 5 },
};

// 模块 ID → 图标
struct MapEnt { const wchar_t* mod; uint16_t ic; };
static const MapEnt kModIcon[] = {
    // 磁盘清理
    { L"clean.basic", IC_CLEAN_BASIC }, { L"clean.system", IC_CLEAN_SYSTEM },
    { L"clean.wechat", IC_CLEAN_WECHAT }, { L"clean.qq", IC_CLEAN_QQ },
    { L"clean.appcache", IC_CLEAN_APPCACHE },
    // 空间分析
    { L"scan.large", IC_SCAN_LARGE }, { L"scan.dup", IC_SCAN_DUP },
    { L"scan.similar", IC_SCAN_SIMILAR }, { L"scan.similarimg", IC_SCAN_SIMILARIMG },
    { L"scan.dirs", IC_SCAN_DIRS }, { L"scan.apps", IC_SCAN_APPS },
    // 网络管理
    { L"net.sharemap", IC_NET_SHAREMAP }, { L"net.sharehost", IC_NET_SHAREHOST },
    { L"net.dnsopt", IC_NET_DNSOPT }, { L"net.adapters", IC_NET_ADAPTERS },
    { L"net.conn", IC_NET_CONN }, { L"net.route", IC_NET_ROUTE },
    { L"net.arp", IC_NET_ARP }, { L"net.flushdns", IC_NET_FLUSHDNS },
    { L"net.wifi", IC_NET_WIFI }, { L"net.mesh", IC_NET_MESH },
    // 远程终端
    { L"net.sessions", IC_NET_SESSIONS }, { L"net.tunnel", IC_NET_TUNNEL },
    { L"net.serial", IC_NET_SERIAL },
    // 网络诊断
    { L"net.portscan", IC_NET_PORTSCAN }, { L"net.lanscan", IC_NET_LANSCAN },
    { L"net.diag", IC_NET_DIAG },
    // 系统管理
    { L"sys.tasks", IC_SYS_TASKS }, { L"sys.process", IC_SYS_PROCESS },
    { L"sys.services", IC_SYS_SERVICES }, { L"sys.acct", IC_SYS_ACCT },
    { L"sys.domain", IC_SYS_DOMAIN },
    // 系统信息
    { L"sys.info", IC_SYS_INFO }, { L"sys.disk", IC_SYS_DISK },
    { L"sys.startup", IC_SYS_STARTUP }, { L"sys.hotfix", IC_SYS_HOTFIX },
    { L"sys.env", IC_SYS_ENV },
    // 系统维护
    { L"sys.hosts", IC_SYS_HOSTS },
    // 软件管家
    { L"soft.rec", IC_SOFT_REC }, { L"soft.rec.browser", IC_SOFT_BROWSER },
    { L"soft.rec.file", IC_SOFT_FILE }, { L"soft.rec.office", IC_SOFT_OFFICE },
    { L"soft.rec.dev", IC_SOFT_DEV }, { L"soft.rec.media", IC_SOFT_MEDIA },
    { L"soft.rec.chat", IC_SOFT_CHAT }, { L"soft.rec.sys", IC_SOFT_SYSSHIELD },
    { L"soft.search", IC_SOFT_SEARCH }, { L"soft.installed", IC_SOFT_INSTALLED },
    { L"soft.upgrade", IC_SOFT_UPGRADE },
    // 安全体检
    { L"sec.scan", IC_SEC_SCAN }, { L"sec.repair", IC_SEC_REPAIR },
    // 常用管理
    { L"sys.restore", IC_SYS_RESTORE }, { L"sys.users", IC_SYS_USERS },
    { L"sys.shares", IC_SYS_SHARES }, { L"sys.events", IC_SYS_EVENTS },
    { L"sys.power", IC_SYS_POWER }, { L"sys.update", IC_SYS_UPDATE },
    // 高级工具
    { L"tool.move", IC_TOOL_MOVE }, { L"tool.usermigrate", IC_TOOL_USERMIGRATE },
    // 系统优化
    { L"tweak.winopt", IC_TWEAK_WINOPT },
    // 净化中心
    { L"purg.wulock", IC_PURG_WULOCK }, { L"purg.rules", IC_PURG_RULES },
    { L"purg.yara", IC_PURG_YARA }, { L"purg.homepage", IC_PURG_HOMEPAGE },
    { L"purg.scan", IC_PURG_SCAN },
    // 密码工具
    { L"pwd.reveal", IC_PWD_REVEAL }, { L"pwd.zip", IC_PWD_ZIP },
    { L"pwd.cred", IC_PWD_CRED }, { L"pwd.wifi", IC_PWD_WIFI },
};

// ---------------------------------------------------------------- 渲染
void AddRoundRect(Gdiplus::GraphicsPath& p, float x, float y, float w, float h,
                  float r) {
    if (r * 2 > w) r = w / 2;
    if (r * 2 > h) r = h / 2;
    if (r < 0.5f) {
        p.AddRectangle(Gdiplus::RectF(x, y, w, h));
        return;
    }
    float x2 = x + w, y2 = y + h;
    p.AddArc(x, y, r * 2, r * 2, 180, 90);
    p.AddArc(x2 - r * 2, y, r * 2, r * 2, 270, 90);
    p.AddArc(x2 - r * 2, y2 - r * 2, r * 2, r * 2, 0, 90);
    p.AddArc(x, y2 - r * 2, r * 2, r * 2, 90, 90);
    p.CloseFigure();
}

} // namespace

// ---------------------------------------------------------------- 接口
int For(const wchar_t* moduleId) {
    if (moduleId) {
        for (const auto& e : kModIcon)
            if (wcscmp(moduleId, e.mod) == 0) return (int)e.ic;
    }
    return IC_GENERIC;
}

void Draw(HDC dc, int iconId, const RECT& box, COLORREF color) {
    if (iconId < 0 || iconId >= IC_COUNT) iconId = IC_GENERIC;
    int w = box.right - box.left, h = box.bottom - box.top;
    if (w <= 2 || h <= 2) return;
    EnsureGdiplus();
    if (!GdiplusReady()) {
        // GDI+ 不可用：退化成圆角方块轮廓（保底可辨识）
        RECT fb = { box.left + 4, box.top + 4, box.right - 4, box.bottom - 4 };
        mz::ui::StrokeRoundRect(dc, fb, 3, color, 1);
        return;
    }

    const IconDef& def = kIcons[iconId];
    float side = (float)std::min(w, h);
    float scale = side / 24.0f;
    float ox = box.left + (w - side) / 2.0f;
    float oy = box.top + (h - side) / 2.0f;
    auto GX = [&](float gx) { return ox + gx * scale; };
    auto GY = [&](float gy) { return oy + gy * scale; };

    Gdiplus::Graphics g(dc);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    Gdiplus::Color col(255, GetRValue(color), GetGValue(color), GetBValue(color));
    Gdiplus::Pen pen(col, std::max(1.0f, 1.9f * scale));
    pen.SetStartCap(Gdiplus::LineCapRound);
    pen.SetEndCap(Gdiplus::LineCapRound);
    pen.SetLineJoin(Gdiplus::LineJoinRound);
    Gdiplus::SolidBrush brush(col);

    for (int i = 0; i < def.n; ++i) {
        const El& e = def.els[i];
        switch (e.kind) {
        case kPoly: {
            if (e.n < 2) break;
            Gdiplus::GraphicsPath p;
            for (int k = 0; k + 1 < e.n; ++k)
                p.AddLine(GX(e.a[k * 2]), GY(e.a[k * 2 + 1]),
                          GX(e.a[k * 2 + 2]), GY(e.a[k * 2 + 3]));
            g.DrawPath(&pen, &p);
            break;
        }
        case kPolyFill: {
            if (e.n < 3) break;
            Gdiplus::GraphicsPath p;
            Gdiplus::PointF pts[7];
            for (int k = 0; k < e.n && k < 7; ++k)
                pts[k] = Gdiplus::PointF(GX(e.a[k * 2]), GY(e.a[k * 2 + 1]));
            p.AddPolygon(pts, std::min<int>(e.n, 7));
            g.FillPath(&brush, &p);
            g.DrawPath(&pen, &p);
            break;
        }
        case kCircle:
            g.DrawEllipse(&pen, GX(e.a[0] - e.a[2]), GY(e.a[1] - e.a[2]),
                          e.a[2] * 2 * scale, e.a[2] * 2 * scale);
            break;
        case kDot:
            g.FillEllipse(&brush, GX(e.a[0] - e.a[2]), GY(e.a[1] - e.a[2]),
                          e.a[2] * 2 * scale, e.a[2] * 2 * scale);
            break;
        case kRect: {
            Gdiplus::GraphicsPath p;
            AddRoundRect(p, GX(e.a[0]), GY(e.a[1]),
                         e.a[2] * scale, e.a[3] * scale, e.a[4] * scale);
            g.DrawPath(&pen, &p);
            break;
        }
        case kArc: {
            Gdiplus::RectF rc(GX(e.a[0] - e.a[2]), GY(e.a[1] - e.a[3]),
                              e.a[2] * 2 * scale, e.a[3] * 2 * scale);
            g.DrawArc(&pen, rc, e.a[4], e.a[5]);
            break;
        }
        }
    }
}

} // namespace icon
} // namespace ui
} // namespace mz
