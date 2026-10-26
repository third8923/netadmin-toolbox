// ============================================================================
//  图像感知哈希（dHash 64 位）实现 —— WIC
// ============================================================================
#include "simhash.h"

#include <wincodec.h>

#include <algorithm>
#include <map>

namespace mz {
namespace {

template <class T>
static void SafeRelease(T*& p) {
    if (p) {
        p->Release();
        p = nullptr;
    }
}

// COM 初始化守卫：
//   S_OK / S_FALSE -> 我们成功初始化（S_FALSE 表示此前已初始化），需配对 CoUninitialize
//   RPC_E_CHANGED_MODE -> 当前线程已是 MTA，继续执行但不再 CoUninitialize
class ComGuard {
public:
    ComGuard() {
        HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        own_ = SUCCEEDED(hr);
        ok_  = own_ || hr == RPC_E_CHANGED_MODE;
    }
    ~ComGuard() { if (own_) CoUninitialize(); }
    bool ok() const { return ok_; }
private:
    bool own_ = false;
    bool ok_  = false;
};

// 每张图独立创建 WIC 工厂，避免跨线程共享 COM 对象（工厂创建开销很小）
static IWICImagingFactory* CreateWicFactory() {
    IWICImagingFactory* factory = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_IWICImagingFactory, (void**)&factory);
    if (FAILED(hr) || !factory) return nullptr;
    return factory;
}

} // namespace

bool ImageDHash(const wstring& path, unsigned long long* hash) {
    if (!hash || path.empty()) return false;
    *hash = 0;

    ComGuard com;
    if (!com.ok()) return false;

    IWICImagingFactory* factory = CreateWicFactory();
    if (!factory) return false;

    IWICBitmapDecoder*  decoder  = nullptr;
    IWICBitmapFrameDecode* frame = nullptr;
    IWICBitmapScaler*   scaler   = nullptr;
    IWICFormatConverter* conv    = nullptr;
    bool ok = false;

    do {
        // 1) 解码首帧
        if (FAILED(factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
                                                      WICDecodeMetadataCacheOnDemand, &decoder)))
            break;
        if (FAILED(decoder->GetFrame(0, &frame)))
            break;

        UINT w = 0, h = 0;
        if (FAILED(frame->GetSize(&w, &h)) || w == 0 || h == 0)
            break;

        // 2) 缩放到 9x8
        if (FAILED(factory->CreateBitmapScaler(&scaler)))
            break;
        if (FAILED(scaler->Initialize(frame, 9, 8, WICBitmapInterpolationModeFant)))
            break;

        // 3) 转 8bpp 灰度
        if (FAILED(factory->CreateFormatConverter(&conv)))
            break;
        if (FAILED(conv->Initialize(scaler, GUID_WICPixelFormat8bppGray,
                                    WICBitmapDitherTypeNone, nullptr, 0.0,
                                    WICBitmapPaletteTypeMedianCut)))
            break;

        // 4) 取像素：8 行，每行 9 字节
        constexpr UINT kStride = 9;
        BYTE pixels[9 * 8] = { 0 };
        if (FAILED(conv->CopyPixels(nullptr, kStride, (UINT)sizeof(pixels), pixels)))
            break;

        // 5) 相邻像素比较 -> 64 位
        unsigned long long value = 0;
        for (int y = 0; y < 8; ++y) {
            for (int x = 0; x < 8; ++x) {
                BYTE left  = pixels[y * kStride + x];
                BYTE right = pixels[y * kStride + x + 1];
                if (left > right) value |= (1ULL << (y * 8 + x));
            }
        }
        *hash = value;
        ok = true;
    } while (false);

    SafeRelease(conv);
    SafeRelease(scaler);
    SafeRelease(frame);
    SafeRelease(decoder);
    factory->Release();
    return ok;
}

int HammingDistance(unsigned long long a, unsigned long long b) {
    unsigned long long x = a ^ b;
    int n = 0;
    while (x) {
        x &= (x - 1);
        ++n;
    }
    return n;
}

vector<vector<wstring>> GroupSimilarImages(const vector<wstring>& paths, int threshold,
                                           const ProgressFn& progress, const CancelFn& cancel) {
    vector<vector<wstring>> groups;
    if (paths.empty()) return groups;
    if (threshold < 0) threshold = 0;

    ComGuard com;
    if (!com.ok()) return groups;

    const size_t total = paths.size();
    vector<unsigned long long> hashes(total, 0);
    vector<size_t> valid;                  // 成功算出 dHash 的下标
    valid.reserve(total);

    // 1) 逐张算指纹（失败跳过）
    for (size_t i = 0; i < total; ++i) {
        if (cancel && cancel()) break;
        if (progress) {
            progress((int)(i * 60 / total),
                     L"正在计算图像指纹 " + std::to_wstring(i + 1) + L"/" + std::to_wstring(total));
        }
        unsigned long long hv = 0;
        if (ImageDHash(paths[i], &hv)) {
            hashes[i] = hv;
            valid.push_back(i);
        }
    }

    if (valid.size() < 2) {
        if (progress) progress(100, L"相似图片分析完成");
        return groups;
    }

    // 2) 并查集聚类：距离 <= threshold 视为相似
    vector<int> parent(valid.size());
    for (size_t i = 0; i < parent.size(); ++i) parent[i] = (int)i;
    std::function<int(int)> find = [&](int x) -> int {
        while (parent[x] != x) {
            parent[x] = parent[parent[x]];
            x = parent[x];
        }
        return x;
    };
    auto unite = [&](int a, int b) {
        int ra = find(a), rb = find(b);
        if (ra != rb) parent[rb] = ra;
    };

    for (size_t i = 0; i < valid.size(); ++i) {
        if (cancel && cancel()) break;
        for (size_t j = i + 1; j < valid.size(); ++j) {
            if (cancel && cancel()) break;
            if (HammingDistance(hashes[valid[i]], hashes[valid[j]]) <= threshold) {
                unite((int)i, (int)j);
            }
        }
        if (progress) {
            progress(60 + (int)(i * 35 / valid.size()), L"正在比对图像指纹...");
        }
    }

    // 3) 收集 >= 2 的组
    std::map<int, vector<size_t>> buckets;     // 根 -> valid 下标
    for (size_t i = 0; i < valid.size(); ++i) buckets[find((int)i)].push_back(i);

    struct GroupRef {
        size_t first = 0;
        vector<wstring> items;
    };
    vector<GroupRef> tmp;
    for (auto& kv : buckets) {
        if (kv.second.size() < 2) continue;
        GroupRef g;
        g.first = kv.second.front();
        for (auto idx : kv.second) g.items.push_back(paths[valid[idx]]);
        tmp.push_back(std::move(g));
    }
    std::sort(tmp.begin(), tmp.end(), [](const GroupRef& a, const GroupRef& b) {
        if (a.items.size() != b.items.size()) return a.items.size() > b.items.size();
        return a.first < b.first;
    });
    groups.reserve(tmp.size());
    for (auto& g : tmp) groups.push_back(std::move(g.items));

    if (progress) progress(100, L"相似图片分析完成");
    return groups;
}

} // namespace mz
