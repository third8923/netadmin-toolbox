#include "regengine.h"
#include <sstream>
#include <algorithm>

namespace mz {

// ---------------------------------------------------------------- 编码工具

static wstring DecodeRegBytes(const vector<unsigned char>& raw) {
    if (raw.size() >= 2 && raw[0] == 0xFF && raw[1] == 0xFE) {
        // UTF-16 LE
        const wchar_t* p = reinterpret_cast<const wchar_t*>(raw.data() + 2);
        size_t chars = (raw.size() - 2) / 2;
        // 去掉尾部 \0
        while (chars > 0 && p[chars - 1] == 0) --chars;
        return wstring(p, chars);
    }
    if (raw.size() >= 3 && raw[0] == 0xEF && raw[1] == 0xBB && raw[2] == 0xBF) {
        return Utf8ToW(std::string((const char*)raw.data() + 3, raw.size() - 3));
    }
    return Utf8ToW(std::string((const char*)raw.data(), raw.size()));
}

bool SplitRegPath(const wstring& full, HKEY* root, wstring* subkey) {
    struct { const wchar_t* name; HKEY h; } roots[] = {
        { L"HKEY_CURRENT_USER",  HKEY_CURRENT_USER },
        { L"HKEY_LOCAL_MACHINE", HKEY_LOCAL_MACHINE },
        { L"HKEY_CLASSES_ROOT",  HKEY_CLASSES_ROOT },
        { L"HKEY_USERS",         HKEY_USERS },
        { L"HKEY_CURRENT_CONFIG",HKEY_CURRENT_CONFIG },
        { L"HKCU",               HKEY_CURRENT_USER },
        { L"HKLM",               HKEY_LOCAL_MACHINE },
        { L"HKCR",               HKEY_CLASSES_ROOT },
        { L"HKU",                HKEY_USERS },
    };
    for (auto& r : roots) {
        size_t n = wcslen(r.name);
        if (_wcsnicmp(full.c_str(), r.name, n) == 0) {
            *root = r.h;
            wstring rest = full.substr(n);
            if (!rest.empty() && rest[0] == L'\\') rest = rest.substr(1);
            *subkey = rest;
            return true;
        }
    }
    return false;
}

// 递归创建键
// 注意：不能直接用 KEY_ALL_ACCESS ——某些受 ACL 保护的键会返回 ACCESS_DENIED。
// 依次降级尝试，取第一个可用的权限组合。
static HKEY CreateKeyPath(HKEY root, const wstring& sub, bool* created) {
    if (sub.empty()) return root;
    static const REGSAM kAccess[] = {
        KEY_SET_VALUE | KEY_CREATE_SUB_KEY | KEY_QUERY_VALUE,
        KEY_WRITE | KEY_READ,
        KEY_ALL_ACCESS,
    };
    for (REGSAM access : kAccess) {
        HKEY h = nullptr;
        DWORD disp = 0;
        LONG r = RegCreateKeyExW(root, sub.c_str(), 0, nullptr, 0,
                                 access, nullptr, &h, &disp);
        if (r == ERROR_SUCCESS && h) {
            if (created) *created = (disp == REG_CREATED_NEW_KEY);
            return h;
        }
        if (h) { RegCloseKey(h); h = nullptr; }
    }
    return nullptr;
}

// ---------------------------------------------------------------- 值解析

static vector<BYTE> ParseHexBytes(const wstring& s, size_t start) {
    vector<BYTE> out;
    wstring hex;
    for (size_t i = start; i < s.size(); ++i) {
        wchar_t c = s[i];
        if (iswxdigit(c)) hex.push_back(c);
    }
    if (hex.size() % 2) hex.push_back(L'0');
    for (size_t i = 0; i + 1 < hex.size(); i += 2) {
        wchar_t buf[3] = { hex[i], hex[i + 1], 0 };
        out.push_back((BYTE)wcstoul(buf, nullptr, 16));
    }
    return out;
}

// 解析带转义的字符串 "abc\"def"
static wstring ParseRegString(const wstring& s, size_t start, size_t* endPos) {
    wstring out;
    size_t i = start + 1; // 跳过开引号
    while (i < s.size()) {
        wchar_t c = s[i];
        if (c == L'\\' && i + 1 < s.size()) {
            wchar_t n = s[i + 1];
            switch (n) {
                case L'\\': out.push_back(L'\\'); break;
                case L'"':  out.push_back(L'"');  break;
                case L'r':  out.push_back(L'\r'); break;
                case L'n':  out.push_back(L'\n'); break;
                case L'0':  out.push_back(L'\0'); break;
                default:    out.push_back(n);     break;
            }
            i += 2;
        } else if (c == L'"') {
            *endPos = i + 1;
            return out;
        } else {
            out.push_back(c);
            ++i;
        }
    }
    *endPos = i;
    return out;
}

static bool ApplySingleValue(HKEY hKey, const wstring& name, bool del,
                             const wstring& typeAndData, RegPatchStats* st,
                             const LogSink& log) {
    const wchar_t* vn = name.empty() ? nullptr : name.c_str();

    if (del) {
        LONG r = RegDeleteValueW(hKey, vn);
        if (r == ERROR_SUCCESS || r == ERROR_FILE_NOT_FOUND) {
            if (r == ERROR_SUCCESS) st->valuesDeleted++;
            return true;
        }
        st->errors++;
        st->errorDetail.push_back(L"删除值失败: " + name);
        return false;
    }

    wstring data = typeAndData;
    while (!data.empty() && (data.back() == L'\r' || data.back() == L' ')) data.pop_back();
    while (!data.empty() && data.front() == L' ') data.erase(0, 1);

    // dword:xxxxxxxx
    if (_wcsnicmp(data.c_str(), L"dword:", 6) == 0) {
        DWORD v = (DWORD)wcstoul(data.c_str() + 6, nullptr, 16);
        if (RegSetValueExW(hKey, vn, 0, REG_DWORD, (const BYTE*)&v, sizeof(v)) == ERROR_SUCCESS) {
            st->valuesSet++; return true;
        }
    }
    // qword:
    else if (_wcsnicmp(data.c_str(), L"qword:", 6) == 0) {
        unsigned long long v = _wcstoui64(data.c_str() + 6, nullptr, 16);
        if (RegSetValueExW(hKey, vn, 0, REG_QWORD, (const BYTE*)&v, sizeof(v)) == ERROR_SUCCESS) {
            st->valuesSet++; return true;
        }
    }
    // hex / hex(2) / hex(7) 等
    else if (_wcsnicmp(data.c_str(), L"hex", 3) == 0) {
        DWORD type = REG_BINARY;
        size_t start = 3;
        if (data.size() > 3 && data[3] == L'(') {
            size_t close = data.find(L')');
            if (close != wstring::npos) {
                type = (DWORD)wcstoul(data.substr(4, close - 4).c_str(), nullptr, 16);
                start = close + 1;
            }
        }
        if (start < data.size() && data[start] == L':') ++start;
        vector<BYTE> bytes = ParseHexBytes(data, start);
        if (RegSetValueExW(hKey, vn, 0, type, bytes.data(), (DWORD)bytes.size()) == ERROR_SUCCESS) {
            st->valuesSet++; return true;
        }
    }
    // 字符串
    else if (!data.empty() && data[0] == L'"') {
        size_t e = 0;
        wstring sv = ParseRegString(data, 0, &e);
        if (RegSetValueExW(hKey, vn, 0, REG_SZ, (const BYTE*)sv.c_str(),
                           (DWORD)((sv.size() + 1) * sizeof(wchar_t))) == ERROR_SUCCESS) {
            st->valuesSet++; return true;
        }
    }
    st->errors++;
    st->errorDetail.push_back(L"无法解析的行: " + data.substr(0, 60));
    return false;
}

// ---------------------------------------------------------------- 主入口

// 按 \n 切行（不用 wistringstream：MinGW 宽字符流在默认 locale 下不可靠）
static vector<wstring> SplitLinesW(const wstring& s) {
    vector<wstring> out;
    size_t start = 0;
    while (start <= s.size()) {
        size_t pos = s.find(L'\n', start);
        wstring line = (pos == wstring::npos) ? s.substr(start) : s.substr(start, pos - start);
        if (!line.empty() && line.back() == L'\r') line.pop_back();
        out.push_back(line);
        if (pos == wstring::npos) break;
        start = pos + 1;
    }
    return out;
}

RegPatchStats ApplyRegContent(const vector<unsigned char>& content, const LogSink& log) {
    RegPatchStats st;
    wstring text = DecodeRegBytes(content);

    // 清理 BOM 残留与特殊字符
    if (!text.empty() && text[0] == 0xFEFF) text.erase(0, 1);
    while (!text.empty() && text.back() == 0) text.pop_back();

    // 去掉孤立的 \0（某些 reg 文件带）
    text.erase(std::remove(text.begin(), text.end(), L'\0'), text.end());

    HKEY curRoot = nullptr;
    wstring curSub;
    bool haveKey = false;
    bool pendingDeleteKey = false;

    for (wstring& raw : SplitLinesW(text)) {
        wstring line = raw;
        // 去首尾空白
        size_t b = line.find_first_not_of(L" \t");
        if (b == wstring::npos) continue;
        size_t e = line.find_last_not_of(L" \t");
        line = line.substr(b, e - b + 1);

        if (line.empty() || line[0] == L';') continue;
        if (_wcsnicmp(line.c_str(), L"Windows Registry Editor", 23) == 0) continue;
        if (_wcsnicmp(line.c_str(), L"REGEDIT4", 8) == 0) continue;
        if (_wcsnicmp(line.c_str(), L"REGEDIT5", 8) == 0) continue;

        // [键路径]
        if (line[0] == L'[') {
            bool del = false;
            size_t s = 1;
            if (line.size() > 2 && line[1] == L'-') { del = true; s = 2; }
            size_t close = line.rfind(L']');
            if (close == wstring::npos) continue;
            wstring path = line.substr(s, close - s);
            // 去引号
            if (!path.empty() && path.front() == L'"' && path.back() == L'"')
                path = path.substr(1, path.size() - 2);

            if (!SplitRegPath(path, &curRoot, &curSub)) {
                st.errors++;
                st.errorDetail.push_back(L"不支持的根键: " + path);
                haveKey = false;
                continue;
            }
            haveKey = true;
            pendingDeleteKey = del;

            if (del) {
                LONG r = RegDeleteTreeW(curRoot, curSub.c_str());
                if (r == ERROR_SUCCESS || r == ERROR_FILE_NOT_FOUND) {
                    st.keysDeleted++;
                    haveKey = false;
                } else {
                    st.errors++;
                    st.errorDetail.push_back(L"删除键失败: " + path);
                }
            } else {
                bool created = false;
                HKEY h = CreateKeyPath(curRoot, curSub, &created);
                if (h) {
                    if (created) st.keysCreated++;
                    RegCloseKey(h);
                } else {
                    st.errors++;
                    st.errorDetail.push_back(L"创建键失败: " + path);
                    haveKey = false;
                }
            }
            continue;
        }

        if (!haveKey) continue;

        // 值行: @=... 或 "name"=...
        wstring name;
        bool del = false;
        wstring data;
        size_t eq;

        if (line[0] == L'@') {
            eq = line.find(L'=');
            if (eq == wstring::npos) continue;
            // 检查键删除标记
            data = line.substr(eq + 1);
        } else if (line[0] == L'"') {
            size_t nameEnd = 0;
            name = ParseRegString(line, 0, &nameEnd);
            eq = line.find(L'=', nameEnd);
            if (eq == wstring::npos) continue;
            data = line.substr(eq + 1);
        } else {
            continue;
        }

        if (!data.empty() && data[0] == L'-' && (data.size() == 1 || data[1] != L' ')) {
            del = true;
            data = L"";
        }

        HKEY hCreate = CreateKeyPath(curRoot, curSub, nullptr);
        if (!hCreate) {
            st.errors++;
            continue;
        }
        ApplySingleValue(hCreate, name, del, data, &st, log);
        RegCloseKey(hCreate);
    }

    return st;
}

// ---------------------------------------------------------------- 便捷读写

bool ReadRegString(HKEY root, const wstring& subkey, const wstring& name, wstring* out) {
    HKEY h = nullptr;
    if (RegOpenKeyExW(root, subkey.c_str(), 0, KEY_READ, &h) != ERROR_SUCCESS) return false;
    DWORD type = 0, size = 0;
    bool ok = false;
    if (RegQueryValueExW(h, name.c_str(), nullptr, &type, nullptr, &size) == ERROR_SUCCESS &&
        (type == REG_SZ || type == REG_EXPAND_SZ) && size >= sizeof(wchar_t)) {
        vector<wchar_t> buf(size / sizeof(wchar_t) + 1, 0);
        if (RegQueryValueExW(h, name.c_str(), nullptr, &type, (BYTE*)buf.data(), &size) == ERROR_SUCCESS) {
            if (out) out->assign(buf.data());
            ok = true;
        }
    }
    RegCloseKey(h);
    return ok;
}

bool ReadRegDword(HKEY root, const wstring& subkey, const wstring& name, DWORD* out) {
    HKEY h = nullptr;
    if (RegOpenKeyExW(root, subkey.c_str(), 0, KEY_READ, &h) != ERROR_SUCCESS) return false;
    DWORD type = 0, size = sizeof(DWORD), val = 0;
    bool ok = false;
    if (RegQueryValueExW(h, name.c_str(), nullptr, &type, (BYTE*)&val, &size) == ERROR_SUCCESS &&
        type == REG_DWORD) {
        if (out) *out = val;
        ok = true;
    }
    RegCloseKey(h);
    return ok;
}

bool WriteRegDword(HKEY root, const wstring& subkey, const wstring& name, DWORD value) {
    HKEY h = nullptr;
    if (RegCreateKeyExW(root, subkey.c_str(), 0, nullptr, 0, KEY_ALL_ACCESS,
                        nullptr, &h, nullptr) != ERROR_SUCCESS) {
        return false;
    }
    LONG r = RegSetValueExW(h, name.c_str(), 0, REG_DWORD, (const BYTE*)&value, sizeof(value));
    RegCloseKey(h);
    return r == ERROR_SUCCESS;
}

bool ReadRegQword(HKEY root, const wstring& subkey, const wstring& name, unsigned long long* out) {
    HKEY h = nullptr;
    if (RegOpenKeyExW(root, subkey.c_str(), 0, KEY_READ, &h) != ERROR_SUCCESS) return false;
    DWORD type = 0, size = sizeof(unsigned long long), val = 0;
    bool ok = false;
    if (RegQueryValueExW(h, name.c_str(), nullptr, &type, (BYTE*)&val, &size) == ERROR_SUCCESS &&
        type == REG_QWORD) {
        if (out) *out = val;
        ok = true;
    }
    RegCloseKey(h);
    return ok;
}

bool ReadRegBinary(HKEY root, const wstring& subkey, const wstring& name, vector<BYTE>* out) {
    HKEY h = nullptr;
    if (RegOpenKeyExW(root, subkey.c_str(), 0, KEY_READ, &h) != ERROR_SUCCESS) return false;
    DWORD type = 0, size = 0;
    bool ok = false;
    if (RegQueryValueExW(h, name.c_str(), nullptr, &type, nullptr, &size) == ERROR_SUCCESS &&
        size > 0) {
        vector<BYTE> buf(size);
        if (RegQueryValueExW(h, name.c_str(), nullptr, &type, buf.data(), &size) == ERROR_SUCCESS) {
            buf.resize(size);
            if (out) out->swap(buf);
            ok = true;
        }
    }
    RegCloseKey(h);
    return ok;
}

} // namespace mz
