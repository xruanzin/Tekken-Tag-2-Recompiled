#include "host/file.h"

#include <algorithm>
#include <limits>

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
    return std::shared_ptr<RandomAccessFile>(new RandomAccessFile(path, std::move(stream)));
}

RandomAccessFile::~RandomAccessFile() = default;

uint64_t RandomAccessFile::size(std::error_code& error) const {
    return fs::file_size(path_, error);
}

uint64_t RandomAccessFile::read_at(uint64_t offset, void* buffer, uint64_t length) {
    std::lock_guard lock(mutex_);
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
    return done;
}

bool RandomAccessFile::flush() {
    std::lock_guard lock(mutex_);
    stream_.flush();
    return static_cast<bool>(stream_);
}

} // namespace cafe::host
