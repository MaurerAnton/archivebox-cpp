#include "arcbox/cdp.hpp"

#include <cctype>
#include <cerrno>
#include <iostream>
#include <cstring>
#include <string>
#include <unordered_set>
#include <thread>
#include <vector>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include "arcbox/adblock.hpp"
#include "arcbox/util.hpp"

namespace arcbox {
namespace {

using Clock = std::chrono::steady_clock;

// No single DevTools message may exceed this: a page that answers with a giant
// frame gets its connection dropped instead of the client growing without end.
constexpr std::size_t kMaxMessageBytes = 96u * 1024u * 1024u;
constexpr std::size_t kReadChunk = 65536;
constexpr std::size_t kMaxHttpResponse = 1024u * 1024u;

// Same window as the browser glue: the archive keeps working either way, so a
// trace is opt-in.
void trace(const std::string& line) {
    static const bool enabled = std::getenv("ARCBOX_DEBUG") != nullptr;
    if (!enabled) {
        return;
    }
    std::cerr << "arcbox: cdp " << line << std::endl;
}

std::string quote(const std::string& s) {
    return "\"" + json_escape(s) + "\"";
}

bool write_all(int fd, const std::string& bytes, std::string& error) {
    std::size_t sent = 0;
    while (sent < bytes.size()) {
        pollfd p{fd, POLLOUT, 0};
        if (::poll(&p, 1, 10000) <= 0) {
            error = "write timed out";
            return false;
        }
        if (p.revents & (POLLERR | POLLHUP | POLLNVAL)) {
            error = "socket went away while writing";
            return false;
        }
        const ssize_t n = ::send(fd, bytes.data() + sent,
                                 bytes.size() - sent, 0);
        if (n <= 0) {
            error = std::string("send: ") + std::strerror(errno);
            return false;
        }
        sent += static_cast<std::size_t>(n);
    }
    return true;
}

// Pulls whatever is readable into `buffer`. Returns false only for real
// errors; an idle socket simply leaves the buffer as it is.
bool read_some(int fd, std::string& buffer, int timeout_ms, std::string& error,
               bool& closed) {
    pollfd p{fd, POLLIN, 0};
    const int rc = ::poll(&p, 1, timeout_ms);
    if (rc == 0) {
        return true;
    }
    if (rc < 0) {
        if (errno == EINTR) {
            return true;
        }
        error = std::string("poll: ") + std::strerror(errno);
        return false;
    }
    if (p.revents & (POLLERR | POLLHUP | POLLNVAL)) {
        closed = true;
        if (!(p.revents & POLLIN)) {
            return true;
        }
    }
    if (buffer.size() >= kMaxMessageBytes) {
        error = "devtools stream exceeded the message ceiling";
        return false;
    }
    char chunk[kReadChunk];
    const ssize_t n = ::recv(fd, chunk, sizeof(chunk), 0);
    if (n == 0) {
        closed = true;
        return true;
    }
    if (n < 0) {
        if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
            return true;
        }
        error = std::string("recv: ") + std::strerror(errno);
        return false;
    }
    buffer.append(chunk, static_cast<std::size_t>(n));
    return true;
}

// Every read is bounded by the whole budget, not per poll slice: a peer that
// answers slowly - or not at all - must still let the caller move on.
bool millis_left(Clock::time_point deadline, int& out) {
    const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
                          deadline - Clock::now())
                          .count();
    if (left <= 0) {
        out = 0;
        return false;
    }
    out = static_cast<int>(left > 2000 ? 2000 : left);
    return true;
}

bool read_until(int fd, std::string& out, const std::string& terminator,
                int timeout_ms, std::string& error) {
    const auto deadline = Clock::now() + std::chrono::milliseconds(timeout_ms);
    bool closed = false;
    while (out.find(terminator) == std::string::npos) {
        int slice = 0;
        if (!millis_left(deadline, slice)) {
            error = "read timed out";
            return false;
        }
        if (!read_some(fd, out, slice, error, closed)) {
            return false;
        }
        if (closed) {
            break;
        }
    }
    return true;
}

