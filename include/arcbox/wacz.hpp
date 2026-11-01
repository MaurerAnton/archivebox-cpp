#pragma once

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iosfwd>
#include <string>
#include <vector>

namespace arcbox {

// Minimal ZIP writer used by the WACZ export. Members are streamed from disk
// in 64 KiB pieces and compressed with zlib, so exporting an archive never
// needs more memory than that.
//
// Zip64 is deliberately not implemented: when an export outgrows what a plain
// zip can address, close() fails instead of writing offsets nobody can read.
class ZipWriter {
public:
    explicit ZipWriter(const std::filesystem::path& path);
    ~ZipWriter();

    ZipWriter(const ZipWriter&) = delete;
    ZipWriter& operator=(const ZipWriter&) = delete;

    bool ok() const;
    // Copies the file into the archive. `store` keeps the bytes as they are,
    // which is what an already gzipped WARC wants.
    bool add_file(const std::string& name,
                  const std::filesystem::path& source,
                  bool store);
    bool add_bytes(const std::string& name, const std::string& data);
    // Writes the central directory.
    bool close();

    const std::string& error() const { return error_; }

private:
    struct Entry {
        std::string name;
        uint32_t crc = 0;
        uint64_t compressed = 0;
        uint64_t uncompressed = 0;
        uint64_t offset = 0;
        uint16_t method = 0;
    };

    bool add_stream(const std::string& name,
                    std::istream& source,
                    bool store);
    void patch_local_header(const Entry& entry);

    std::ofstream out_;
    std::vector<Entry> entries_;
    std::string error_;
    bool closed_ = false;
    bool failed_ = false;
};

}  // namespace arcbox
