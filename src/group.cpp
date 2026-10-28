// Group collaboration: Claude writes, GPT reviews and fixes, Claude re-checks
// GPT's result, and the members keep handing the work around (Gemini joins as a
// third reviewer when invited) until everyone accepts the latest changes. A member
// out of quota is skipped; when nobody has quota left the task stops with a record.
#include "app.hpp"
#include "diff.hpp"
#include "editions.hpp"
#include "humanize.hpp"
#include "takeover.hpp"
#include "util.hpp"

namespace {

const size_t kMaxPromptDiff = 40 * 1024;

AgentKind other(AgentKind k) { return k == AgentKind::Claude ? AgentKind::Codex : AgentKind::Claude; }

AgentKind kindOf(const std::string& name) {
    return name == "codex" ? AgentKind::Codex : name == "gemini" ? AgentKind::Gemini : AgentKind::Claude;
}

const char* phaseLabel(const std::string& phase) {
    return phase == "write" ? "實作" : phase == "recheck" ? "複審" : phase == "selfcheck" ? "自查" : "審查";
}

std::string finalText(const Json& msg) {
    std::string out;
    for (const auto& b : msg["blocks"].items()) {
        if (b["type"].str() != "text") continue;
        if (!out.empty()) out += "\n\n";
        out += b["text"].str();
    }
    return out;
}

}  // namespace

std::vector<AgentKind> App::groupMembers(const Conversation& c) const {
    AgentKind lead = c.lead == "codex" ? AgentKind::Codex : AgentKind::Claude;
    std::vector<AgentKind> members{lead, other(lead)};
    if (c.geminiMember) members.push_back(AgentKind::Gemini);
    return members;
}

bool App::groupCanRun(const Conversation& c, AgentKind k) const {
    if (c.group.skipped.count(agentName(k)) || agentExhausted(k)) return false;
    // A Google account the CLI no longer serves cannot take a turn at all; treat it like an empty quota.
    if (k == AgentKind::Gemini) return geminiStatus_.state != "retired" && geminiStatus_.state != "missing";
    // Claude without a usable subscription is passed over as before; an unavailable GPT still pauses for a retry.
    if (k == AgentKind::Claude) return claudeStatus_.state == "ok";
    return true;
}

namespace {
std::string unavailableReason(bool exhausted, AgentKind k) {
    if (exhausted) return "額度不足";
    return k == AgentKind::Gemini ? "的帳號目前無法使用 Gemini CLI" : "尚未通過訂閱檢查";
}
}  // namespace

std::string App::groupInstructions(const Conversation& c, AgentKind k) const {
    std::string me = nameOf(k), partner = nameOf(other(k));
    std::string role = "你在 CCA 協作群組中，使用者叫 " + userName() + "，你是 " + me + "。\n";
    if (c.group.solo && c.group.current == agentName(k)) {
        return role +
               "其他成員目前額度不足或無法參與，由你接續實作、驗證與自查。先查閱交接和目前檔案，從中斷處繼續。"
               "不得宣稱其他成員已審查；本地模型的建議須自行查證。請簡潔回報完成與尚未完成項目。";
    }
    AgentKind lead = c.lead == "codex" ? AgentKind::Codex : AgentKind::Claude;
    if (k == AgentKind::Gemini)
        role +=
            nameOf(lead) + " 是主要撰寫者，" + nameOf(other(lead)) +
            " 先審查，你是第三位審查者。查證前兩位沒有發現的錯誤、遺漏與安全問題，必要時直接修改，再交給下一位複審。";
    else if (k == lead)
        role += "你是主要撰寫者，負責實作使用者需求；完成後由 " + partner + " 審查並修改，再交給你複審。";
    else role += partner + " 是主要撰寫者，你是審查者。查證錯誤、遺漏與安全問題，必要時直接修改，再交給對方複審。";
    if (c.geminiMember && k != AgentKind::Gemini) role += nameOf(AgentKind::Gemini) + " 也在群組裡，擔任第三位審查者。";
    return role +
           "請遵守使用者對唯讀、禁止修改等限制。沒有實質問題不要為改而改。"
           "依照交接訊息規則在最後一行寫 VERDICT。請用台灣繁體中文，說明簡潔。";
}