// A short-lived connection to the browser's own debugging HTTP endpoint.
int connect_tcp(int port, std::string& error) {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        error = std::string("socket: ") + std::strerror(errno);
        return -1;
    }
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(static_cast<uint16_t>(port));
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    const int flags = ::fcntl(fd, F_GETFL, 0);
    ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    const int rc = ::connect(fd, reinterpret_cast<sockaddr*>(&address),
                             sizeof(address));
    if (rc < 0 && errno != EINPROGRESS) {
        error = std::string("connect: ") + std::strerror(errno);
        ::close(fd);
        return -1;
    }
    pollfd p{fd, POLLOUT, 0};
    if (::poll(&p, 1, 5000) <= 0 || !(p.revents & POLLOUT) ||
        (p.revents & (POLLERR | POLLHUP | POLLNVAL))) {
        error = "debug port did not accept the connection";
        ::close(fd);
        return -1;
    }
    // A non-blocking connect reports success through poll even when the peer
    // refused it; only SO_ERROR says whether the socket is really usable.
    int socket_error = 0;
    socklen_t length = sizeof(socket_error);
    if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &socket_error, &length) < 0 ||
        socket_error != 0) {
        error = socket_error == 0
                    ? std::string("connect failed")
                    : (std::string("connect: ") + std::strerror(socket_error));
        ::close(fd);
        return -1;
    }
    ::fcntl(fd, F_SETFL, flags);
    return fd;
}

// Finds the value position right after `"key":`, skipping whitespace.
std::size_t find_value(const std::string& json, const std::string& key) {
    const std::string needle = "\"" + key + "\"";
    std::size_t at = json.find(needle);
    while (at != std::string::npos) {
        std::size_t colon = json.find(':', at + needle.size());
        if (colon != std::string::npos) {
            ++colon;
            while (colon < json.size() &&
                   std::isspace(static_cast<unsigned char>(json[colon]))) {
                ++colon;
            }
            return colon;
        }
        at = json.find(needle, at + 1);
    }
    return std::string::npos;
}

void append_utf8(std::string& out, unsigned long code) {
    if (code < 0x80) {
        out += static_cast<char>(code);
    } else if (code < 0x800) {
        out += static_cast<char>(0xc0 | (code >> 6));
        out += static_cast<char>(0x80 | (code & 0x3f));
    } else if (code < 0x10000) {
        out += static_cast<char>(0xe0 | (code >> 12));
        out += static_cast<char>(0x80 | ((code >> 6) & 0x3f));
        out += static_cast<char>(0x80 | (code & 0x3f));
    } else {
        out += static_cast<char>(0xf0 | (code >> 18));
        out += static_cast<char>(0x80 | ((code >> 12) & 0x3f));
        out += static_cast<char>(0x80 | ((code >> 6) & 0x3f));
        out += static_cast<char>(0x80 | (code & 0x3f));
    }
}

std::string json_string_field(const std::string& json, const std::string& key) {
    const std::size_t at = find_value(json, key);
    if (at == std::string::npos || at >= json.size() || json[at] != '"') {
        return std::string();
    }
    std::string out;
    out.reserve(256);
    std::size_t i = at + 1;
    while (i < json.size() && json[i] != '"') {
        const char c = json[i];
        if (c != '\\') {
            out += c;
            ++i;
            continue;
        }
        if (i + 1 >= json.size()) {
            break;
        }
        const char escape = json[i + 1];
        i += 2;
        switch (escape) {
            case 'n':
                out += '\n';
                break;
            case 't':
                out += '\t';
                break;
            case 'r':
                out += '\r';
                break;
            case 'b':
                out += '\b';
                break;
            case 'f':
                out += '\f';
                break;
            case '/':
                out += '/';
                break;
            case '"':
                out += '"';
                break;
            case '\\':
                out += '\\';
                break;
            case 'u': {
                if (i + 4 > json.size()) {
                    return out;
                }
                try {
                    append_utf8(out, std::stoul(json.substr(i, 4), nullptr, 16));
                } catch (const std::exception&) {
                    // Malformed escape: keep going instead of failing the render.
                }
                i += 4;
                break;
            }
            default:
                out += escape;
                break;
        }
    }
    return out;
}

int json_int_field(const std::string& json, const std::string& key) {
    const std::size_t at = find_value(json, key);
    if (at == std::string::npos) {
        return 0;
    }
    std::size_t i = at;
    bool negative = false;
    if (i < json.size() && json[i] == '-') {
        negative = true;
        ++i;
    }
    long value = 0;
    bool any = false;
    while (i < json.size() && std::isdigit(static_cast<unsigned char>(json[i]))) {
        value = value * 10 + (json[i] - '0');
        ++i;
        any = true;
    }
    if (!any) {
        return 0;
    }
    return static_cast<int>(negative ? -value : value);
}

