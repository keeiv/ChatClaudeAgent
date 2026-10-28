#include "hardware.hpp"

#include <windows.h>
#include <dxgi.h>

#include <cmath>

#include "util.hpp"

#pragma comment(lib, "dxgi.lib")

namespace {

double gb(unsigned long long bytes) { return std::round(bytes / (1024.0 * 1024.0 * 1024.0) * 10.0) / 10.0; }

std::string cpuName() {
    wchar_t buf[256];
    DWORD size = sizeof buf;
    if (RegGetValueW(HKEY_LOCAL_MACHINE, L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", L"ProcessorNameString",
                     RRF_RT_REG_SZ, nullptr, buf, &size) == ERROR_SUCCESS) {
        return util::trim(util::narrow(buf));
    }
    return "";
}

struct Candidate {
    const char* model;
    const char* label;
    double sizeGb;  // download / memory size at the default quantization
    const char* why;
};

// Checked against the Ollama library on 2026-10-23. Sizes are the default tags.
const Candidate kCandidates[] = {
    {"qwen3-coder:30b", "Qwen3-Coder 30B", 19.0, "目前本機最強的程式模型（MoE，只啟用 3.3B 參數，速度快）"},
    {"devstral:24b", "Devstral 24B", 14.0, "專為程式代理訓練，SWE-Bench 表現好"},
    {"gpt-oss:20b", "gpt-oss 20B", 14.0, "推理能力強的通用模型"},
    {"qwen3:14b", "Qwen3 14B", 9.3, "思考能力好，審查程式很穩"},
    {"qwen2.5-coder:14b", "Qwen2.5-Coder 14B", 9.0, "程式專用，讀 diff 很準"},
    {"qwen3:8b", "Qwen3 8B", 5.2, "速度和能力平衡"},
    {"qwen2.5-coder:7b", "Qwen2.5-Coder 7B", 4.7, "程式專用的小模型"},
    {"qwen3:4b", "Qwen3 4B", 2.5, "很省資源，適合筆電或沒有獨立顯卡"},
    {"qwen2.5-coder:3b", "Qwen2.5-Coder 3B", 1.9, "最小的程式模型"},
    {"qwen3:1.7b", "Qwen3 1.7B", 1.4, "只有 CPU 時也跑得動"},
};

}  // namespace

Json detectHardware() {
    MEMORYSTATUSEX mem{sizeof mem};
    GlobalMemoryStatusEx(&mem);
    Json hw = J({{"cpu", cpuName()}, {"ramGb", gb(mem.ullTotalPhys)}, {"freeRamGb", gb(mem.ullAvailPhys)}, {"gpus", Json::array()}});
    double best = 0;
    IDXGIFactory1* factory = nullptr;
    if (SUCCEEDED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)&factory))) {
        IDXGIAdapter1* adapter = nullptr;
        for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
            DXGI_ADAPTER_DESC1 d{};
            adapter->GetDesc1(&d);
            adapter->Release();
            if (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;
            double vram = gb(d.DedicatedVideoMemory);
            // Integrated GPUs report a small carve-out and share system RAM.
            bool integrated = vram < 2.0;
            hw["gpus"].push(J({{"name", util::narrow(d.Description)}, {"vramGb", vram}, {"integrated", integrated},
                               {"vendorId", (long long)d.VendorId}}));
            if (!integrated && vram > best) best = vram;
        }
        factory->Release();
    }
    hw["bestVramGb"] = best;
    return hw;
}

Json recommendModels(const Json& hw) {
    double vram = hw["bestVramGb"].num();
    double ram = hw["ramGb"].num();
    Json out = Json::array();
    bool picked = false;
    for (const auto& c : kCandidates) {
        // Leave room for the context window (about 1.5 GB) on the GPU.
        std::string fit;
        if (vram > 0 && c.sizeGb + 1.5 <= vram) fit = "gpu";
        else if (vram > 0 && c.sizeGb <= vram + ram * 0.35) fit = "tight";  // part of it spills to system RAM
        else if (vram == 0 && c.sizeGb + 2 <= ram * 0.7) fit = "cpu";  // leave room for Windows and the apps
        else continue;
        Json item = J({{"model", c.model}, {"label", c.label}, {"sizeGb", c.sizeGb}, {"why", c.why}, {"fit", fit}});
        // The first model that fits entirely is the recommendation; both reviewers can share it.
        if (!picked && (fit == "gpu" || (vram == 0 && fit == "cpu"))) {
            item["recommended"] = true;
            picked = true;
        }
        out.push(item);
    }
    if (!picked && out.size() > 0) out.at(out.size() - 1)["recommended"] = true;
    return out;
}
