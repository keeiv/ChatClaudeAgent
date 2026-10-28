// Provider/MCP management and verified task images. All settings changes stay on the UI thread.
#include "app.hpp"

#include <wincodec.h>
#include <windows.h>
#include <wrl/client.h>

#include <algorithm>
#include <cctype>

#include "local_tools.hpp"
#include "pipeline.hpp"
#include "util.hpp"

namespace {
bool safeId(const std::string& id) {
    if (id.empty() || id.size() > 80) return false;
    for (unsigned char c : id)
        if (!isalnum(c) && c != '-' && c != '_') return false;
    return true;
}
// A pin is the worker's SHA-256 of a tool's name, description, input schema and annotations.
bool validPin(const Json& value) {
    if (!value.isString() || value.str().size() != 64) return false;
    for (unsigned char c : value.str())
        if (!isxdigit(c) || isupper(c)) return false;
    return true;
}
// Pins survive a re-save only when the program and its arguments are unchanged and the tool is still allowed:
// a different executable is a different thing to trust, so everything is approved again.
Json carriedPins(const Json& old, const Json& server) {
    Json pins = Json::object();
    if (old["command"].str() != server["command"].str() || old["args"].dump() != server["args"].dump()) return pins;
    for (const auto& tool : server["allowedTools"].items())
        if (tool.isString() && validPin(old["pins"][tool.str()])) pins[tool.str()] = old["pins"][tool.str()];
    return pins;
}
// How each allowed tool compares with what the server advertises right now.
Json pinStates(const Json& server, const Json& hashes) {
    Json states = Json::object();
    for (const auto& tool : server["allowedTools"].items()) {
        const std::string name = tool.str();
        const std::string pin = server["pins"][name].str();
        if (!hashes.has(name)) states[name] = "missing";
        else if (pin.empty()) states[name] = "unpinned";
        else states[name] = pin == hashes[name].str() ? "ok" : "changed";
    }
    return states;
}
bool validPng(const std::string& bytes) {
    using Microsoft::WRL::ComPtr;
    HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    bool ok = false;
    {
        ComPtr<IWICImagingFactory> factory;
        ComPtr<IWICStream> stream;
        ComPtr<IWICBitmapDecoder> decoder;
        ComPtr<IWICBitmapFrameDecode> frame;
        UINT w = 0, h = 0;
        GUID format{};
        if (SUCCEEDED(
                CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))) &&
            SUCCEEDED(factory->CreateStream(&stream)) &&
            SUCCEEDED(stream->InitializeFromMemory((BYTE*)bytes.data(), (DWORD)bytes.size())) &&
            SUCCEEDED(
                factory->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnLoad, &decoder)) &&
            SUCCEEDED(decoder->GetContainerFormat(&format)) && IsEqualGUID(format, GUID_ContainerFormatPng) &&
            SUCCEEDED(decoder->GetFrame(0, &frame)) && SUCCEEDED(frame->GetSize(&w, &h)) && w && h &&
            (unsigned long long)w * h <= 40000000) {
            // Force decoding a row, not only accepting a filename or a PNG signature.
            ComPtr<IWICFormatConverter> converter;
            if (SUCCEEDED(factory->CreateFormatConverter(&converter)) &&
                SUCCEEDED(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone,
                                                nullptr, 0, WICBitmapPaletteTypeCustom))) {
                std::vector<BYTE> pixels((size_t)w * h * 4);
                ok = SUCCEEDED(converter->CopyPixels(nullptr, w * 4, (UINT)pixels.size(), pixels.data()));
            }
        }
    }
    if (SUCCEEDED(init)) CoUninitialize();
    return ok;
}
}  // namespace

void App::sendPlatform() {
    send(J({{"type", "platform"},
            {"providers", settings_["providers"]},
            {"servers", settings_["toolServers"]},
            {"sandbox", settings_["sandboxStatus"]}}));
}