bool json_error_message(const std::string& json, std::string& message) {
    const std::size_t pos = json.find("\"error\"");
    if (pos == std::string::npos) {
        return false;
    }
    message = json_string_field(json.substr(pos), "message");
    if (message.empty()) {
        message = "unknown devtools error";
    }
    return true;
}

// Elements whose URL host is on the blocklist are removed as they appear, which
// is what fits inside this renderer: no cosmetic filter language, just the hosts
// the archive already refuses.
std::string hide_script(const std::vector<std::string>& hosts) {
    if (hosts.empty()) {
        return std::string();
    }
    std::string list;
    for (std::size_t i = 0; i < hosts.size(); ++i) {
        if (i > 0) {
            list += ",";
        }
        list += "\"" + json_escape(hosts[i]) + "\"";
    }
    // The script runs before the document has a root element, so attaching the
    // observer has to wait - otherwise the whole injection throws and nothing
    // gets hidden at all.
    return R"js((function(){var hosts=[)js" + list +
           R"js(];function host(u){try{return new URL(u,location.href).hostname}catch(e){return ""}}
function clean(){var nodes=document.querySelectorAll("iframe,img,script,link[rel=stylesheet]");
for(var i=0;i<nodes.length;i++){var s=nodes[i].src||nodes[i].href||"";if(!s)continue;var h=host(s);
if(h&&hosts.indexOf(h)>=0&&nodes[i].parentNode){nodes[i].parentNode.removeChild(nodes[i])}}}
function install(){if(!document.documentElement){document.addEventListener("readystatechange",install);return;}
clean();new MutationObserver(clean).observe(document.documentElement,{childList:true,subtree:true});}
install();})();)js";
}

}  // namespace

CdpClient::CdpClient() : random_(std::random_device{}()) {}

CdpClient::~CdpClient() {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}

int CdpClient::remaining_seconds() const {
    const auto left = std::chrono::duration_cast<std::chrono::seconds>(
                          deadline_ - Clock::now())
                          .count();
    return left > 0 ? static_cast<int>(left) : 0;
}

bool CdpClient::send_frame(unsigned char opcode, const std::string& payload) {
    if (fd_ < 0) {
        return false;
    }
    std::vector<unsigned char> frame;
    frame.push_back(opcode);
    const std::size_t size = payload.size();
    if (size < 126) {
        frame.push_back(static_cast<unsigned char>(0x80 | size));
    } else if (size <= 0xffff) {
        frame.push_back(0x80 | 126);
        frame.push_back(static_cast<unsigned char>((size >> 8) & 0xff));
        frame.push_back(static_cast<unsigned char>(size & 0xff));
    } else {
        frame.push_back(0x80 | 127);
        for (int shift = 56; shift >= 0; shift -= 8) {
            frame.push_back(static_cast<unsigned char>((size >> shift) & 0xff));
        }
    }
    const std::uint32_t key = random_();
    const unsigned char mask[4] = {
        static_cast<unsigned char>(key & 0xff),
        static_cast<unsigned char>((key >> 8) & 0xff),
        static_cast<unsigned char>((key >> 16) & 0xff),
        static_cast<unsigned char>((key >> 24) & 0xff)};
    frame.insert(frame.end(), mask, mask + 4);
    for (std::size_t i = 0; i < size; ++i) {
        frame.push_back(static_cast<unsigned char>(payload[i]) ^ mask[i % 4]);
    }
    return write_all(fd_, std::string(frame.begin(), frame.end()), error_);
}

bool CdpClient::send_text(const std::string& payload) {
    return send_frame(0x81, payload);  // FIN + text opcode
}

