#include "host/file.h"

#include <algorithm>
#include <limits>
#include <cstring>
#include <unordered_map>

namespace cafe::host {
namespace fs = std::filesystem;

std::shared_ptr<RandomAccessFile> RandomAccessFile::open(const fs::path& path,
                                                          FileOptions options,
                                                          std::error_code& error) {
    error.clear();
    if (options.create && !fs::exists(path, error)) {
        std::ofstream create(path, std::ios::binary);
        if (!create) {
            error = std::make_error_code(std::errc::io_error);
            return nullptr;
        }
    }
    if (error) return nullptr;

    const bool read_only = options.read && !options.write && !options.create && !options.truncate;
    static std::mutex pool_mutex;
    static std::unordered_map<fs::path, std::weak_ptr<RandomAccessFile>> read_pool;
    if (read_only) {
        std::lock_guard lock(pool_mutex);
        if (auto it = read_pool.find(path); it != read_pool.end()) {
            if (auto existing = it->second.lock()) return existing;
            read_pool.erase(it);
        }
    }

    std::ios::openmode mode = std::ios::binary;
    if (options.read || options.write) mode |= std::ios::in;
    if (options.write) mode |= std::ios::out;
    if (options.truncate) mode |= std::ios::trunc;
    std::fstream stream(path, mode);
    if (!stream) {
        error = fs::exists(path) ? std::make_error_code(std::errc::permission_denied)
                                 : std::make_error_code(std::errc::no_such_file_or_directory);
        return nullptr;
    }
    const uint64_t file_size = fs::file_size(path, error);
    if (error) return nullptr;
    auto result = std::shared_ptr<RandomAccessFile>(
        new RandomAccessFile(path, std::move(stream), file_size, read_only));
    if (read_only) {
        std::lock_guard lock(pool_mutex);
        read_pool[path] = result;
    }
    return result;
}

RandomAccessFile::~RandomAccessFile() = default;

uint64_t RandomAccessFile::size(std::error_code& error) const {
    error.clear();
    return size_;
}

uint64_t RandomAccessFile::read_at(uint64_t offset, void* buffer, uint64_t length) {
    std::lock_guard lock(mutex_);
    if (offset >= size_ || length == 0) return 0;
    length = std::min(length, size_ - offset);

    constexpr uint64_t kReadAhead = 2ull << 20;
    constexpr uint64_t kCacheRequest = 256ull << 10;
    constexpr uint64_t kAlign = 64ull << 10;
    if (cache_reads_ && length <= kCacheRequest) {
        const uint64_t cache_end = cache_offset_ + read_cache_.size();
        if (!read_cache_.empty() && offset >= cache_offset_ && offset + length <= cache_end) {
            std::memcpy(buffer, read_cache_.data() + (offset - cache_offset_), static_cast<size_t>(length));
            return length;
        }

        const uint64_t start = offset & ~(kAlign - 1);
        const uint64_t want = std::min(kReadAhead, size_ - start);
        read_cache_.resize(static_cast<size_t>(want));
        stream_.clear();
        stream_.seekg(static_cast<std::streamoff>(start), std::ios::beg);
        if (stream_) {
            stream_.read(reinterpret_cast<char*>(read_cache_.data()), static_cast<std::streamsize>(want));
            read_cache_.resize(static_cast<size_t>(stream_.gcount()));
            cache_offset_ = start;
            const uint64_t filled_end = cache_offset_ + read_cache_.size();
            if (offset >= cache_offset_ && offset + length <= filled_end) {
                std::memcpy(buffer, read_cache_.data() + (offset - cache_offset_), static_cast<size_t>(length));
                return length;
            }
        } else {
            read_cache_.clear();
        }
    }

    stream_.clear();
    stream_.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
    if (!stream_) return 0;
    const auto amount = static_cast<std::streamsize>(std::min<uint64_t>(
        length, static_cast<uint64_t>(std::numeric_limits<std::streamsize>::max())));
    stream_.read(static_cast<char*>(buffer), amount);
    return static_cast<uint64_t>(stream_.gcount());
}

uint64_t RandomAccessFile::write_at(uint64_t offset, const void* buffer, uint64_t length) {
    std::lock_guard lock(mutex_);
    read_cache_.clear();
    stream_.clear();
    stream_.seekp(static_cast<std::streamoff>(offset), std::ios::beg);
    if (!stream_) return 0;
    uint64_t done = 0;
    const auto* bytes = static_cast<const char*>(buffer);
    while (done < length) {
        const auto amount = static_cast<std::streamsize>(std::min<uint64_t>(
            length - done, static_cast<uint64_t>(std::numeric_limits<std::streamsize>::max())));
        stream_.write(bytes + done, amount);
        if (!stream_) return done;
        done += static_cast<uint64_t>(amount);
    }
    size_ = std::max(size_, offset + done);
    return done;
}

bool RandomAccessFile::flush() {
    std::lock_guard lock(mutex_);
    stream_.flush();
    return static_cast<bool>(stream_);
}

} // namespace cafe::host