std::string App::reviewPrompt(const Conversation& c, AgentKind from, AgentKind to) const {
    const GroupRun& g = c.group;
    std::string p = "【CCA 協作｜第 " + std::to_string(g.round + 1) + " 輪審查】\n\n";
    p += "使用者的需求：\n" + g.task + "\n\n";
    // Earlier rounds as a short structured record, not the whole conversation again.
    if (c.task.isObject() && c.task["handoffs"].size() > 1) {
        Json earlier = c.task;
        earlier["handoffs"].items().pop_back();  // the turn just finished is described below in full
        p += "到目前為止的交接紀錄（以實際檔案與 git 為準，不要只相信摘要）：\n" + taskBrief(earlier, 3000) + "\n";
    }
    p += g.phase == "write" ? nameOf(from) + " 已經完成第一版。"
                            : nameOf(from) + " 審查後回覆了。";
    std::string text = util::trim(g.turnText);
    p += text.empty() ? "它沒有留下說明。\n\n" : "它的說明：\n" + util::truncateUtf8(text, 6000) + "\n\n";

    if (g.turnFiles.size() == 0) {
        p += nameOf(from) + " 這一輪沒有修改任何檔案。\n\n";
    } else {
        p += "這一輪修改的檔案：\n";
        for (const auto& f : g.turnFiles.items()) {
            p += "- " + f["path"].str() + "（+" + std::to_string(f["added"].i64()) + " −" +
                 std::to_string(f["removed"].i64()) + "）\n";
        }
        std::string diffs;
        bool cut = false;
        for (const auto& f : g.turnFiles.items()) {
            std::string one = "--- " + f["path"].str() + "\n" + f["diff"].str();
            if (!one.empty() && one.back() != '\n') one += '\n';
            if (diffs.size() + one.size() > kMaxPromptDiff) {
                cut = true;
                break;
            }
            diffs += one;
        }
        p += "\n變更內容：\n```diff\n" + diffs + "```\n";
        if (cut) p += "（diff 太長，其餘部分請直接開檔案查看。）\n";
        p += "\n";
    }

    auto last = g.lastVerdict.find(agentName(to));
    if (last != g.lastVerdict.end() && last->second == "issues") {
        p += "你上一輪提出了疑慮，以上是 " + nameOf(from) + " 的回應。如果回應合理、疑慮已經解決，就回覆 VERDICT: APPROVE。\n\n";
    }
    p += "請審查專案中目前的程式碼（需要時直接開檔案看完整內容），確認它正確並符合使用者的需求。發現問題就直接修改檔案。\n\n";
    if (humanizeOn()) p += humanizeReviewNote(settings_["humanize"]["extra"].str()) + "\n\n";
    p += "回覆的最後一行必須是以下三者之一：\n";
    p += "VERDICT: APPROVE（你完全同意目前的程式碼，而且這一輪沒有修改任何檔案）\n";
    p += "VERDICT: CHANGED（你修改了檔案，請簡短說明改了什麼、為什麼）\n";
    p += "VERDICT: ISSUES（你有疑慮但沒有修改，請說明需要 " + (c.geminiMember ? "其他成員" : nameOf(other(to))) +
         " 處理什麼）";
    return p + reviewFocus(c, to);
}