CdpClient::Frame CdpClient::take_frame(std::string& payload,
                                       std::string& error) {
    while (true) {
        if (buffer_.size() < 2) {
            return Frame::Incomplete;
        }
        const auto first = static_cast<unsigned char>(buffer_[0]);
        const auto second = static_cast<unsigned char>(buffer_[1]);
        const bool fin = (first & 0x80) != 0;
        const int opcode = first & 0x0f;
        const bool masked = (second & 0x80) != 0;
        std::uint64_t length = second & 0x7f;
        std::size_t header = 2;
        if (length == 126) {
            if (buffer_.size() < 4) {
                return Frame::Incomplete;
            }
            length = (static_cast<std::uint64_t>(
                          static_cast<unsigned char>(buffer_[2]))
                      << 8) |
                     static_cast<unsigned char>(buffer_[3]);
            header = 4;
        } else if (length == 127) {
            if (buffer_.size() < 10) {
                return Frame::Incomplete;
            }
            length = 0;
            for (int i = 2; i < 10; ++i) {
                length = (length << 8) |
                         static_cast<unsigned char>(buffer_[i]);
            }
            header = 10;
        }
        if (length > kMaxMessageBytes) {
            error = "devtools message above the size ceiling";
            buffer_.clear();
            return Frame::Closed;
        }
        const std::size_t mask_start = header;
        const std::size_t body_start = mask_start + (masked ? 4 : 0);
        const std::size_t total = body_start + static_cast<std::size_t>(length);
        if (buffer_.size() < total) {
            return Frame::Incomplete;
        }
        std::string body = buffer_.substr(body_start,
                                          static_cast<std::size_t>(length));
        if (masked) {
            for (std::size_t i = 0; i < body.size(); ++i) {
                body[i] = static_cast<char>(
                    static_cast<unsigned char>(body[i]) ^
                    static_cast<unsigned char>(buffer_[mask_start + (i % 4)]));
            }
        }
        buffer_.erase(0, total);

        if (opcode == 0x8) {
            return Frame::Closed;
        }
        if (opcode == 0x9) {  // ping: answer with a pong and keep waiting
            send_frame(0x8A, body);
            continue;
        }
        if (opcode == 0xA) {
            continue;
        }
        if (opcode == 0x1) {
            fragment_ = std::move(body);
        } else if (opcode == 0x0) {
            if (fragment_.size() + body.size() > kMaxMessageBytes) {
                error = "fragmented devtools message above the size ceiling";
                fragment_.clear();
                return Frame::Closed;
            }
            fragment_ += body;
        } else {
            continue;  // binary or reserved: not part of this protocol
        }
        if (!fin) {
            continue;
        }
        payload = std::move(fragment_);
        fragment_.clear();
        return Frame::Message;
    }
}

bool CdpClient::recv_text(std::string& payload, std::string& error) {
    const int deadline_slice_ms = 200;
    while (true) {
        const Frame frame = take_frame(payload, error);
        if (frame == Frame::Message) {
            return true;
        }
        if (frame == Frame::Closed) {
            error = error.empty() ? std::string("devtools socket closed")
                                  : error;
            return false;
        }
        if (remaining_seconds() <= 0) {
            error = "timed out";
            return false;
        }
        bool closed = false;
        if (!read_some(fd_, buffer_, deadline_slice_ms, error, closed)) {
            return false;
        }
        if (closed && take_frame(payload, error) == Frame::Incomplete) {
            error = "devtools socket closed";
            return false;
        }
    }
}

