#include "module.h"
#include <algorithm>

namespace mz {

ModuleRegistry& ModuleRegistry::Instance() {
    static ModuleRegistry inst;
    return inst;
}

void ModuleRegistry::Register(IModule* m) {
    if (!m) return;
    for (auto* e : modules_) {
        if (e == m) return;
        if (e->Id() && m->Id() && wcscmp(e->Id(), m->Id()) == 0) return;  // 去重
    }
    modules_.push_back(m);
}

IModule* ModuleRegistry::Find(const wstring& id) const {
    for (auto* m : modules_) {
        if (m->Id() && _wcsicmp(m->Id(), id.c_str()) == 0) return m;
    }
    return nullptr;
}

vector<ModuleRegistry::GroupEntry> ModuleRegistry::Grouped() const {
    vector<GroupEntry> out;
    for (auto* m : modules_) {
        wstring g = m->Group() ? m->Group() : L"其他";
        bool found = false;
        for (auto& e : out) {
            if (e.group == g) { e.mods.push_back(m); found = true; break; }
        }
        if (!found) out.push_back({ g, { m } });
    }
    return out;
}

} // namespace mz
