#include "arcbox/warc.hpp"

#include <sstream>
#include <string>

#include <zlib.h>

#include "arcbox/util.hpp"

namespace arcbox {

static bool gz_write_string(gzFile gz, const std::string& text, std::string& error) {
    if (text.empty()) {
        return true;
    }
    int written = gzwrite(gz, text.data(), static_cast<unsigned int>(text.size()));
    if (written < 0 || static_cast<size_t>(written) != text.size()) {
        error = "gzwrite failed: " + std::string(gzerror(gz, nullptr));
        return false;
    }
    return true;
}

static bool write_record(gzFile gz,
                         const std::string& fields,
                         const std::string& block,
                         std::string& error) {
    std::ostringstream header;
    header << "WARC/1.0\r\n" << fields;
    header << "Content-Length: " << block.size() << "\r\n\r\n";

    if (!gz_write_string(gz, header.str(), error)) {
        return false;
    }
    if (!gz_write_string(gz, block, error)) {
        return false;
    }
    return gz_write_string(gz, "\r\n\r\n", error);
}

bool write_warc(const std::filesystem::path& output,
                const HttpCapture& capture,
                const Config& cfg,
                std::string& error) {
    std::filesystem::create_directories(output.parent_path());

    gzFile gz = gzopen(output.c_str(), "wb6");
    if (!gz) {
        error = "cannot open WARC for writing: " + output.string();
        return false;
    }

    const std::string filename = output.filename().string();
    const std::string date = now_iso8601();

    std::ostringstream warcinfo_fields;
    warcinfo_fields << "WARC-Type: warcinfo\r\n"
                    << "WARC-Date: " << date << "\r\n"
                    << "WARC-Record-ID: <urn:uuid:" << uuid_v4() << ">\r\n"
                    << "WARC-Filename: " << filename << "\r\n"
                    << "Content-Type: application/warc-fields\r\n";

    std::ostringstream warcinfo_body;
    warcinfo_body << "software: ArchiveBox.cpp/0.1\r\n"
                  << "format: WARC File Format 1.0\r\n"
                  << "description: WARC written by arcbox\r\n";

    if (!write_record(gz, warcinfo_fields.str(), warcinfo_body.str(), error)) {
        gzclose(gz);
        return false;
    }

    std::string request_block = "GET " + url_path(capture.requested_url) +
                                " HTTP/1.1\r\nHost: " +
                                url_authority(capture.requested_url) +
                                "\r\nUser-Agent: " + cfg.user_agent +
                                "\r\nAccept: */*\r\nConnection: close\r\n\r\n";

    std::ostringstream request_fields;
    request_fields << "WARC-Type: request\r\n"
                   << "WARC-Target-URI: " << capture.requested_url << "\r\n"
                   << "WARC-Date: " << date << "\r\n"
                   << "WARC-Record-ID: <urn:uuid:" << uuid_v4() << ">\r\n"
                   << "WARC-Block-Digest: sha256:"
                   << sha256_hex(request_block) << "\r\n"
                   << "Content-Type: application/http; msgtype=request\r\n";

    if (!write_record(gz, request_fields.str(), request_block, error)) {
        gzclose(gz);
        return false;
    }

    std::string http_block = capture.status_line + "\r\n";
    http_block += capture.headers;
    if (!capture.headers.empty() && !ends_with(capture.headers, "\r\n")) {
        http_block += "\r\n";
    }
    http_block += "\r\n";
    http_block += capture.body;

    const std::string payload_digest = sha256_hex(capture.body);
    const std::string block_digest = sha256_hex(http_block);

    std::ostringstream response_fields;
    response_fields << "WARC-Type: response\r\n"
                    << "WARC-Target-URI: " << capture.requested_url << "\r\n"
                    << "WARC-Date: " << date << "\r\n"
                    << "WARC-Record-ID: <urn:uuid:" << uuid_v4() << ">\r\n"
                    << "WARC-Payload-Digest: sha256:" << payload_digest << "\r\n"
                    << "WARC-Block-Digest: sha256:" << block_digest << "\r\n"
                    << "Content-Type: application/http; msgtype=response\r\n";

    if (!write_record(gz, response_fields.str(), http_block, error)) {
        gzclose(gz);
        return false;
    }

    if (gzclose(gz) != Z_OK) {
        error = "gzip close failed for " + output.string();
        return false;
    }
    return true;
}

} // namespace arcbox