void App::groupStart(Conversation& c, const std::string& text) {
    if (c.group.active || anyBusy(c)) {
        toast("error", "協作還在進行中，請等它結束或先停止");
        return;
    }
    if (c.pipeline["nodes"].size()) {
        pipelineStart(c, text);
        return;
    }
    c.workflow = WorkflowRun();
    c.stateDirty = true;
    pushMessage(c, J({{"author", "user"}, {"text", text}}));
    std::string problem;
    if (c.cwd.empty() || !util::dirExists(c.cwd)) problem = "請先選擇這個群組的專案資料夾。";
    else if (c.lead != "codex" && claudeStatus_.state != "ok" && !(c.autoFallback && claudeLimited())) problem = "Claude 尚未通過訂閱檢查。";
    else if (codexStatus_.state != "ok") problem = "GPT 尚未通過訂閱檢查。";
    if (!problem.empty()) {
        pushMessage(c, J({{"author", "system"}, {"kind", "error"}, {"text", problem}}));
        return;
    }
    c.group = GroupRun();
    c.group.active = true;
    c.group.task = text;
    c.group.limit = c.maxRounds;
    c.userStopped = false;
    c.lastRequest = text;
    c.task = newTask(c.id, text, util::nowMs());
    saveTask(c.task);
    c.group.solo = c.lead == "codex" && (claudeLimited() || claudeStatus_.state != "ok") && !c.geminiMember;
    groupHandOff(c, c.lead == "codex" ? AgentKind::Codex : AgentKind::Claude, text, "write");
}

void App::groupSkip(Conversation& c, AgentKind k, const std::string& reason) {
    GroupRun& g = c.group;
    g.skipped.insert(agentName(k));
    g.approvals.erase(agentName(k));
    pushMessage(c, J({{"author", "system"},
                      {"kind", "skip"},
                      {"agent", agentName(k)},
                      {"round", g.round},
                      {"phase", g.phase},
                      {"text", nameOf(k) + " " + reason + "，本輪" + phaseLabel(g.phase) + "已跳過"}}));
    std::vector<AgentKind> left;
    for (AgentKind m : groupMembers(c))
        if (groupCanRun(c, m)) left.push_back(m);
    if (left.empty()) {
        groupQuotaStop(c, nameOf(k) + reason);
        return;
    }
    // The skipped step goes to the next member in rotation order, so review keeps its usual direction.
    const auto members = groupMembers(c);
    auto at = std::find(members.begin(), members.end(), k);
    AgentKind next = left.front();
    for (size_t step = 1; at != members.end() && step <= members.size(); ++step) {
        AgentKind candidate = members[(size_t(at - members.begin()) + step) % members.size()];
        if (std::find(left.begin(), left.end(), candidate) != left.end()) {
            next = candidate;
            break;
        }
    }
    const std::string step = util::truncateUtf8(g.lastPrompt, 12000);
    if (left.size() == 1) {
        g.solo = true;
        std::string phase = g.phase == "write" ? "write" : "selfcheck";
        groupHandOff(c, next, handoverPrompt(c, g.task + "\n\n" + nameOf(k) + " 原本要處理的步驟：\n" + step), phase);
        return;
    }
    if (g.phase == "write") {
        groupHandOff(c, next, handoverPrompt(c, g.task + "\n\n" + nameOf(k) + " 原本要處理的步驟：\n" + step), "write");
        return;
    }
    groupHandOff(c, next, "（原本交給 " + nameOf(k) + "，因" + reason + "改由你處理。）\n\n" + step,
                 agentName(next) == c.lead ? "recheck" : "review");
}

void App::groupQuotaStop(Conversation& c, const std::string& detail) {
    GroupRun& g = c.group;
    const std::string next = g.current.empty() ? c.lead : g.current;
    Json skipped = Json::array();
    for (const auto& s : g.skipped) skipped.push(s);
    Json files = Json::array();
    for (const auto& f : g.allFiles.fields()) files.push(f.first);
    if (c.task.isObject()) {
        // The record names the step that could not run, so 接續任務 starts exactly there once quota returns.
        c.task["quota_stop"] = J({{"at", util::nowMs()},
                                  {"reason", detail},
                                  {"round", g.round},
                                  {"phase", g.phase},
                                  {"next", next + ":" + g.phase},
                                  {"skipped", skipped},
                                  {"files", files}});
        auto& items = c.task["handoffs"].items();
        if (!items.empty()) items.back()["next_action"] = next + ":" + (g.phase.empty() ? "write" : g.phase);
    }
    std::string text = "全部 AI 的額度都已用完（最後一次：" + detail + "），已強制停止。進度：第 " +
                       std::to_string(g.round) + " 輪，" + phaseLabel(g.phase) + "中；累計修改 " +
                       std::to_string(files.size()) + " 個檔案。下一步記錄為 " + nameOf(kindOf(next)) + " 的" +
                       phaseLabel(g.phase) + "，額度恢復後按「接續任務」從這裡繼續。";
    // With automatic resume on, the earliest reported (or estimated) reset decides when the task restarts.
    const long long resume = autoResumeAt(c);
    if (c.task.isObject()) c.task["quota_stop"]["resumeAt"] = resume;
    if (resume) text += "已排定 " + localClock(resume) + " 自動接續，可在任務紀錄取消。";
    groupFinish(c, "quota", text);
}