bool CdpClient::connect(int port, int timeout_seconds, std::string& error) {
    const auto deadline = Clock::now() + std::chrono::seconds(timeout_seconds);
    std::string websocket_path;
    int attempt = 0;
    while (Clock::now() < deadline) {
        ++attempt;
        trace("endpoint attempt " + std::to_string(attempt));
        const int fd = connect_tcp(port, error);
        if (fd >= 0) {
            const std::string request =
                "GET /json/version HTTP/1.1\r\nHost: 127.0.0.1:" +
                std::to_string(port) + "\r\nConnection: close\r\n\r\n";
            std::string body;
            // Reading until the marker avoids waiting for the close: this
            // endpoint keeps the connection open after the answer.
            if (write_all(fd, request, error) &&
                read_until(fd, body, "webSocketDebuggerUrl", 5000, error)) {
                websocket_path =
                    json_string_field(body, "webSocketDebuggerUrl");
            }
            trace("attempt got " + std::to_string(body.size()) +
                  " B, error: " + error);
            ::close(fd);
        } else {
            trace("attempt failed: " + error);
        }
        if (!websocket_path.empty()) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    trace("endpoint: " + websocket_path);
    if (websocket_path.empty()) {
        error = "the browser debugging endpoint never came up";
        return false;
    }

    const std::size_t slash = websocket_path.find("/devtools/");
    if (slash == std::string::npos) {
        error = "unexpected debugger url: " + websocket_path;
        return false;
    }
    const std::string path = websocket_path.substr(slash);

    fd_ = connect_tcp(port, error);
    if (fd_ < 0) {
        return false;
    }
    std::vector<unsigned char> nonce(16);
    for (unsigned char& byte : nonce) {
        byte = static_cast<unsigned char>(random_());
    }
    const std::string request =
        "GET " + path + " HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(port) +
        "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: " +
        base64_encode(nonce) + "\r\nSec-WebSocket-Version: 13\r\n\r\n";
    if (!write_all(fd_, request, error)) {
        return false;
    }
    std::string handshake;
    if (!read_until(fd_, handshake, "\r\n\r\n", 5000, error)) {
        return false;
    }
    if (handshake.find("101") == std::string::npos) {
        error = "websocket handshake refused";
        return false;
    }
    trace("websocket up");
    return true;
}

int CdpClient::send_command(const std::string& method,
                            const std::string& params,
                            const std::string& session) {
    const int id = next_id_++;
    std::string payload = "{\"id\":" + std::to_string(id) +
                          ",\"method\":" + quote(method);
    if (!session.empty()) {
        payload += ",\"sessionId\":" + quote(session);
    }
    payload += ",\"params\":" + (params.empty() ? "{}" : params) + "}";
    if (!send_text(payload)) {
        trace("send failed: " + error_);
        return -1;
    }
    trace("-> " + method + " #" + std::to_string(id));
    return id;
}

bool CdpClient::handle_event(const std::string& method,
                             const std::string& message) {
    if (method == "Page.loadEventFired") {
        loaded_ = true;
        trace("load event");
        return true;
    }
    trace("event " + method + " (" + std::to_string(message.size()) + " B)");
    if (method != "Fetch.requestPaused" || filter_ == nullptr) {
        return true;
    }
    const std::string request_id = json_string_field(message, "requestId");
    if (request_id.empty()) {
        return true;
    }
    const std::string request_url = json_string_field(message, "url");
    const bool refused =
        !request_url.empty() && filter_->blocked(request_url);
    trace("request " + request_url + (refused ? " blocked" : " allowed"));
    if (refused) {
        if (blocked_ != nullptr) {
            blocked_->push_back(request_url);
        }
        send_command("Fetch.failRequest",
                     "{\"requestId\":" + quote(request_id) +
                         ",\"errorReason\":\"BlockedByClient\"}",
                     session_);
    } else {
        send_command("Fetch.continueRequest",
                     "{\"requestId\":" + quote(request_id) + "}", session_);
    }
    return true;
}

bool CdpClient::await_result(int id, std::string& payload, std::string& error) {
    if (id < 0) {
        error = "cannot send command";
        return false;
    }
    while (true) {
        if (remaining_seconds() <= 0) {
            error = "timed out";
            trace("await #" + std::to_string(id) + " timed out");
            return false;
        }
        std::string message;
        if (!recv_text(message, error)) {
            trace("await #" + std::to_string(id) + " recv: " + error);
            return false;
        }
        if (message.empty()) {
            continue;
        }
        const int reply_id = json_int_field(message, "id");
        if (reply_id == 0) {
            const std::string method = json_string_field(message, "method");
            if (!method.empty()) {
                handle_event(method, message);
            }
            continue;
        }
        if (reply_id != id) {
            continue;  // an answer to a request already settled
        }
        std::string failure;
        if (json_error_message(message, failure)) {
            error = failure;
            trace("<- #" + std::to_string(id) + " error: " + failure);
            return false;
        }
        trace("<- #" + std::to_string(id) + " ok " + std::to_string(message.size()) + " B");
        payload = message;
        return true;
    }
}

CdpClient::Outcome CdpClient::render(const Adblock& adblock,
                                     const std::string& url,
                                     bool dom,
                                     bool screenshot,
                                     bool pdf,
                                     int timeout_seconds,
                                     long max_artifact_bytes) {
    Outcome out;
    std::vector<std::string> blocked;
    filter_ = &adblock;
    blocked_ = &blocked;
    loaded_ = false;
    session_.clear();
    deadline_ = Clock::now() +
                std::chrono::seconds(timeout_seconds > 0 ? timeout_seconds : 1);

    std::string error;
    std::string reply;

    const int create = send_command(
        "Target.createTarget",
        "{\"url\":\"about:blank\",\"newWindow\":false}", std::string());
    if (!await_result(create, reply, error)) {
        out.error = "Target.createTarget: " + error;
        return out;
    }
    const std::string target_id = json_string_field(reply, "targetId");
    if (target_id.empty()) {
        out.error = "devtools gave no target";
        return out;
    }
    const int attach = send_command(
        "Target.attachToTarget",
        "{\"targetId\":" + quote(target_id) + ",\"flatten\":true}",
        std::string());
    if (!await_result(attach, reply, error)) {
        out.error = "Target.attachToTarget: " + error;
        return out;
    }
    session_ = json_string_field(reply, "sessionId");
    if (session_.empty()) {
        out.error = "devtools gave no session";
        return out;
    }

    // Interception is best effort: without it the host-resolver rules still
    // cover the same hosts, so a refused Fetch.enable must not lose the render.
    const int page = send_command("Page.enable", std::string(), session_);
    if (!await_result(page, reply, error)) {
        out.error = "Page.enable: " + error;
        return out;
    }
    (void)await_result(send_command("Runtime.enable", std::string(), session_),
                       reply, error);
    const int fetch = send_command(
        "Fetch.enable",
        "{\"patterns\":[{\"urlPattern\":\"*\",\"requestStage\":\"Request\"}]}",
        session_);
    error.clear();
    const bool intercepting = await_result(fetch, reply, error);
    if (!intercepting) {
        error.clear();
    }

    const std::string script = hide_script(adblock.resolver_rules(url, 64));
    if (!script.empty()) {
        (void)await_result(send_command("Page.addScriptToEvaluateOnNewDocument",
                                        "{\"source\":" + quote(script) + "}",
                                        session_),
                           reply, error);
    }

    const int navigate = send_command(
        "Page.navigate", "{\"url\":" + quote(url) + "}", session_);
    error.clear();
    if (!await_result(navigate, reply, error)) {
        out.error = "Page.navigate: " + error;
        return out;
    }
    out.navigated = true;

    // Wait for the load event while answering the requests Chrome pauses.
    while (!loaded_) {
        if (remaining_seconds() <= 0) {
            break;
        }
        std::string message;
        if (!recv_text(message, error)) {
            break;
        }
        const std::string method = json_string_field(message, "method");
        if (!method.empty()) {
            handle_event(method, message);
        }
    }

    if (dom) {
        const int eval = send_command(
            "Runtime.evaluate",
            "{\"expression\":\"document.documentElement.outerHTML\","
            "\"returnByValue\":true,\"awaitPromise\":false}",
            session_);
        error.clear();
        if (await_result(eval, reply, error)) {
            std::string html = json_string_field(reply, "value");
            if (!html.empty() &&
                static_cast<long>(html.size()) <= max_artifact_bytes) {
                out.html = std::move(html);
            } else if (!html.empty() && out.error.empty()) {
                out.error = "rendered DOM above the size ceiling";
            }
        }
    }

    if (screenshot) {
        const int shot =
            send_command("Page.captureScreenshot",
                         "{\"format\":\"png\",\"captureBeyondViewport\":false}",
                         session_);
        error.clear();
        if (await_result(shot, reply, error)) {
            std::vector<unsigned char> bytes;
            if (base64_decode(json_string_field(reply, "data"), bytes) &&
                static_cast<long>(bytes.size()) <= max_artifact_bytes) {
                out.screenshot_png.assign(bytes.begin(), bytes.end());
            }
        }
    }

    if (pdf) {
        const int print = send_command("Page.printToPDF",
                                       "{\"printBackground\":true}", session_);
        error.clear();
        if (await_result(print, reply, error)) {
            std::vector<unsigned char> bytes;
            if (base64_decode(json_string_field(reply, "data"), bytes) &&
                static_cast<long>(bytes.size()) <= max_artifact_bytes) {
                out.pdf.assign(bytes.begin(), bytes.end());
            }
        }
    }

    std::unordered_set<std::string> seen;
    seen.reserve(blocked.size());
    for (const std::string& request : blocked) {
        if (seen.insert(request).second) {
            out.blocked_urls.push_back(request);
        }
    }
    if (!out.html.empty() || !out.screenshot_png.empty() || !out.pdf.empty()) {
        out.error.clear();
    } else if (out.error.empty()) {
        out.error = "devtools produced no artifact";
    }

    filter_ = nullptr;
    blocked_ = nullptr;
    return out;
}

}  // namespace arcbox
