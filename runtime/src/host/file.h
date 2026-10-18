#pragma once

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <vector>
#include <memory>
#include <mutex>
#include <system_error>

namespace cafe::host {

struct FileOptions {
    bool read = false;
    bool write = false;
    bool create = false;
    bool truncate = false;
};

class RandomAccessFile {
public:
    static std::shared_ptr<RandomAccessFile> open(const std::filesystem::path& path,
                                                   FileOptions options,
                                                   std::error_code& error);
    ~RandomAccessFile();

    uint64_t size(std::error_code& error) const;
    uint64_t read_at(uint64_t offset, void* buffer, uint64_t length);
    uint64_t write_at(uint64_t offset, const void* buffer, uint64_t length);
    bool flush();

private:
    RandomAccessFile(std::filesystem::path path, std::fstream stream, uint64_t size, bool cache_reads)
        : path_(std::move(path)), stream_(std::move(stream)), size_(size), cache_reads_(cache_reads) {}

    std::filesystem::path path_;
    mutable std::mutex mutex_;
    std::fstream stream_;
    uint64_t size_ = 0;
    bool cache_reads_ = false;
    uint64_t cache_offset_ = 0;
    std::vector<uint8_t> read_cache_;
    uint64_t last_read_end_ = 0;
    uint32_t sequential_reads_ = 0;
};

} // namespace cafe::host