void App::groupHandOff(Conversation& c, AgentKind next, const std::string& prompt, const std::string& phase) {
    GroupRun& g = c.group;
    if (c.task.isObject()) {
        setNextAction(c.task, std::string(agentName(next)) + ":" + phase);
        c.task["status"] = "running";
        saveTask(c.task);
    }
    g.phase = phase;
    g.current = agentName(next);
    g.lastPrompt = prompt;
    g.turnFiles = Json::array();
    g.turnText.clear();
    g.paused = false;
    g.pauseReason.clear();
    g.pauseKind.clear();
    // Known exhaustion is skipped before spending a turn; a failed login still pauses for the user below.
    if (c.autoFallback && (agentExhausted(next) || (next == AgentKind::Gemini && !groupCanRun(c, next)))) {
        groupSkip(c, next, unavailableReason(agentExhausted(next), next));
        return;
    }
    // Local notes collected since the last handoff travel with this one, and the reply must answer them.
    g.notesFrom = std::move(g.notesNext);
    g.notesNext.clear();
    c.stateDirty = true;
    std::string err;
    if (!sendToAgent(c, next, prompt, &err)) {
        groupPause(c, "無法把工作交給 " + nameOf(next) + "：" + err, next, prompt, phase, g.round);
    }
}

void App::groupTurnDone(Conversation& c, AgentKind k, const Json& msg, const std::string& status) {
    GroupRun& g = c.group;
    if (g.stopRequested || status == "interrupted") {
        groupFinish(c, "stopped", "已停止協作。");
        return;
    }
    if (k == AgentKind::Claude && c.autoFallback && claudeLimited() && !c.geminiMember) {
        std::string err;
        if (!takeOver(c, "Claude 額度已耗盡", &err)) groupPause(c, err, AgentKind::Codex, g.lastPrompt, "write", g.round);
        return;
    }
    // A completed turn still counts even if it used the last of the quota; only a refused turn is skipped.
    if (c.autoFallback && status == "failed" && (quotaFailure(k, msg) || agentExhausted(k))) {
        groupSkip(c, k, "額度不足");
        return;
    }
    if (status == "failed") {
        groupPause(c, nameOf(k) + " 這一輪沒有完成，可以重試或結束協作。", k, g.lastPrompt, g.phase, g.round);
        return;
    }

    // Read the verdict and hide the machine-readable line from the chat.
    std::string text = finalText(msg);
    std::string rest;
    Verdict v = parseVerdict(text, &rest);
    g.turnText = rest;
    if (Json* live = findMsg(c, msg["id"].str())) {
        if (v != Verdict::None) (*live)["verdict"] = verdictName(v);
        Json& blocks = (*live)["blocks"];
        for (auto it = blocks.items().rbegin(); it != blocks.items().rend(); ++it) {
            if ((*it)["type"].str() != "text") continue;
            std::string stripped;
            if (parseVerdict((*it)["text"].str(), &stripped) != Verdict::None) (*it)["text"] = stripped;
            break;
        }
        markDirty(c, msg["id"].str());
    }
    // Every finished turn becomes a structured handoff the next agent (and a restart) can use.
    if (c.task.isObject()) {
        std::string vname = v != Verdict::None ? verdictName(v) : g.phase == "write" ? "write" : "";
        addHandoff(c.task, handoffFromTurn(c.task, msg, agentName(k), g.phase, g.turnFiles, vname, util::nowMs()));
        saveTask(c.task);
    }
    // A reviewer that edited files is tested before the next member sees them (regression_guard.cpp).
    g.turnSnapshot = msg["snapshot"].str();
    if (groupGuardApplies(c, k)) {
        groupGuardStart(c, k, (int)v);
        return;
    }
    groupTurnJudged(c, k, (int)v);
}

