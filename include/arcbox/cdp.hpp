#pragma once

#include <chrono>
#include <cstdint>
#include <random>
#include <string>
#include <vector>

namespace arcbox {

class Adblock;

// Smallest usable Chrome DevTools Protocol client.
//
// It speaks to a browser arcbox started itself, over one WebSocket on the
// loopback interface: text frames only, everything bounded by a deadline and a
// message size ceiling, so a chatty page cannot turn the render step into a new
// OOM source. Requests are answered by `Fetch.requestPaused` events and filtered
// through the same blocklist the HTTP path uses, which closes the gap where
// Chrome fetched its own subresources past the archiver.
class CdpClient {
public:
    struct Outcome {
        bool navigated = false;
        std::string html;
        std::string screenshot_png;
        std::string pdf;
        std::vector<std::string> blocked_urls;
        std::string error;
    };

    CdpClient();
    ~CdpClient();

    CdpClient(const CdpClient&) = delete;
    CdpClient& operator=(const CdpClient&) = delete;

    // Waits for the browser's debugging endpoint on `port` and opens the
    // WebSocket behind it.
    bool connect(int port, int timeout_seconds, std::string& error);

    // Attaches to a fresh page, filters its requests through `adblock`,
    // navigates to `url` and collects the requested artifacts from that single
    // session. Artifacts above `max_artifact_bytes` are dropped.
    Outcome render(const Adblock& adblock,
                   const std::string& url,
                   bool dom,
                   bool screenshot,
                   bool pdf,
                   int timeout_seconds,
                   long max_artifact_bytes);

private:
    enum class Frame { Incomplete, Message, Closed };

    int send_command(const std::string& method,
                     const std::string& params,
                     const std::string& session);
    bool await_result(int id, std::string& payload, std::string& error);
    bool handle_event(const std::string& method, const std::string& message);
    int remaining_seconds() const;

    bool send_frame(unsigned char opcode, const std::string& payload);
    bool send_text(const std::string& payload);
    bool recv_text(std::string& payload, std::string& error);
    Frame take_frame(std::string& payload, std::string& error);

    int fd_ = -1;
    int next_id_ = 1;
    std::string buffer_;
    std::string fragment_;
    std::string session_;
    std::string error_;
    bool loaded_ = false;

    const Adblock* filter_ = nullptr;
    std::vector<std::string>* blocked_ = nullptr;
    std::chrono::steady_clock::time_point deadline_;
    std::mt19937 random_;
};

}  // namespace arcbox