bool App::handlePlatform(const Json& m) {
    std::string type = m["type"].str();
    if (type == "task.pause") {
        Conversation* c = find(m["id"].str());
        if (!c || !c->group.pipeline || !c->group.active || c->group.paused) return true;
        c->task["pause_requested"] = true;
        if (!c->preparing.empty()) return true;
        Json node = pipelineNode(*c);
        AgentKind kind = node["backend"].str() == "claude" ? AgentKind::Claude : AgentKind::Codex;
        if (c->gemini && c->gemini->busy()) {
            c->gemini->shutdown();
            onAgentEvent(c->id, kind, {AgentEvent::Kind::TurnDone, "", J({{"status", "interrupted"}})});
        } else if (c->worker && c->worker->busy()) {
            c->worker->shutdown();
            if (c->activeMsg[kind == AgentKind::Claude ? 0 : 1].empty()) {
                c->task["pause_requested"] = false;
                pipelineBlock(*c, "使用者已暫停，代理尚未開始回覆");
            } else onAgentEvent(c->id, kind, {AgentEvent::Kind::TurnDone, "", J({{"status", "interrupted"}})});
        } else if (kind == AgentKind::Claude && c->claude && c->claude->busy()) c->claude->interrupt();
        else if (kind == AgentKind::Codex && c->codex && c->codex->busy()) c->codex->interrupt();
        else {
            c->task["pause_requested"] = false;
            pipelineBlock(*c, "使用者已暫停");
        }
        return true;
    }
    if (type == "task.rereview") {
        Conversation* c = find(m["id"].str());
        if (!c || !c->task["pipeline"]["nodes"].size() || anyBusy(*c) || (c->group.active && !c->group.paused)) {
            toast("error", "請先暫停目前代理，再重新審查");
            return true;
        }
        c->workspaceLease.reset();
        c->group = GroupRun();
        c->task["cursor"] = 1;
        c->task["cycle"] = 1;
        c->task["passes"] = Json::object();
        c->task["guard_attempts"] = Json::object();
        c->task["pause_requested"] = false;
        pipelineStart(*c, "", true);
        return true;
    }
    if (type == "platform.get") {
        sendPlatform();
        return true;
    }
    bool mutation = type == "provider.save" || type == "provider.remove" || type == "toolserver.save" ||
                    type == "toolserver.remove" || type == "toolserver.pin";
    if (mutation) {
        for (const auto& c : convs_)
            if (c->group.pipeline && c->group.active) {
                toast("error", "請先結束多代理任務，再更改供應商或 MCP 權限");
                return true;
            }
        for (const auto& control : controls_)
            if (control->busy()) {
                toast("info", "請等待目前的管理操作完成");
                return true;
            }
    }
    if (type == "pipeline.set") {
        Conversation* c = find(m["id"].str());
        std::string error;
        if (!c || c->kind != "group") return true;
        if (c->group.active || anyBusy(*c)) {
            toast("error", "請先結束目前任務再更改流程");
            return true;
        }
        if (!m["pipeline"]["nodes"].size()) c->pipeline = Json::object();
        else if (!validatePipeline(m["pipeline"], &error)) {
            toast("error", error);
            return true;
        } else c->pipeline = m["pipeline"];
        metaDirty_ = true;
        saveMeta();
        sendConvs();
        return true;
    }
    if (type == "task.history") {
        Conversation* c = find(m["id"].str());
        if (c)
            send(J({{"type", "task.history"},
                    {"id", c->id},
                    {"items", taskHistory(c->task["task_id"].str(), m["after"].i64())}}));
        return true;
    }
    if (type == "artifact.add") {
        Conversation* c = find(m["id"].str());
        if (c) importArtifact(*c, m);
        return true;
    }
    if (type == "provider.remove") {
        if (!safeId(m["id"].str())) return true;
        std::wstring file = util::joinPath(util::joinPath(util::appDataDir(), L"provider-credentials"),
                                           util::widen(m["id"].str() + ".bin"));
        if (!DeleteFileW(file.c_str()) && GetLastError() != ERROR_FILE_NOT_FOUND &&
            GetLastError() != ERROR_PATH_NOT_FOUND) {
            toast("error", "無法移除加密金鑰，供應商設定仍保留");
            return true;
        }
        settings_["providers"].erase(m["id"].str());
        metaDirty_ = true;
        saveMeta();
        sendPlatform();
        return true;
    }
    if (type == "toolserver.save") {
        Json server = m["server"];
        if (!safeId(server["id"].str()) || server["command"].str().empty() || !server["args"].isArray() ||
            !server["allowedTools"].isArray() || !m["confirm"].boolean()) {
            toast("error", "請填寫 MCP 伺服器設定並確認來源與權限");
            return true;
        }
        if (server["args"].size() > 100 || server["allowedTools"].size() > 300) {
            toast("error", "MCP 設定超過上限");
            return true;
        }
        for (const auto& arg : server["args"].items())
            if (!arg.isString() || arg.str().size() > 4096) return true;
        server = J({{"id", server["id"]},
                    {"name", server["name"]},
                    {"command", server["command"]},
                    {"args", server["args"]},
                    {"allowedTools", server["allowedTools"]},
                    {"enabled", server["enabled"].boolean()}});
        server["approved"] = true;
        Json& servers = settings_["toolServers"];
        if (!servers.isArray()) servers = Json::array();
        server["pins"] = Json::object();
        bool replaced = false;
        for (auto& current : servers.items())
            if (current["id"].str() == server["id"].str()) {
                server["pins"] = carriedPins(current, server);
                current = server;
                replaced = true;
            }
        if (!replaced) servers.push(server);
        metaDirty_ = true;
        saveMeta();
        sendPlatform();
        return true;
    }
    if (type == "toolserver.remove") {
        auto& items = settings_["toolServers"].items();
        items.erase(
            std::remove_if(items.begin(), items.end(), [&](const Json& s) { return s["id"].str() == m["id"].str(); }),
            items.end());
        metaDirty_ = true;
        saveMeta();
        sendPlatform();
        return true;
    }
    if (type != "provider.save" && type != "provider.test" && type != "provider.models" && type != "sandbox.status" &&
        type != "toolserver.test" && type != "toolserver.pin")
        return false;
    Json provider = m["provider"];
    Json request = J({{"action", type == "provider.save"                                 ? "key.save"
                                 : type == "toolserver.test" || type == "toolserver.pin" ? "mcp.test"
                                                                                         : type},
                      {"data_dir", util::narrow(util::appDataDir())}});
    if (type == "provider.save") {
        if (!safeId(provider["id"].str()) ||
            (provider["protocol"].str() != "responses" && provider["protocol"].str() != "chat")) {
            toast("error", "供應商識別碼或協定無效");
            return true;
        }
        if (provider["inputRate"].num() < 0 || provider["outputRate"].num() < 0 ||
            provider["inputRate"].num() > 1000000 || provider["outputRate"].num() > 1000000) {
            toast("error", "Token 費率超出可用範圍");
            return true;
        }
        // An explicit allowlist prevents credentials or arbitrary fields reaching settings or the UI.
        provider = J({{"id", provider["id"]},
                      {"name", provider["name"]},
                      {"url", provider["url"]},
                      {"protocol", provider["protocol"]},
                      {"inputRate", provider["inputRate"].num()},
                      {"outputRate", provider["outputRate"].num()},
                      {"billing", "api"}});
        request["key"] = m["apiKey"];
    } else if (type == "provider.test" || type == "provider.models") {
        const Json& providers = settings_["providers"];
        provider = providers[m["id"].str()];
    }
    request["provider"] = provider;
    Json serverSnapshot;
    if (type == "toolserver.test" || type == "toolserver.pin")
        for (const auto& s : settings_["toolServers"].items())
            if (s["id"].str() == m["id"].str()) request["server"] = serverSnapshot = s;
    // The pins the user chose are checked against a fresh listing, so a server that changed after the user looked
    // at its tools cannot get the new description pinned.
    Json wanted = Json::object();
    if (type == "toolserver.pin") {
        if (!serverSnapshot.isObject() || !m["pins"].isObject()) {
            toast("error", "找不到這個 MCP 伺服器，或沒有選擇要釘住的工具");
            return true;
        }
        for (const auto& [name, pin] : m["pins"].fields()) {
            bool allowed = false;
            for (const auto& tool : serverSnapshot["allowedTools"].items()) allowed = allowed || tool.str() == name;
            if (!allowed || !validPin(pin)) {
                toast("error", "只能釘住已列入核准清單的工具");
                return true;
            }
            wanted[name] = pin;
        }
    }
    std::string requestId = m["id"].str(provider["id"].str());
    std::string requestToken = m["requestToken"].str();
    std::weak_ptr<int> life = lifetime_;
    auto worker = std::make_shared<RuntimeSession>(
        AgentEventFn{},
        [this, life, type, provider, requestId, requestToken, serverSnapshot, wanted](const Json& workerResult) {
            if (life.expired()) return;
            Json result = workerResult;
            if ((type == "toolserver.test" || type == "toolserver.pin") && result["ok"].boolean()) {
                for (auto& current : settings_["toolServers"].items()) {
                    if (current["id"].str() != serverSnapshot["id"].str()) continue;
                    // The server may have been edited while the worker ran; only an unchanged one takes pins.
                    bool same = current["command"].str() == serverSnapshot["command"].str() &&
                                current["args"].dump() == serverSnapshot["args"].dump();
                    if (type == "toolserver.pin" && same) {
                        Json pins = Json::object(), rejected = Json::array();
                        for (const auto& [name, pin] : wanted.fields()) {
                            if (result["hashes"][name].str() == pin.str()) pins[name] = pin;
                            else rejected.push(name);
                        }
                        current["pins"] = pins;
                        result["rejected"] = rejected;
                        metaDirty_ = true;
                        saveMeta();
                    }
                    result["pinState"] = pinStates(current, result["hashes"]);
                }
            }
            if (result["ok"].boolean() && type == "provider.save") {
                settings_["providers"][provider["id"].str()] = provider;
                metaDirty_ = true;
                saveMeta();
            }
            if (type == "sandbox.status") settings_["sandboxStatus"] = result;
            const Json& providers = settings_["providers"];
            if ((type == "provider.test" || type == "provider.models") && result["ok"].boolean() &&
                providers[requestId].isObject() && providers[requestId]["url"].str() == provider["url"].str() &&
                providers[requestId]["protocol"].str() == provider["protocol"].str()) {
                settings_["providers"][provider["id"].str()]["models"] = result["models"];
                metaDirty_ = true;
            }
            send(J({{"type", "platform.result"},
                    {"action", type},
                    {"id", requestId},
                    {"requestToken", requestToken},
                    {"result", result}}));
            sendPlatform();
        });
    controls_.erase(std::remove_if(controls_.begin(), controls_.end(), [](const auto& p) { return !p->busy(); }),
                    controls_.end());
    controls_.push_back(worker);
    std::string error;
    if (!worker->start(request, &error))
        send(J({{"type", "platform.result"},
                {"action", type},
                {"id", requestId},
                {"requestToken", requestToken},
                {"result", J({{"ok", false}, {"error", error}})}}));
    return true;
}