void App::groupTurnJudged(Conversation& c, AgentKind k, int verdictValue) {
    GroupRun& g = c.group;
    const Verdict v = static_cast<Verdict>(verdictValue);
    if (g.solo) {
        if (v == Verdict::Issues) {
            groupPause(c, nameOf(k) + " 自查仍有未解決問題，請確認後再繼續。", k, g.lastPrompt, "selfcheck", g.round);
            return;
        }
        if (g.phase != "selfcheck" && g.turnFiles.size() > 0) {
            g.nextAgent = agentName(k);
            g.nextPhase = "selfcheck";
            g.nextPrompt =
                reviewPrompt(c, k, k) + "\n其他成員無法參與：請自行查證修改和測試，修正後完整交付，不要等待其他成員。";
            ++g.round;
            g.reviewedBy = nameOf(k);
            g.localQueue.clear();
            g.localNotes.clear();
            g.notesNext.clear();
            for (const auto& slot : c.localMembers) {
                if (localEnabled(slot == "local2" ? AgentKind::Local2 : AgentKind::Local1)) g.localQueue.push_back(slot);
            }
            groupRunLocals(c);
        } else {
            std::string absent;
            for (AgentKind m : groupMembers(c))
                if (m != k) absent += (absent.empty() ? "" : "、") + nameOf(m);
            groupFinish(c, "completed", nameOf(k) + " 已完成工作與自查；" + absent + " 本輪未參與審查。");
        }
        return;
    }
    bool changed = g.turnFiles.size() > 0;
    const std::string me = agentName(k);
    bool approves = !changed && (v == Verdict::Approve || v == Verdict::None);
    g.lastVerdict[me] = g.phase == "write" ? "write" : changed ? "changed" : approves ? "approve" : "issues";
    if (g.phase == "write" || changed) {
        g.lastChanger = me;
        g.approvals.clear();
    } else if (approves) g.approvals.insert(me);
    else g.approvals.erase(me);

    // The next member in rotation who can still run; members out of quota are announced once and passed over.
    const auto members = groupMembers(c);
    auto at = std::find(members.begin(), members.end(), k);
    std::vector<AgentKind> active;
    bool found = false;
    AgentKind next = k;
    for (size_t step = 1; step < members.size(); ++step) {
        AgentKind candidate = members[(size_t(at - members.begin()) + step) % members.size()];
        if (!groupCanRun(c, candidate)) {
            if (!g.skipped.count(agentName(candidate))) {
                const bool exhausted = agentExhausted(candidate);
                g.skipped.insert(agentName(candidate));
                pushMessage(
                    c,
                    J({{"author", "system"},
                       {"kind", "skip"},
                       {"agent", agentName(candidate)},
                       {"round", g.round},
                       {"text", nameOf(candidate) + " " + unavailableReason(exhausted, candidate) + "，這一輪跳過"}}));
            }
            continue;
        }
        active.push_back(candidate);
        if (!found) next = candidate, found = true;
    }
    if (!found) {
        // Nobody else can review: the remaining member checks its own work once, as in a solo takeover.
        g.solo = true;
        if (g.phase == "selfcheck" || !changed) {
            groupFinish(c, "completed", nameOf(k) + " 已完成工作；其他成員額度不足，本輪未參與審查。");
            return;
        }
        groupHandOff(c, k, reviewPrompt(c, k, k) + "\n其他成員額度不足：請自行驗證後交付。", "selfcheck");
        return;
    }

    if (g.phase != "write" && approves) {
        // Everyone still in the run must accept the latest change, and no objection may be left unanswered.
        bool agreed = true;
        for (AgentKind m : active) {
            const std::string name = agentName(m);
            // Without a known author (an old record), only unanswered objections block agreement.
            if (!g.lastChanger.empty() && name != g.lastChanger && !g.approvals.count(name)) agreed = false;
            if (g.lastVerdict[name] == "issues") agreed = false;
        }
        if (agreed) {
            const std::string detail = v == Verdict::None  ? nameOf(k) + " 沒有提出修改（未附結論，視為同意）。"
                                       : active.size() > 1 ? "所有成員都沒有意見了。"
                                                           : "雙方都沒有意見了。";
            groupFinish(c, "consensus", detail);
            return;
        }
    }

    std::string prompt = reviewPrompt(c, k, next);
    std::string phase = agentName(next) == c.lead ? "recheck" : "review";
    int nextRound = g.round + 1;
    if (nextRound > g.limit) {
        groupPause(c, "已經來回 " + std::to_string(g.round) + " 輪，雙方還沒有達成共識。要再來回 2 輪，還是先結束？",
                   next, prompt, phase, nextRound);
        return;
    }
    // `reason` lets the UI re-word the line with whatever names the user picks later.
    std::string handoff, reason;
    if (g.phase == "write") {
        reason = "first";
        handoff = nameOf(k) + " 把第一版交給 " + nameOf(next) + " 審查";
    } else if (changed) {
        reason = "changed";
        handoff = nameOf(k) + " 修改了 " + std::to_string(g.turnFiles.size()) + " 個檔案，交給 " + nameOf(next) + " 複審";
    } else if (approves) {
        reason = "answered";
        handoff = nameOf(k) + " 回應了 " + nameOf(next) + " 的疑慮（沒有修改），交給 " + nameOf(next) + " 確認";
    } else {
        reason = "issues";
        handoff = nameOf(k) + " 提出了疑慮，交給 " + nameOf(next) + " 處理";
    }
    pushMessage(c, J({{"author", "system"}, {"kind", "handoff"}, {"from", agentName(k)}, {"to", agentName(next)},
                      {"round", nextRound}, {"reason", reason}, {"files", (long long)g.turnFiles.size()},
                      {"text", handoff}}));
    g.round = nextRound;
    // Invited local models review the new changes (read-only) before the next agent starts.
    if (changed && !c.localMembers.empty()) {
        g.nextAgent = agentName(next);
        g.nextPrompt = prompt;
        g.nextPhase = phase;
        g.reviewedBy = nameOf(k);
        g.localNotes.clear();
        g.notesNext.clear();
        g.localQueue.clear();
        for (const auto& l : c.localMembers) {
            if (localEnabled(l == "local2" ? AgentKind::Local2 : AgentKind::Local1)) g.localQueue.push_back(l);
        }
        if (!g.localQueue.empty()) {
            groupRunLocals(c);
            return;
        }
    }
    groupHandOff(c, next, prompt, phase);
}

