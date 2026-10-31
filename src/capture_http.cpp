#include "arcbox/capture_http.hpp"

#include <mutex>
#include <sstream>
#include <string>

#include <curl/curl.h>

#include "arcbox/util.hpp"

namespace arcbox {

struct BodySink {
    std::string body;
    size_t max_bytes = 0;
    bool overflow = false;
};

struct HeaderSink {
    std::string headers;
    std::string status_line;
};

static size_t body_callback(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* sink = static_cast<BodySink*>(userdata);
    size_t bytes = size * nmemb;
    if (sink->max_bytes > 0 && sink->body.size() + bytes > sink->max_bytes) {
        sink->overflow = true;
        return 0;
    }
    sink->body.append(ptr, bytes);
    return bytes;
}

static size_t header_callback(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* sink = static_cast<HeaderSink*>(userdata);
    size_t bytes = size * nmemb;
    std::string line(ptr, bytes);

    if (line.rfind("HTTP/", 0) == 0) {
        sink->status_line = trim(line);
        sink->headers.clear();
        return bytes;
    }
    if (line == "\r\n" || line == "\n") {
        return bytes;
    }
    sink->headers += line;
    return bytes;
}

HttpCapture fetch_http(const std::string& url, const Config& cfg) {
    HttpCapture result;
    result.requested_url = url;
    result.effective_url = url;

    static std::once_flag init_flag;
    std::call_once(init_flag, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });

    CURL* curl = curl_easy_init();
    if (!curl) {
        result.error = "curl_easy_init failed";
        return result;
    }

    BodySink body;
    body.max_bytes = static_cast<size_t>(cfg.max_body_bytes);
    HeaderSink header;

    struct curl_slist* request_headers = nullptr;
    request_headers = curl_slist_append(
        request_headers,
        "Accept: text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8");
    request_headers =
        curl_slist_append(request_headers, "Accept-Encoding: identity");
    request_headers =
        curl_slist_append(request_headers, "Accept-Language: en-US,en;q=0.5");

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_USERAGENT, cfg.user_agent.c_str());
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 10L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, cfg.connect_timeout_seconds);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, cfg.timeout_seconds);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, request_headers);
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "http,https");
    curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, "http,https");
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, body_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, header_callback);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &header);

    CURLcode rc = curl_easy_perform(curl);

    if (rc != CURLE_OK) {
        if (rc == CURLE_WRITE_ERROR && body.overflow) {
            result.error = "response body exceeds configured limit";
        } else {
            result.error = curl_easy_strerror(rc);
        }
    }

    char* effective_url = nullptr;
    curl_easy_getinfo(curl, CURLINFO_EFFECTIVE_URL, &effective_url);
    if (effective_url && *effective_url) {
        result.effective_url = effective_url;
    }

    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    result.status_code = static_cast<int>(status);
    result.status_line = header.status_line;
    result.headers = header.headers;
    result.body = std::move(body.body);

    char* content_type = nullptr;
    curl_easy_getinfo(curl, CURLINFO_CONTENT_TYPE, &content_type);
    if (content_type && *content_type) {
        result.content_type = content_type;
    }

    if (result.status_code > 0 && result.status_line.empty()) {
        result.status_line =
            "HTTP/1.1 " + std::to_string(result.status_code) + " Unknown";
    }

    curl_slist_free_all(request_headers);
    curl_easy_cleanup(curl);
    return result;
}

} // namespace arcbox