void App::importArtifact(Conversation& c, const Json& artifact) {
    std::wstring full;
    std::string error, data;
    if (!resolveInside(c.cwd, artifact["path"].str(), &full, &error)) {
        toast("error", "截圖路徑不在專案內");
        return;
    }
    WIN32_FILE_ATTRIBUTE_DATA info{};
    if (!GetFileAttributesExW(full.c_str(), GetFileExInfoStandard, &info) || info.nFileSizeHigh ||
        info.nFileSizeLow > 10 * 1024 * 1024 || !util::readFile(full, data) || !validPng(data)) {
        toast("error", "測試截圖必須是有效的 PNG，且不超過 10 MiB／4,000 萬像素");
        return;
    }
    for (const auto& previous : c.task["artifacts"].items()) {
        std::string bytes;
        if (util::readFile(util::joinPath(mediaDir(), util::widen(previous["file"].str())), bytes) && bytes == data)
            return;
    }
    std::string file = "artifact-" + util::newId() + ".png";
    if (!util::writeFileAtomic(util::joinPath(mediaDir(), util::widen(file)), data)) {
        toast("error", "無法保存測試圖片");
        return;
    }
    Json record = J({{"id", util::newId()},
                     {"file", file},
                     {"task_id", c.task["task_id"]},
                     {"ts", util::nowMs()},
                     {"source", artifact["source"].str("project")},
                     {"path", artifact["path"]},
                     {"operation", artifact["operation"]},
                     {"result", artifact["result"].str("unknown")},
                     {"error", artifact["error"]}});
    if (!c.task["artifacts"].isArray()) c.task["artifacts"] = Json::array();
    c.task["artifacts"].push(record);
    saveTask(c.task);
    Json image = J({{"type", "image"}, {"artifact", record}});
    Json* current = nullptr;
    for (const auto& id : c.activeMsg)
        if (!id.empty()) {
            current = findMsg(c, id);
            if (current) break;
        }
    if (current) {
        (*current)["blocks"].push(image);
        markDirty(c, (*current)["id"].str());
    } else
        pushMessage(c, J({{"author", "system"},
                          {"kind", "artifact"},
                          {"text", "測試圖片：" + artifact["operation"].str()},
                          {"blocks", Json(Json::Array{image})}}));
    c.stateDirty = true;
}