void App::groupPause(Conversation& c, const std::string& reason, AgentKind next, const std::string& prompt,
                     const std::string& phase, int round, const std::string& kind) {
    GroupRun& g = c.group;
    g.paused = true;
    g.pauseReason = reason;
    g.pauseKind = kind;
    g.pendingAgent = agentName(next);
    g.pendingPrompt = prompt;
    g.pendingPhase = phase;
    g.pendingRound = round;
    if (c.task.isObject()) {
        setNextAction(c.task, "paused: " + reason);
        c.task["status"] = "paused";
        saveTask(c.task);
    }
    bool limitHit = round > g.limit;
    pushMessage(c, J({{"author", "system"},
                      {"kind", "paused"},
                      {"text", reason},
                      {"action", limitHit          ? "more"
                                 : kind == "guard" ? "guard"
                                                   : "retry"}}));
    c.stateDirty = true;
    if (windowAction) windowAction("flash");
}

void App::groupContinue(Conversation& c) {
    GroupRun& g = c.group;
    if (g.pipeline && g.active && g.paused) {
        if (c.task["conflict"].boolean()) {
            c.task["head_revision"] = c.task["conflict_head"];
            c.task["conflict"] = false;
            c.task["passes"] = Json::object();
            c.task["tests_passed"] = false;
        }
        c.task["last_issue"] = "";
        pipelineNext(c);
        return;
    }
    if (!g.active || !g.paused) return;
    if (g.pendingRound > g.limit) g.limit += 2;
    AgentKind next = g.pendingAgent == "codex" ? AgentKind::Codex : AgentKind::Claude;
    if (g.pauseKind == "guard") {
        // One more send-back; if it fails again the group pauses again instead of retrying on its own.
        c.guardResend[(int)next] = true;
        c.guardTry[(int)next] = guardRetries();
        g.guardMsg.clear();
    }
    g.notesNext = g.notesFrom;  // a retry still has to answer the same local notes
    if (g.pendingRound != g.round) {
        pushMessage(c, J({{"author", "system"}, {"kind", "handoff"}, {"from", agentName(other(next))},
                          {"to", g.pendingAgent}, {"round", g.pendingRound},
                          {"text", "繼續協作，交給 " + nameOf(next)}}));
    }
    g.round = g.pendingRound;
    groupHandOff(c, next, g.pendingPrompt, g.pendingPhase);
}

