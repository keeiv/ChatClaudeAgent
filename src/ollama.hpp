// Local models through Ollama's HTTP API on 127.0.0.1:11434. As reviewers they get
// read-only file tools inside the project folder (local_tools.hpp) and nothing else.
#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "agent.hpp"
#include "guard.hpp"
#include "json.hpp"

namespace ollama {

// Streams a request; onLine gets each NDJSON line, onDone runs last with the HTTP
// status (0 when the server could not be reached). Both run on a worker thread.
// Setting *cancel to true stops reading.
void request(const std::string& method, const std::string& path, const std::string& body,
             std::function<void(const std::string& line)> onLine,
             std::function<void(int status, const std::string& error)> onDone,
             std::shared_ptr<std::atomic<bool>> cancel = nullptr);

// Blocking form of request(), for worker threads. Returns the HTTP status (0: unreachable).
int perform(const std::string& method, const std::string& path, const std::string& body,
            const std::function<void(const std::string& line)>& onLine, std::atomic<bool>* cancel, std::string* error,
            unsigned long connectTimeoutMs = 3000);

std::wstring findExe();  // ollama.exe, or empty when Ollama is not installed

}  // namespace ollama

// How a review turn runs: the folder its tools may read, what changed, and the rules its
// final answer is checked against before CCA accepts it.
struct LocalReviewSetup {
    bool review = false;                    // false: plain chat, no tools, no checks
    std::wstring root;                      // project folder the tools may read
    std::vector<std::string> changedPaths;  // normalized (normalizeRelPath), deleted files left out
    GuardRules rules;
    int maxCorrections = 2;
    bool think = false;  // the model supports Ollama's "think"
};

// One local model in one conversation. UI thread only.
class LocalSession : public std::enable_shared_from_this<LocalSession> {
public:
    LocalSession(AgentKind kind, AgentEventFn onEvent);
    ~LocalSession();

    void configure(const std::string& model, const std::string& system);
    void setReview(LocalReviewSetup setup) { review_ = std::move(setup); }
    // history: earlier turns as [{role, content}], oldest first.
    bool send(const std::string& text, const Json& history, std::string* error);
    void interrupt();
    bool busy() const { return busy_; }

private:
    void emit(AgentEvent::Kind k, const std::string& id, Json data);
    static void runTurn(std::weak_ptr<LocalSession> self, std::string model, Json messages, LocalReviewSetup setup,
                        std::shared_ptr<std::atomic<bool>> cancel, int turn);

    AgentKind kind_;
    AgentEventFn onEvent_;
    std::string model_, system_;
    LocalReviewSetup review_;
    bool busy_ = false;
    std::shared_ptr<std::atomic<bool>> cancel_;
    int seq_ = 0;
};
