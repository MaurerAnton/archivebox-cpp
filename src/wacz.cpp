#include "arcbox/wacz.hpp"

#include <cstring>
#include <ctime>
#include <fstream>
#include <sstream>
#include <vector>

#include <zlib.h>

namespace arcbox {
namespace {

// One read/compress/write cycle at a time, so peak memory is two 64 KiB
// buffers no matter how large the member is.
constexpr std::size_t kChunk = 65536;
constexpr std::uint32_t kLocalSignature = 0x04034b50;
constexpr std::uint32_t kCentralSignature = 0x02014b50;
constexpr std::uint32_t kEndSignature = 0x06054b50;
constexpr std::uint64_t kMax32 = 0xffffffffull;

void put16(std::ostream& out, std::uint16_t value) {
    const unsigned char bytes[2] = {static_cast<unsigned char>(value & 0xff),
                                    static_cast<unsigned char>(value >> 8)};
    out.write(reinterpret_cast<const char*>(bytes), sizeof(bytes));
}

void put32(std::ostream& out, std::uint32_t value) {
    const unsigned char bytes[4] = {
        static_cast<unsigned char>(value & 0xff),
        static_cast<unsigned char>((value >> 8) & 0xff),
        static_cast<unsigned char>((value >> 16) & 0xff),
        static_cast<unsigned char>((value >> 24) & 0xff)};
    out.write(reinterpret_cast<const char*>(bytes), sizeof(bytes));
}

// Zip stores local time, so the members are stamped with whatever the host
// clock says; nothing in WACZ depends on it.
std::uint16_t dos_time() {
    const std::time_t now = std::time(nullptr);
    std::tm tm{};
    localtime_r(&now, &tm);
    return static_cast<std::uint16_t>((tm.tm_hour << 11) | (tm.tm_min << 5) |
                                      (tm.tm_sec / 2));
}

std::uint16_t dos_date() {
    const std::time_t now = std::time(nullptr);
    std::tm tm{};
    localtime_r(&now, &tm);
    const int year = tm.tm_year + 1900 - 1980;
    return static_cast<std::uint16_t>((year << 9) | ((tm.tm_mon + 1) << 5) |
                                      tm.tm_mday);
}

}  // namespace

ZipWriter::ZipWriter(const std::filesystem::path& path) {
    out_.open(path, std::ios::binary | std::ios::out | std::ios::trunc);
    if (!out_.is_open()) {
        failed_ = true;
        error_ = "cannot open " + path.string();
    }
}

ZipWriter::~ZipWriter() {
    if (!closed_) {
        close();
    }
}

bool ZipWriter::ok() const { return out_.is_open() && !failed_; }

void ZipWriter::patch_local_header(const Entry& entry) {
    const std::streampos here = out_.tellp();
    out_.seekp(static_cast<std::streamoff>(entry.offset + 14));
    put32(out_, entry.crc);
    put32(out_, static_cast<std::uint32_t>(entry.compressed));
    put32(out_, static_cast<std::uint32_t>(entry.uncompressed));
    out_.seekp(here);
}

bool ZipWriter::add_stream(const std::string& name,
                           std::istream& source,
                           bool store) {
    if (closed_ || failed_ || !out_.is_open()) {
        if (error_.empty()) {
            error_ = "zip writer is closed";
        }
        return false;
    }

    Entry entry;
    entry.name = name;
    entry.method = store ? 0 : 8;
    entry.offset = static_cast<std::uint64_t>(out_.tellp());

    put32(out_, kLocalSignature);
    put16(out_, 20);  // version needed
    put16(out_, 0);   // flags: sizes follow in the header, no data descriptor
    put16(out_, entry.method);
    put16(out_, dos_time());
    put16(out_, dos_date());
    put32(out_, 0);  // crc32, patched once the bytes are known
    put32(out_, 0);  // compressed size
    put32(out_, 0);  // uncompressed size
    put16(out_, static_cast<std::uint16_t>(name.size()));
    put16(out_, 0);
    out_.write(name.data(), static_cast<std::streamsize>(name.size()));

    std::vector<unsigned char> input(kChunk);
    std::vector<unsigned char> output(kChunk);
    z_stream zs{};
    bool compressed = false;
    if (!store) {
        if (deflateInit2(&zs, Z_DEFAULT_COMPRESSION, Z_DEFLATED, -15, 8,
                         Z_DEFAULT_STRATEGY) != Z_OK) {
            failed_ = true;
            error_ = "zlib init failed for " + name;
            return false;
        }
        compressed = true;
    }

    auto drain = [&]() {
        const std::size_t produced = output.size() - zs.avail_out;
        if (produced > 0) {
            out_.write(reinterpret_cast<const char*>(output.data()),
                       static_cast<std::streamsize>(produced));
            entry.compressed += produced;
        }
    };

    std::uint32_t crc = 0;
    bool ok = true;
    while (source.good() && ok) {
        source.read(reinterpret_cast<char*>(input.data()),
                    static_cast<std::streamsize>(kChunk));
        const std::streamsize got = source.gcount();
        if (got <= 0) {
            break;
        }
        crc = crc32(crc, input.data(), static_cast<uInt>(got));
        entry.uncompressed += static_cast<std::uint64_t>(got);
        if (store) {
            out_.write(reinterpret_cast<const char*>(input.data()), got);
            entry.compressed += static_cast<std::uint64_t>(got);
            if (!out_.good()) {
                ok = false;
            }
            continue;
        }
        zs.next_in = input.data();
        zs.avail_in = static_cast<uInt>(got);
        while (zs.avail_in > 0 && ok) {
            zs.next_out = output.data();
            zs.avail_out = static_cast<uInt>(output.size());
            const int rc = deflate(&zs, Z_NO_FLUSH);
            if (rc != Z_OK && rc != Z_STREAM_END) {
                ok = false;
                break;
            }
            drain();
            if (!out_.good()) {
                ok = false;
            }
        }
    }

    if (compressed) {
        if (ok) {
            int rc = Z_OK;
            do {
                zs.next_out = output.data();
                zs.avail_out = static_cast<uInt>(output.size());
                rc = deflate(&zs, Z_FINISH);
                drain();
                if (!out_.good()) {
                    ok = false;
                    break;
                }
            } while (rc == Z_OK);
            if (rc != Z_STREAM_END) {
                ok = false;
            }
        }
        deflateEnd(&zs);
    }

    if (!ok) {
        failed_ = true;
        error_ = "failed to write " + name;
        return false;
    }
    if (entry.compressed > kMax32 || entry.uncompressed > kMax32) {
        failed_ = true;
        error_ = name + " needs zip64, which this writer does not produce";
        return false;
    }

    entry.crc = crc;
    patch_local_header(entry);
    entries_.push_back(entry);
    return true;
}

bool ZipWriter::add_file(const std::string& name,
                         const std::filesystem::path& source,
                         bool store) {
    std::ifstream in(source, std::ios::binary);
    if (!in) {
        failed_ = true;
        error_ = "cannot read " + source.string();
        return false;
    }
    return add_stream(name, in, store);
}

bool ZipWriter::add_bytes(const std::string& name, const std::string& data) {
    std::istringstream in(data);
    return add_stream(name, in, false);
}

bool ZipWriter::close() {
    if (closed_) {
        return !failed_;
    }
    closed_ = true;
    if (failed_ || !out_.is_open()) {
        return false;
    }

    for (const Entry& entry : entries_) {
        if (entry.offset > kMax32) {
            failed_ = true;
            error_ = "archive needs zip64, which this writer does not produce";
            return false;
        }
    }
    if (entries_.size() > 0xffff) {
        failed_ = true;
        error_ = "too many members for a plain zip";
        return false;
    }

    const std::uint64_t central_offset =
        static_cast<std::uint64_t>(out_.tellp());
    if (central_offset > kMax32) {
        failed_ = true;
        error_ = "archive needs zip64, which this writer does not produce";
        return false;
    }

    for (const Entry& entry : entries_) {
        put32(out_, kCentralSignature);
        put16(out_, 20);  // version made by
        put16(out_, 20);  // version needed
        put16(out_, 0);   // flags
        put16(out_, entry.method);
        put16(out_, dos_time());
        put16(out_, dos_date());
        put32(out_, entry.crc);
        put32(out_, static_cast<std::uint32_t>(entry.compressed));
        put32(out_, static_cast<std::uint32_t>(entry.uncompressed));
        put16(out_, static_cast<std::uint16_t>(entry.name.size()));
        put16(out_, 0);  // extra
        put16(out_, 0);  // comment
        put16(out_, 0);  // disk number
        put16(out_, 0);  // internal attributes
        put32(out_, 0);  // external attributes
        put32(out_, static_cast<std::uint32_t>(entry.offset));
        out_.write(entry.name.data(), static_cast<std::streamsize>(
                                          entry.name.size()));
    }

    const std::uint64_t central_size =
        static_cast<std::uint64_t>(out_.tellp()) - central_offset;
    put32(out_, kEndSignature);
    put16(out_, 0);  // disk number
    put16(out_, 0);  // disk with the central directory
    put16(out_, static_cast<std::uint16_t>(entries_.size()));
    put16(out_, static_cast<std::uint16_t>(entries_.size()));
    put32(out_, static_cast<std::uint32_t>(central_size));
    put32(out_, static_cast<std::uint32_t>(central_offset));
    put16(out_, 0);  // comment length
    out_.flush();

    if (!out_.good()) {
        failed_ = true;
        error_ = "writing the central directory failed";
        return false;
    }
    return true;
}

}  // namespace arcbox