void App::groupAccept(Conversation& c) {
    GroupRun& g = c.group;
    if (!g.active || !g.paused || g.pauseKind != "guard") return;
    Json* m = findMsg(c, g.guardMsg);
    if (!m) return;
    AgentKind k = g.pendingAgent == "codex" ? AgentKind::Codex : AgentKind::Claude;
    (*m)["guard"]["status"] = "accepted";
    markDirty(c, g.guardMsg);
    Json copy = *m;
    g.paused = false;
    g.pauseKind.clear();
    g.pauseReason.clear();
    g.guardMsg.clear();
    pushMessage(c, J({{"author", "system"},
                      {"kind", "info"},
                      {"text", userName() + " 接受了 " + nameOf(k) + " 這則未通過規範審查的回覆，協作繼續。"}}));
    groupTurnDone(c, k, copy, "completed");
}

void App::taskResume(Conversation& c) {
    if (c.kind == "group" && c.task["pipeline"]["nodes"].size() && !c.group.active &&
        c.task["status"].str() == "interrupted") {
        pipelineStart(c, "", true);
        return;
    }
    if (c.kind != "group" || c.group.active || !c.task.isObject() || c.task["status"].str() != "interrupted") {
        toast("error", "沒有可以接續的任務");
        return;
    }
    if (c.cwd.empty() || !util::dirExists(c.cwd)) {
        toast("error", "找不到這個群組的專案資料夾");
        return;
    }
    // The saved next step says who was about to work ("codex:review"); otherwise the lead.
    std::string next = c.task["handoffs"].size() ? c.task["handoffs"].items().back()["next_action"].str() : "";
    std::string who = next.substr(0, next.find(':'));
    std::string phase = next.find(':') == std::string::npos ? "" : next.substr(next.find(':') + 1);
    if (who != "claude" && who != "codex" && (who != "gemini" || !c.geminiMember)) {
        who = c.lead == "codex" ? "codex" : "claude";
        phase = c.task["handoffs"].size() ? "recheck" : "write";
    }
    if (phase != "write" && phase != "review" && phase != "recheck" && phase != "selfcheck") phase = "recheck";
    AgentKind k = kindOf(who);
    c.group = GroupRun();
    c.group.active = true;
    c.group.task = c.task["request"].str();
    c.group.limit = c.maxRounds;
    c.group.round = (int)c.task["handoffs"].size();
    // The latest change's author agrees with it, so a resumed review can still reach consensus in one turn.
    const auto& handoffs = c.task["handoffs"].items();
    for (auto it = handoffs.rbegin(); it != handoffs.rend(); ++it)
        if ((*it)["role"].str() == "write" || (*it)["changed_files"].size() > 0) {
            c.group.lastChanger = (*it)["agent_id"].str();
            break;
        }
    c.userStopped = false;
    c.workflow = WorkflowRun();
    const bool quota = c.task.has("quota_stop");
    c.task.erase("quota_stop");
    std::string prompt =
        std::string(quota ? "【CCA 任務接續】這個任務先前因為所有 AI 額度用完而強制停止，現在額度已恢復，接著做。"
                          : "【CCA 任務接續】CCA 在這個任務進行中關閉了，現在接著做。") +
        "下面是結構化的交接紀錄，"
        "請先用 git status、git diff 和實際檔案確認上一位留下的修改還在、測試狀態如何，再從下一步繼續，不要重新開始。"
        "紀錄只是摘要，與檔案不一致時以檔案為準。\n\n" +
        taskBrief(c.task, 8000) + "\n你接手的階段：" + phase + "。完成後照平常的規則在最後一行寫 VERDICT。";
    pushMessage(c, J({{"author", "system"},
                      {"kind", "info"},
                      {"text", "接續中斷的任務，交給 " + nameOf(k) + "（" + phase + "）。"}}));
    groupHandOff(c, k, prompt, phase);
}

void App::groupStop(Conversation& c) {
    GroupRun& g = c.group;
    if (!g.active) return;
    g.stopRequested = true;
    if (c.gemini && c.gemini->busy()) {
        c.gemini->shutdown();
        onAgentEvent(c.id, AgentKind::Gemini, {AgentEvent::Kind::TurnDone, "", J({{"status", "interrupted"}})});
        return;
    }
    if (c.worker) c.worker->shutdown();
    if (c.gemini) c.gemini->shutdown();
    c.stateDirty = true;
    bool busy = (c.claude && c.claude->busy()) || (c.codex && c.codex->busy()) || (c.local[0] && c.local[0]->busy()) ||
                (c.local[1] && c.local[1]->busy());
    if (!busy) {
        groupFinish(c, "stopped", "已停止協作。");
        return;
    }
    if (c.claude) c.claude->interrupt();
    if (c.codex) c.codex->interrupt();
    for (auto& l : c.local) if (l) l->interrupt();
}

void App::groupFinish(Conversation& c, const std::string& outcome, const std::string& detail) {
    c.workflow.outcome = outcome;
    if (c.task.isObject()) {
        Json previous = c.task;
        // A quota stop keeps its recorded next step, so the task stays resumable once quota returns.
        if (outcome != "quota") setNextAction(c.task, "done: " + outcome);
        c.task["status"] = outcome == "consensus" || outcome == "completed" ? "done"
                           : outcome == "stopped"                           ? "stopped"
                           : outcome == "quota"                             ? "interrupted"
                                                                            : "failed";
        c.task["outcome"] = detail;
        if (outcome == "consensus" || outcome == "completed") c.task.erase("auto_resume_attempts");
        if (!saveTask(c.task) && c.group.pipeline && (outcome == "consensus" || outcome == "completed")) {
            c.task = previous;
            c.workflow.outcome = "blocked";
            pipelineBlock(c, "任務結果無法保存，沒有標記完成；請修復資料目錄後重試");
            return;
        }
    }
    c.workspaceLease.reset();
    GroupRun& g = c.group;
    Json files = Json::array();
    long long added = 0, removed = 0;
    for (const auto& f : g.allFiles.fields()) {
        files.push(J({{"path", f.first}, {"added", f.second["added"]}, {"removed", f.second["removed"]},
                      {"change", f.second["change"]}}));
        added += f.second["added"].i64();
        removed += f.second["removed"].i64();
    }
    c.workflow.round = g.round;
    c.workflow.files = (int)files.size();
    c.workflow.added = added;
    c.workflow.removed = removed;
    pushMessage(c, J({{"author", "system"},
                      {"kind", "summary"},
                      {"outcome", outcome},
                      {"rounds", g.round},
                      {"files", files},
                      {"added", added},
                      {"removed", removed},
                      {"text", detail}}));
    c.group = GroupRun();
    c.stateDirty = true;
    if (windowAction) windowAction("flash");
    finishPlan(c, outcome);
}
