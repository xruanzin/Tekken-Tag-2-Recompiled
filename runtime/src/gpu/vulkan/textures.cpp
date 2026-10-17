// Textures and samplers.
//
// A texture is a GPU image for one texture resource (the seven
// SQ_TEX_RESOURCE words). Its contents come from guest memory, detiled,
// or from a render target at the same address if the GPU wrote that last.
// GX2Invalidate (SURFACE_SYNC) marks textures whose memory the CPU wrote;
// a dirty texture reloads if the data's hash changed. Textures matching a
// replacement (gpu/replacements.h) show its RGBA8 contents instead.

#include "gpu/vulkan/renderer.h"

#include "gpu/latte.h"
#include "gpu/replacements.h"
#include "gpu/tiling.h"
#include "gx2/internal.h"

#include "cafe/runtime.h"
#include "host/platform.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <fstream>
#include <future>
#include <functional>
#include <iterator>
#include <mutex>
#include <unordered_map>
#include <memory>
#include <thread>
#include <set>
#include <string>

#include <openssl/evp.h>

namespace cafe::gpu::vk {
namespace {

using namespace latte;

uint64_t hash_words(const uint32_t* words, size_t count) {
    uint64_t h = 0xCBF29CE484222325ull;
    for (size_t i = 0; i < count; ++i) h = (h ^ words[i]) * 0x100000001B3ull;
    return h;
}

// Fast hash of guest data, 8 bytes at a time.
uint64_t hash_bytes(const uint8_t* data, size_t size) {
    uint64_t h = 0x9E3779B97F4A7C15ull ^ size;
    size_t i = 0;
    for (; i + 8 <= size; i += 8) {
        uint64_t v;
        std::memcpy(&v, data + i, 8);
        h = (h ^ (v * 0xBF58476D1CE4E5B9ull)) * 0x94D049BB133111EBull;
        h ^= h >> 29;
    }
    for (; i < size; ++i) h = (h ^ data[i]) * 0x100000001B3ull;
    return h;
}

constexpr uint32_t kTextureCacheMagic = 0x32585454u; // "TTX2"
constexpr uint32_t kTextureCacheVersion = 2;
constexpr uint64_t kMaxCachedTextureBytes = 64ull << 20;
constexpr uint64_t kMiB = 1ull << 20;
constexpr uint64_t kGiB = 1ull << 30;

uint64_t texture_ram_cache_budget() {
    const uint64_t total = host::system_memory_total_bytes();
    const uint64_t available = host::system_memory_available_bytes();

    uint64_t target = 64ull * kMiB;
    if (total >= 8ull * kGiB) target = 128ull * kMiB;
    if (total >= 12ull * kGiB) target = 256ull * kMiB;
    if (total >= 16ull * kGiB) target = 384ull * kMiB;
    if (total >= 24ull * kGiB) target = 512ull * kMiB;
    if (total >= 32ull * kGiB) target = 768ull * kMiB;
    if (total >= 48ull * kGiB) target = 1024ull * kMiB;

    // Keep a large safety margin for the title, driver and Windows. The cache
    // grows with capable systems but backs off automatically under pressure.
    if (available != 0) target = std::min<uint64_t>(target, std::max<uint64_t>(64ull * kMiB, available / 6));
    return target;
}

uint64_t texture_cache_writer_budget() {
    return std::clamp<uint64_t>(texture_ram_cache_budget() / 2, 64ull * kMiB, 256ull * kMiB);
}

class TextureRamCache {
public:
    TextureRamCache() : budget_(texture_ram_cache_budget()) {
        char info[160]{};
        std::snprintf(info, sizeof(info), "MEMORY texture_ram_cache=%lluMB texture_write_queue=%lluMB",
                      static_cast<unsigned long long>(budget_ >> 20),
                      static_cast<unsigned long long>(texture_cache_writer_budget() >> 20));
        cafe::diagnostic_trace(info);
    }

    std::shared_ptr<const std::vector<uint8_t>> find(uint64_t key, size_t expected) {
        std::lock_guard lock(mutex_);
        auto it = entries_.find(key);
        if (it == entries_.end()) return {};
        if (!it->second.payload || it->second.payload->size() != expected) {
            bytes_ -= it->second.payload ? it->second.payload->size() : 0;
            entries_.erase(it);
            return {};
        }
        it->second.stamp = ++clock_;
        return it->second.payload;
    }

    std::shared_ptr<const std::vector<uint8_t>> insert(uint64_t key, std::vector<uint8_t>&& payload) {
        auto shared = std::make_shared<const std::vector<uint8_t>>(std::move(payload));
        if (shared->empty() || shared->size() > kMaxCachedTextureBytes || budget_ == 0) return shared;

        std::lock_guard lock(mutex_);
        if (auto old = entries_.find(key); old != entries_.end()) {
            bytes_ -= old->second.payload ? old->second.payload->size() : 0;
            entries_.erase(old);
        }

        while (!entries_.empty() && bytes_ + shared->size() > budget_) {
            auto victim = entries_.begin();
            for (auto it = std::next(entries_.begin()); it != entries_.end(); ++it) {
                if (it->second.stamp < victim->second.stamp) victim = it;
            }
            bytes_ -= victim->second.payload ? victim->second.payload->size() : 0;
            entries_.erase(victim);
        }

        if (shared->size() <= budget_) {
            bytes_ += shared->size();
            entries_.emplace(key, Entry{shared, ++clock_});
        }
        return shared;
    }

private:
    struct Entry {
        std::shared_ptr<const std::vector<uint8_t>> payload;
        uint64_t stamp = 0;
    };

    std::mutex mutex_;
    std::unordered_map<uint64_t, Entry> entries_;
    uint64_t budget_ = 0;
    uint64_t bytes_ = 0;
    uint64_t clock_ = 0;
};

TextureRamCache& texture_ram_cache() {
    static TextureRamCache cache;
    return cache;
}

struct TextureCacheHeader {
    uint32_t magic = kTextureCacheMagic;
    uint32_t version = kTextureCacheVersion;
    uint64_t key = 0;
    uint64_t payload_size = 0;
    uint64_t payload_hash = 0;
};

bool texture_cache_enabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("TTT2_TEXTURE_CACHE");
        return value == nullptr || *value == '\0' || *value != '0';
    }();
    return enabled;
}

std::filesystem::path texture_cache_directory() {
    static const std::filesystem::path dir = [] {
        if (!texture_cache_enabled()) return std::filesystem::path();
        std::filesystem::path d = host::cache_directory() / "ttt2" / "textures-v2";
        std::error_code ec;
        std::filesystem::create_directories(d, ec);
        return ec ? std::filesystem::path() : d;
    }();
    return dir;
}

std::filesystem::path texture_cache_path(uint64_t key) {
    char name[32];
    std::snprintf(name, sizeof(name), "%016llx.bin", static_cast<unsigned long long>(key));
    return texture_cache_directory() / name;
}

bool load_texture_cache(uint64_t key, size_t expected, std::vector<uint8_t>& payload) {
    if (texture_cache_directory().empty() || expected == 0 || expected > kMaxCachedTextureBytes) return false;
    std::ifstream in(texture_cache_path(key), std::ios::binary | std::ios::ate);
    if (!in) return false;
    const auto size = in.tellg();
    if (size != static_cast<std::streamoff>(sizeof(TextureCacheHeader) + expected)) return false;
    in.seekg(0);
    TextureCacheHeader header{};
    in.read(reinterpret_cast<char*>(&header), sizeof(header));
    if (!in || header.magic != kTextureCacheMagic || header.version != kTextureCacheVersion ||
        header.key != key || header.payload_size != expected) {
        return false;
    }
    payload.resize(expected);
    in.read(reinterpret_cast<char*>(payload.data()), static_cast<std::streamsize>(expected));
    if (!in) return false;
    return hash_bytes(payload.data(), payload.size()) == header.payload_hash;
}

class TextureCacheWriter {
public:
    TextureCacheWriter() : thread_([this] { run(); }) {}
    ~TextureCacheWriter() {
        {
            std::lock_guard lock(mutex_);
            stop_ = true;
        }
        cv_.notify_all();
        if (thread_.joinable()) thread_.join();
    }

    void enqueue(uint64_t key, std::shared_ptr<const std::vector<uint8_t>> payload) {
        if (texture_cache_directory().empty() || !payload || payload->empty() ||
            payload->size() > kMaxCachedTextureBytes) {
            return;
        }
        std::lock_guard lock(mutex_);
        if (queued_bytes_ + payload->size() > texture_cache_writer_budget()) return;
        queued_bytes_ += payload->size();
        queue_.push_back({key, std::move(payload)});
        cv_.notify_one();
    }

private:
    struct Item {
        uint64_t key;
        std::shared_ptr<const std::vector<uint8_t>> payload;
    };

    void run() {
        host::set_thread_name("texture-cache");
        host::set_thread_low_priority();
        for (;;) {
            Item item{};
            {
                std::unique_lock lock(mutex_);
                cv_.wait(lock, [&] { return stop_ || !queue_.empty(); });
                if (stop_ && queue_.empty()) return;
                item = std::move(queue_.front());
                queued_bytes_ -= item.payload ? item.payload->size() : 0;
                queue_.pop_front();
            }
            const std::filesystem::path path = texture_cache_path(item.key);
            const std::filesystem::path temporary = path.string() + ".tmp";
            TextureCacheHeader header{};
            header.key = item.key;
            if (!item.payload) continue;
            header.payload_size = item.payload->size();
            header.payload_hash = hash_bytes(item.payload->data(), item.payload->size());
            {
                std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
                out.write(reinterpret_cast<const char*>(&header), sizeof(header));
                out.write(reinterpret_cast<const char*>(item.payload->data()),
                          static_cast<std::streamsize>(item.payload->size()));
                if (!out) {
                    std::error_code ec;
                    std::filesystem::remove(temporary, ec);
                    continue;
                }
            }
            std::error_code ec;
            std::filesystem::remove(path, ec);
            ec.clear();
            std::filesystem::rename(temporary, path, ec);
            if (ec) std::filesystem::remove(temporary, ec);
        }
    }

    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<Item> queue_;
    uint64_t queued_bytes_ = 0;
    bool stop_ = false;
    std::thread thread_;
};

TextureCacheWriter& texture_cache_writer() {
    static TextureCacheWriter writer;
    return writer;
}

class TextureWorkerPool {
public:
    TextureWorkerPool() {
        const uint32_t hardware = std::max(1u, std::thread::hardware_concurrency());
        const uint64_t total = host::system_memory_total_bytes();
        worker_count_ = 1;
        if (hardware >= 6 && total >= 8ull * kGiB) worker_count_ = 2;
        if (hardware >= 12 && total >= 12ull * kGiB) worker_count_ = 3;
        if (hardware >= 20 && total >= 24ull * kGiB) worker_count_ = 4;
        for (uint32_t i = 0; i < worker_count_; ++i) workers_.emplace_back([this] { run(); });

        char info[128]{};
        std::snprintf(info, sizeof(info), "MEMORY texture_workers=%u logical_cpus=%u",
                      worker_count_, hardware);
        cafe::diagnostic_trace(info);
    }
    ~TextureWorkerPool() {
        {
            std::lock_guard lock(mutex_);
            stop_ = true;
        }
        cv_.notify_all();
        for (std::thread& worker : workers_) if (worker.joinable()) worker.join();
    }
    std::future<void> submit(std::function<void()> fn) {
        std::packaged_task<void()> job(std::move(fn));
        std::future<void> result = job.get_future();
        {
            std::lock_guard lock(mutex_);
            jobs_.push_back(std::move(job));
        }
        cv_.notify_one();
        return result;
    }
    uint32_t worker_count() const { return worker_count_; }
private:
    void run() {
        host::set_thread_name("texture-detile");
        host::set_thread_low_priority();
        for (;;) {
            std::packaged_task<void()> job;
            {
                std::unique_lock lock(mutex_);
                cv_.wait(lock, [&] { return stop_ || !jobs_.empty(); });
                if (stop_ && jobs_.empty()) return;
                job = std::move(jobs_.front());
                jobs_.pop_front();
            }
            job();
        }
    }
    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<std::packaged_task<void()>> jobs_;
    std::vector<std::thread> workers_;
    uint32_t worker_count_ = 1;
    bool stop_ = false;
};

TextureWorkerPool& texture_worker_pool() {
    static TextureWorkerPool pool;
    return pool;
}

VkImageViewType view_type_of(uint32_t dim) {
    switch (dim) {
    case gx2::kDim1D: return VK_IMAGE_VIEW_TYPE_1D;
    case gx2::kDim3D: return VK_IMAGE_VIEW_TYPE_3D;
    case gx2::kDimCube: return VK_IMAGE_VIEW_TYPE_CUBE;
    case gx2::kDim1DArray: return VK_IMAGE_VIEW_TYPE_1D_ARRAY;
    case gx2::kDim2DArray: case gx2::kDim2DMsaaArray: return VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    default: return VK_IMAGE_VIEW_TYPE_2D;
    }
}

struct Resource {
    uint32_t dim, tile_mode, width, height, depth, format, base, mips, base_level, last_level, first_slice, last_slice;
    bool depth_tiles;
    uint32_t sel[4];
};

Resource decode(const uint32_t* w) {
    Resource r{};
    r.dim = w[0] & 7;
    r.tile_mode = (w[0] >> 3) & 0xF;
    r.depth_tiles = (w[0] >> 7) & 1;
    r.width = ((w[0] >> 19) & 0x1FFF) + 1;
    r.height = (w[1] & 0x1FFF) + 1;
    r.depth = ((w[1] >> 13) & 0x1FFF) + 1;
    r.format = texture_surface_format(w[1], w[4]);
    r.base = w[2] << 8;
    r.mips = w[3] << 8;
    for (int i = 0; i < 4; ++i) r.sel[i] = (w[4] >> (16 + 3 * i)) & 7;
    r.base_level = (w[4] >> 28) & 0xF;
    r.last_level = w[5] & 0xF;
    r.first_slice = (w[5] >> 4) & 0x1FFF;
    r.last_slice = (w[5] >> 17) & 0x1FFF;
    if (r.dim == gx2::kDim1D || r.dim == gx2::kDim1DArray) r.height = 1;
    if (r.dim == gx2::kDimCube) r.depth *= 6;
    if (r.dim == gx2::kDim1D || r.dim == gx2::kDim2D || r.dim == gx2::kDim2DMsaa) r.depth = 1;
    if (r.dim == gx2::kDim2DMsaa || r.dim == gx2::kDim2DMsaaArray) r.last_level = 0; // LAST_LEVEL holds the samples
    return r;
}

uint64_t texture_cache_key(uint64_t contents, const Resource& r, const gx2::Surface& surface, bool filtered) {
    uint32_t words[] = {
        static_cast<uint32_t>(contents), static_cast<uint32_t>(contents >> 32),
        r.format, r.width, r.height, r.depth, r.dim, r.tile_mode,
        r.depth_tiles ? 1u : 0u, r.base_level, r.last_level, r.first_slice, r.last_slice,
        r.base, r.mips,
        static_cast<uint32_t>(surface.mip_levels), static_cast<uint32_t>(surface.image),
        static_cast<uint32_t>(surface.mipmaps), static_cast<uint32_t>(surface.swizzle),
        static_cast<uint32_t>(surface.image_size), static_cast<uint32_t>(surface.image_size >> 32),
        static_cast<uint32_t>(surface.mipmap_size), static_cast<uint32_t>(surface.mipmap_size >> 32),
        static_cast<uint32_t>(surface.tile_mode), filtered ? 1u : 0u,
    };
    return hash_words(words, std::size(words));
}

// The GX2 surface a resource describes, laid out as GX2 laid it out.
gx2::Surface surface_of(const Resource& r) {
    gx2::Surface s{};
    s.dim = r.dim == gx2::kDim2DMsaa ? gx2::kDim2D : r.dim == gx2::kDim2DMsaaArray ? gx2::kDim2DArray : r.dim;
    s.width = r.width;
    s.height = r.height;
    s.depth = r.depth;
    s.mip_levels = r.last_level + 1;
    s.format = r.format;
    s.use = gx2::surface_use::kTexture | (r.depth_tiles ? gx2::surface_use::kDepthBuffer : 0);
    s.tile_mode = r.tile_mode;
    s.swizzle = gx2::is_macro_tiled(r.tile_mode) ? (r.base & 0x700) : 0;
    s.image = gx2::is_macro_tiled(r.tile_mode) ? r.base & ~0x700u : r.base;
    s.mipmaps = gx2::is_macro_tiled(r.tile_mode) ? r.mips & ~0x700u : r.mips;
    gx2::calc_surface_size_and_alignment(s);
    return s;
}

VkComponentSwizzle component(const FormatInfo& f, uint32_t sel) {
    static constexpr VkComponentSwizzle kRgba[] = {VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_G,
                                                   VK_COMPONENT_SWIZZLE_B, VK_COMPONENT_SWIZZLE_A};
    switch (sel) {
    case 0: case 1: case 2: case 3: {
        const VkComponentSwizzle m[] = {f.swizzle.r, f.swizzle.g, f.swizzle.b, f.swizzle.a};
        return m[sel] == VK_COMPONENT_SWIZZLE_IDENTITY ? kRgba[sel] : m[sel];
    }
    case 4: return VK_COMPONENT_SWIZZLE_ZERO;
    default: return VK_COMPONENT_SWIZZLE_ONE;
    }
}

} // namespace

Texture* Renderer::texture(const uint32_t words[7]) {
    const uint64_t key = hash_words(words, 7);
    auto& entry = textures_[key];
    if (entry && !std::equal(entry->words.begin(), entry->words.end(), words)) {
        fatal("GPU: texture key collision");
    }
    if (!entry) {
        const Resource r = decode(words);
        const FormatInfo f = color_format(r.format);
        if (f.format == VK_FORMAT_UNDEFINED) {
            static uint32_t reported = 0;
            if (reported != r.format) {
                std::fprintf(stderr, "ttt2: gpu: unsupported texture format 0x%03X (dim %u, %ux%u)\n", r.format, r.dim,
                             r.width, r.height);
            }
            reported = r.format;
            textures_.erase(key);
            return nullptr;
        }
        auto t = std::make_unique<Texture>();
        std::copy(words, words + 7, t->words.begin());
        create_texture_image(*t, 1);
        const gx2::Surface s = surface_of(r);
        t->memory[0][0] = s.image;
        t->memory[0][1] = s.image + s.image_size;
        t->memory[1][0] = s.mipmaps;
        t->memory[1][1] = s.mip_levels > 1 ? uint32_t{s.mipmaps} + s.mipmap_size : uint32_t{s.mipmaps};
        for (const auto& range : t->memory) {
            if (range[1] > range[0]) for_pages(range[0], range[1], [&](uint32_t page) { texture_pages_[page].push_back(t.get()); });
        }
        entry = std::move(t);
    }
    load_texture(*entry);
    return entry.get();
}

// (Re)creates a texture's image and view, `scale` times the resource's size.
void Renderer::create_texture_image(Texture& t, uint32_t scale) {
    if (t.image.image != VK_NULL_HANDLE) {
        submit(true); // recorded work may still use the old image
        vkDestroyImageView(ctx_.device, t.view, nullptr);
        destroy_image(t.image);
    }
    const Resource r = decode(t.words.data());
    const FormatInfo f = color_format(t.rgba ? fmt::k8_8_8_8 : r.format);
    t.scale = scale;
    const uint32_t layers = r.depth;
    t.image = create_image(f.format, VK_IMAGE_ASPECT_COLOR_BIT, r.width * scale, r.height * scale, layers,
                           r.last_level + 1,
                           VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                           view_type_of(r.dim));
    VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vci.image = t.image.image;
    vci.viewType = view_type_of(r.dim);
    vci.format = f.format;
    vci.components = {component(f, r.sel[0]), component(f, r.sel[1]), component(f, r.sel[2]), component(f, r.sel[3])};
    const uint32_t base_level = std::min(r.base_level, r.last_level);
    const uint32_t array_layers = r.dim == gx2::kDim3D ? 1 : layers;
    uint32_t first = std::min(r.first_slice, array_layers - 1);
    uint32_t count = std::min(r.last_slice, array_layers - 1) - first + 1;
    if (vci.viewType == VK_IMAGE_VIEW_TYPE_CUBE) {
        first = 0;
        count = 6;
    } else if (vci.viewType == VK_IMAGE_VIEW_TYPE_2D || vci.viewType == VK_IMAGE_VIEW_TYPE_1D ||
               vci.viewType == VK_IMAGE_VIEW_TYPE_3D) {
        count = 1;
    }
    vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, base_level, r.last_level - base_level + 1, first, count};
    VK_CHECK(vkCreateImageView(ctx_.device, &vci, nullptr, &t.view));
}

void Renderer::copy_depth_to_texture(const Target& src, Texture& t, uint32_t width, uint32_t height) {
    note_target_read(&src);
    const uint32_t bytes = src.image.format == VK_FORMAT_D16_UNORM ? 2 : 4;
    if (t.scale != src.scale) create_texture_image(t, src.scale);
    width = std::min({width * src.scale, src.image.width, t.image.width});
    height = std::min({height * src.scale, src.image.height, t.image.height});
    // Through a buffer in video memory: depth and colour images do not
    // copy directly. (The barriers order reuse across submissions.)
    const VkDeviceSize size = VkDeviceSize{width} * height * bytes;
    if (depth_staging_.size < size) {
        if (depth_staging_.buffer != VK_NULL_HANDLE) {
            submit(true);
            ctx_.destroy_buffer(depth_staging_);
        }
        depth_staging_ = ctx_.create_buffer(std::max<VkDeviceSize>(size, 64u << 20),
                                            VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    }
    end_rendering_and_barrier();
    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1};
    region.imageExtent = {width, height, 1};
    vkCmdCopyImageToBuffer(cmd(), src.image.image, VK_IMAGE_LAYOUT_GENERAL, depth_staging_.buffer, 1, &region);
    barrier();
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    vkCmdCopyBufferToImage(cmd(), depth_staging_.buffer, t.image.image, VK_IMAGE_LAYOUT_GENERAL, 1, &region);
    barrier();
    profile_mark("depth to texture", src.address, width, height);
}

static void report_target_texture_once(uint32_t format, uint32_t scale) {
    static bool reported = false;
    if (!reported) {
        std::fprintf(stderr, "ttt2: gpu: compressed texture (fmt 0x%03X) over an upscaled render target (x%u)\n", format,
                     scale);
    }
    reported = true;
}

// Render targets holding a texture's levels: written by the GPU after the
// CPU last wrote that memory, with elements of the texture's size. Elements
// are texels, or 4x4 blocks of a compressed texture: the title compresses
// textures on the GPU by rendering each block as one 64- or 128-bit texel
// (BC1 over R16G16B16A16, BC3 over R32G32B32A32), and the console reads the
// same memory either way. Returns false when level 0 has no such target.
bool Renderer::load_texture_from_targets(Texture& t) {
    const Resource r = decode(t.words.data());
    const FormatInfo f = color_format(r.format);
    const gx2::Surface s = surface_of(r);
    const uint32_t levels = std::min(r.last_level + 1, 14u);
    const Target* sources[14] = {};
    uint64_t newest = 0;
    for (uint32_t level = 0; level < levels; ++level) {
        uint32_t address = level == 0 ? uint32_t{s.image}
                                      : s.mipmaps + (level > 1 ? uint32_t{s.mip_level_offset[level - 1]} : 0u);
        if (gx2::is_macro_tiled(r.tile_mode)) address &= ~0x700u;
        const Target* src = find_target(address, 0, false);
        if (src == nullptr || src->written <= src->overwritten) {
            if (level == 0) return false;
            continue;
        }
        if (color_format(src->format).bytes != f.bytes) {
            static std::set<uint64_t> reported;
            if (reported.insert(uint64_t{address} << 32 | r.format << 12 | src->format).second) {
                std::fprintf(stderr,
                             "ttt2: gpu: texture 0x%08X level %u (fmt 0x%03X, %ux%u) over a render target of format "
                             "0x%03X\n",
                             address, level, r.format, r.width, r.height, src->format);
            }
            if (level == 0) return false;
            continue;
        }
        sources[level] = src;
        note_target_read(src);
        newest = std::max(newest, src->written);
    }
    if (t.source == sources[0] && t.loaded >= newest && t.scale == sources[0]->scale) return true;
    // An upscaled target gives an upscaled texture; levels of another scale
    // than level 0 are left out.
    const uint32_t scale = sources[0]->scale;
    if (scale != 1 && f.compressed) {
        report_target_texture_once(r.format, scale);
        return false;
    }
    if (t.scale != scale) create_texture_image(t, scale);
    const uint32_t block = f.compressed ? 4 : 1;
    end_rendering_and_barrier();
    for (uint32_t level = 0; level < levels; ++level) {
        const Target* src = sources[level];
        if (src == nullptr || src->scale != scale) continue;
        const uint32_t width = std::max(1u, r.width >> level) * scale, height = std::max(1u, r.height >> level) * scale;
        // In source texels, which are the destination's blocks.
        VkImageCopy region{};
        region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, 0, 1};
        region.extent = {std::min((width + block - 1) / block, src->image.width),
                         std::min((height + block - 1) / block, src->image.height), 1};
        vkCmdCopyImage(cmd(), src->image.image, VK_IMAGE_LAYOUT_GENERAL, t.image.image, VK_IMAGE_LAYOUT_GENERAL, 1,
                       &region);
    }
    barrier();
    profile_mark("texture from target", sources[0]->address, r.width * scale, r.height * scale);
    t.source = sources[0];
    t.loaded = stamp();
    t.dirty = false;
    return true;
}

void Renderer::load_texture(Texture& t) {
    const Resource r = decode(t.words.data());
    const FormatInfo f = color_format(r.format);
    // A depth buffer read as a texture.
    const uint32_t hw = r.format & 0x3F;
    if (hw == fmt::k8_24 || hw == fmt::k8_24Float || hw == fmt::k32Float || hw == fmt::k16 ||
        hw == fmt::kX24_8_32Float) {
        const uint32_t depth_address = gx2::is_macro_tiled(r.tile_mode) ? r.base & ~0x700u : r.base;
        if (const Target* src = find_target(depth_address, 0, true); src && src->written > src->overwritten) {
            const bool compatible = (hw == fmt::k16) == (src->image.format == VK_FORMAT_D16_UNORM) &&
                                    hw != fmt::kX24_8_32Float;
            if (!compatible) {
                std::fprintf(stderr, "ttt2: gpu: texture format 0x%03X over a depth buffer of format 0x%03X\n",
                             r.format, src->format);
                return;
            }
            if (t.source != src || t.loaded < src->written) {
                copy_depth_to_texture(*src, t, r.width, r.height);
                t.source = src;
                t.loaded = stamp();
                t.dirty = false;
            }
            return;
        }
    }
    // Render targets at the texture's address, written after the CPU last
    // wrote the memory, hold the contents.
    if (load_texture_from_targets(t)) return;
    if (t.replacement >= 0 && t.replacement_generation != gpu::texture_replacements_generation()) {
        t.dirty = true; // shows a replacement whose contents changed
        t.hash = 0;
    }
    if (!t.dirty && t.source == nullptr) return;
    // A replay renders the frame's data again: what the CPU wrote since is the next frame's.
    if (replaying_ && t.loaded != 0) return;

    const gx2::Surface s = surface_of(r);
    if (!guest_memory_committed(s.image, s.image_size) ||
        (s.mip_levels > 1 && !guest_memory_committed(s.mipmaps, s.mipmap_size))) {
        t.dirty = false;
        return;
    }
    uint64_t h = hash_bytes(guest_pointer(s.image), s.image_size);
    if (const char* w = std::getenv("TTT2_WATCH")) {
        if (static_cast<uint32_t>(std::strtoul(w, nullptr, 16)) == s.image) {
            std::fprintf(stderr, "watch: texture load 0x%08X hash %016llx (frame %llu)\n", uint32_t{s.image},
                         static_cast<unsigned long long>(h), static_cast<unsigned long long>(frame_number_));
        }
    }
    if (s.mip_levels > 1) h ^= hash_bytes(guest_pointer(s.mipmaps), s.mipmap_size) * 31;
    t.dirty = false;
    if (h == t.hash && t.source == nullptr && t.loaded != 0 && t.scale == 1) return;
    if (t.scale != 1) create_texture_image(t, 1);
    t.hash = h;
    t.source = nullptr;
    t.loaded = stamp();

    const auto texture_prepare_started = std::chrono::steady_clock::now();
    const auto report_texture_prepare = [&](const char* kind) {
        const double ms = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - texture_prepare_started).count();
        static const bool trace_loads = std::getenv("TTT2_TRACE_TEXTURE_LOADS") != nullptr;
        if (trace_loads || ms >= 25.0) {
            std::fprintf(stderr,
                         "ttt2: gpu: texture %s 0x%08X fmt 0x%03X %ux%u levels %u prepared in %.2f ms\n",
                         kind, uint32_t{s.image}, r.format, r.width, r.height, uint32_t{s.mip_levels}, ms);
        }
    };

    const uint32_t first_non_macro = (s.swizzle >> 16) & 0xFF;
    const uint32_t block = f.compressed ? 4 : 1;
    const auto tiled_level = [&](uint32_t level) {
        const gx2::SurfaceInfo li = gx2::surface_info(s, level);
        uint32_t level_address = level == 0 ? uint32_t{s.image} : s.mipmaps + (level > 1 ? uint32_t{s.mip_level_offset[level - 1]} : 0u);
        const uint32_t swizzle = gx2::is_macro_tiled(li.tile_mode) && level < first_non_macro ? (s.swizzle & 0x700) : 0;
        TiledLevel tl{};
        tl.data = guest_pointer(level_address);
        tl.base256b = (level_address | swizzle) >> 8;
        tl.tile_mode = li.tile_mode;
        tl.bpp = li.bpp;
        tl.pitch = li.pitch;
        tl.height = li.height;
        tl.slices = std::max(li.depth, 1u);
        tl.samples = 1;
        tl.depth = r.depth_tiles;
        return tl;
    };
    // Level 0 in rows, for the replacements and TTT2_TRACE_TEXTURES (which
    // prints the MD5 a replacement names).
    static const bool trace = std::getenv("TTT2_TRACE_TEXTURES") != nullptr;
    std::array<uint8_t, 16> md5{};
    std::vector<uint8_t> prepared_level0;
    const auto& replacements = gpu::texture_replacements();
    const bool candidate = r.dim == gx2::kDim2D && r.depth == 1 &&
                           std::any_of(replacements.begin(), replacements.end(), [&](const gpu::TextureReplacement& c) {
                               return c.width == r.width && c.height == r.height && c.format == hw;
                           });
    if (candidate || (trace && hw != fmt::k8)) {
        const uint32_t ew = (r.width + block - 1) / block, eh = (r.height + block - 1) / block;
        prepared_level0.resize(size_t{ew} * eh * f.bytes);
        detile(tiled_level(0), ew, eh, 0, 1, prepared_level0.data());
        unsigned int length = 0;
        EVP_Digest(prepared_level0.data(), prepared_level0.size(), md5.data(), &length, EVP_md5(), nullptr);
        if (trace) {
            static std::set<std::array<uint8_t, 16>> seen;
            if (seen.insert(md5).second) {
                char hex[33];
                for (int i = 0; i < 16; ++i) std::snprintf(hex + 2 * i, 3, "%02x", md5[i]);
                std::fprintf(stderr, "ttt2: texture 0x%08X %ux%u format 0x%03X levels %u md5 %s (frame %llu)\n",
                             uint32_t{s.image}, r.width, r.height, r.format, uint32_t{s.mip_levels}, hex,
                             static_cast<unsigned long long>(frame_number_));
            }
        }
    }
    t.replacement = -1;
    for (size_t i = 0; candidate && i < replacements.size(); ++i) {
        const gpu::TextureReplacement& c = replacements[i];
        if (c.width == r.width && c.height == r.height && c.format == hw && c.md5 == md5) t.replacement = static_cast<int>(i);
    }
    const std::vector<uint8_t>* replaced = nullptr;
    if (t.replacement >= 0) {
        t.replacement_generation = gpu::texture_replacements_generation();
        replaced = replacements[t.replacement].contents();
        if (replaced && replaced->size() != size_t{r.width} * r.height * 4) replaced = nullptr;
    }
    if ((replaced != nullptr) != t.rgba) {
        t.rgba = replaced != nullptr;
        create_texture_image(t, 1);
    }
    end_rendering_and_barrier();
    if (replaced) {
        // Level 0 as given, each smaller level the average of the one above.
        std::vector<uint8_t> level(*replaced);
        uint32_t width = r.width, height = r.height;
        for (uint32_t i = 0;; ++i) {
            VkDeviceSize offset = 0;
            std::memcpy(upload(level.size(), 16, offset), level.data(), level.size());
            VkBufferImageCopy region{};
            region.bufferOffset = offset;
            region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, i, 0, 1};
            region.imageExtent = {width, height, 1};
            vkCmdCopyBufferToImage(cmd(), ring_.buffer(), t.image.image, VK_IMAGE_LAYOUT_GENERAL, 1, &region);
            if (i + 1 >= s.mip_levels || (width == 1 && height == 1)) break;
            const uint32_t w = std::max(1u, width / 2), h = std::max(1u, height / 2);
            std::vector<uint8_t> smaller(size_t{w} * h * 4);
            for (uint32_t y = 0; y < h; ++y) {
                for (uint32_t x = 0; x < w; ++x) {
                    for (uint32_t k = 0; k < 4; ++k) {
                        uint32_t sum = 0;
                        for (uint32_t dy = 0; dy < 2; ++dy) {
                            for (uint32_t dx = 0; dx < 2; ++dx) {
                                const uint32_t sx = std::min(width - 1, 2 * x + dx), sy = std::min(height - 1, 2 * y + dy);
                                sum += level[(size_t{sy} * width + sx) * 4 + k];
                            }
                        }
                        smaller[(size_t{y} * w + x) * 4 + k] = static_cast<uint8_t>((sum + 2) / 4);
                    }
                }
            }
            level = std::move(smaller);
            width = w;
            height = h;
        }
        barrier();
        profile_mark("texture replaced", uint32_t{s.image}, r.width, r.height);
        report_texture_prepare("replacement");
        return;
    }
    const gpu::PlaneFilter& plane_filter = gpu::plane_filter();

    struct PreparedLevel {
        uint32_t width = 0, height = 0, ew = 0, eh = 0, slices = 0;
        size_t bytes = 0;
        std::vector<uint8_t> data;
    };
    std::vector<PreparedLevel> linear(s.mip_levels);
    std::vector<TiledLevel> tiled_levels(s.mip_levels);
    size_t total_linear_bytes = 0;
    for (uint32_t level = 0; level < s.mip_levels; ++level) {
        // Address-library queries stay on the renderer thread. The workers
        // only consume immutable TiledLevel descriptors and guest bytes.
        tiled_levels[level] = tiled_level(level);
        PreparedLevel& p = linear[level];
        p.width = std::max(1u, r.width >> level);
        p.height = std::max(1u, r.height >> level);
        p.ew = (p.width + block - 1) / block;
        p.eh = (p.height + block - 1) / block;
        p.slices = r.dim == gx2::kDim3D ? std::max(1u, r.depth >> level) : r.depth;
        p.bytes = size_t{p.ew} * p.eh * p.slices * f.bytes;
        total_linear_bytes += p.bytes;
    }

    const bool filtered_level0 = hw == fmt::k8 && !linear.empty() && linear[0].slices == 1 && plane_filter;
    const uint64_t cache_key = texture_cache_key(h, r, s, filtered_level0);
    std::shared_ptr<const std::vector<uint8_t>> cached_linear;
    const bool cache_eligible = total_linear_bytes != 0 && total_linear_bytes <= kMaxCachedTextureBytes &&
                                t.replacement < 0;
    if (cache_eligible) {
        cached_linear = texture_ram_cache().find(cache_key, total_linear_bytes);
        if (!cached_linear) {
            std::vector<uint8_t> disk_payload;
            if (load_texture_cache(cache_key, total_linear_bytes, disk_payload)) {
                cached_linear = texture_ram_cache().insert(cache_key, std::move(disk_payload));
            }
        }
    }
    const bool cache_hit = static_cast<bool>(cached_linear);

    if (!cache_hit) {
        // Large mip levels use a persistent low-priority pool, avoiding
        // std::async thread creation spikes during gameplay.
        std::vector<std::future<void>> futures;
        futures.reserve(std::min<size_t>(linear.size(), texture_worker_pool().worker_count()));
        const auto prepare = [&](uint32_t level) {
            PreparedLevel& p = linear[level];
            p.data.resize(p.bytes);
            if (level == 0 && !prepared_level0.empty() && prepared_level0.size() == p.bytes) {
                std::memcpy(p.data.data(), prepared_level0.data(), p.bytes);
            } else {
                detile(tiled_levels[level], p.ew, p.eh, 0, p.slices, p.data.data());
            }
            if (hw == fmt::k8_24) {
                auto* v = reinterpret_cast<uint32_t*>(p.data.data());
                for (size_t i = 0; i < p.bytes / 4; ++i) {
                    const float depth = static_cast<float>(v[i] & 0xFFFFFF) / 16777215.0f;
                    std::memcpy(&v[i], &depth, 4);
                }
            }
        };

        constexpr size_t kParallelMipThreshold = 256u << 10;
        uint32_t async_jobs = 0;
        const uint32_t max_async_jobs = texture_worker_pool().worker_count();
        for (uint32_t level = 0; level < s.mip_levels; ++level) {
            if (linear[level].bytes >= kParallelMipThreshold && async_jobs < max_async_jobs) {
                futures.push_back(texture_worker_pool().submit([&, level] { prepare(level); }));
                ++async_jobs;
            } else {
                prepare(level);
            }
        }
        for (auto& future : futures) future.get();

        if (filtered_level0 && !linear[0].data.empty()) {
            plane_filter(uint32_t{s.image}, linear[0].ew, linear[0].eh, linear[0].data.data());
        }
    }

    end_rendering_and_barrier();
    size_t linear_offset = 0;
    for (uint32_t level = 0; level < s.mip_levels; ++level) {
        PreparedLevel& p = linear[level];
        VkDeviceSize offset = 0;
        uint8_t* staging = upload(p.bytes, 16, offset);
        const uint8_t* source = cache_hit ? cached_linear->data() + linear_offset : p.data.data();
        std::memcpy(staging, source, p.bytes);

        VkBufferImageCopy region{};
        region.bufferOffset = offset;
        region.bufferRowLength = p.ew * block;
        region.bufferImageHeight = p.eh * block;
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, 0, r.dim == gx2::kDim3D ? 1 : p.slices};
        region.imageExtent = {p.width, p.height, r.dim == gx2::kDim3D ? p.slices : 1};
        vkCmdCopyBufferToImage(cmd(), ring_.buffer(), t.image.image, VK_IMAGE_LAYOUT_GENERAL, 1, &region);
        linear_offset += p.bytes;
    }
    barrier();

    if (cache_eligible && !cache_hit) {
        std::vector<uint8_t> payload(total_linear_bytes);
        size_t cursor = 0;
        for (const PreparedLevel& p : linear) {
            std::memcpy(payload.data() + cursor, p.data.data(), p.bytes);
            cursor += p.bytes;
        }
        auto shared_payload = texture_ram_cache().insert(cache_key, std::move(payload));
        texture_cache_writer().enqueue(cache_key, std::move(shared_payload));
    }
    if (cache_hit && std::getenv("TTT2_TRACE_TEXTURE_LOADS")) {
        std::fprintf(stderr, "ttt2: gpu: texture cache HIT 0x%08X %ux%u levels %u (%zu bytes)\n",
                     uint32_t{s.image}, r.width, r.height, uint32_t{s.mip_levels}, total_linear_bytes);
    }
    profile_mark("texture upload", uint32_t{s.image}, r.width, r.height);
    report_texture_prepare("upload");
    // TTT2_DUMP_TEXTURES=<directory>: every texture loaded from memory, as PNG.
    static const char* dump = std::getenv("TTT2_DUMP_TEXTURES");
    if (dump && r.dim == gx2::kDim2D) {
        Image rgba = create_image(VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_ASPECT_COLOR_BIT, r.width, r.height, 1, 1,
                                  VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_IMAGE_VIEW_TYPE_2D);
        VkImageBlit blit{};
        blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        blit.srcOffsets[1] = {static_cast<int32_t>(r.width), static_cast<int32_t>(r.height), 1};
        blit.dstSubresource = blit.srcSubresource;
        blit.dstOffsets[1] = blit.srcOffsets[1];
        vkCmdBlitImage(cmd(), t.image.image, VK_IMAGE_LAYOUT_GENERAL, rgba.image, VK_IMAGE_LAYOUT_GENERAL, 1, &blit,
                       VK_FILTER_NEAREST);
        char name[128];
        std::snprintf(name, sizeof(name), "/tex_%08X_%03X_%ux%u_t%u.png", uint32_t{s.image}, r.format, r.width, r.height,
                      r.tile_mode);
        save_image(rgba, std::string(dump) + name);
        destroy_image(rgba);
    }
}

void Renderer::mark_textures_dirty(uint32_t address, uint64_t end) {
    if (end - address > (64u << 20)) { // huge: every texture
        for (auto& [key, t] : textures_) {
            for (const auto& range : t->memory) {
                if (range[0] < end && range[1] > address) t->dirty = true;
            }
        }
        return;
    }
    for_pages(address, end, [&](uint32_t page) {
        const auto it = texture_pages_.find(page);
        if (it == texture_pages_.end()) return;
        for (Texture* t : it->second) {
            for (const auto& range : t->memory) {
                if (range[0] < end && range[1] > address) t->dirty = true;
            }
        }
    });
}

void Renderer::invalidate(uint32_t address, uint32_t size, uint32_t coherency_flags) {
    static const uint32_t watch = [] {
        const char* w = std::getenv("TTT2_WATCH");
        return w ? static_cast<uint32_t>(std::strtoul(w, nullptr, 16)) : 0u;
    }();
    if (watch) {
        if (address < uint64_t{watch} + 0x30000 && uint64_t{address} + size > watch) {
            std::fprintf(stderr, "watch: surface sync 0x%08X+0x%X flags 0x%08X (frame %llu)\n", address, size,
                         coherency_flags, static_cast<unsigned long long>(frame_number_));
        }
    }
    // A texture cache invalidation: textures loaded from this memory check
    // whether it changed (the DMA engine or the CPU may have written it).
    if (size == 0 || !(coherency_flags & (1u << 23))) return;
    // Titles flush the whole cache every frame; CPU writes come through
    // cpu_wrote(), so only a ranged invalidation suggests changed data.
    if (size >= 0x80000000u) return;
    mark_textures_dirty(address, uint64_t{address} + size);
}

void Renderer::cpu_wrote(uint32_t address, uint32_t size) {
    if (frames_per_frame_ > 1) forget_snapshots(address, size);
    const uint64_t end = uint64_t{address} + size;
    static const uint32_t watch = [] {
        const char* w = std::getenv("TTT2_WATCH");
        return w ? static_cast<uint32_t>(std::strtoul(w, nullptr, 16)) : 0u;
    }();
    if (watch && address < uint64_t{watch} + 0x30000 && end > watch) {
        std::fprintf(stderr, "watch: cpu wrote 0x%08X-0x%08X (frame %llu)\n", address, static_cast<uint32_t>(end),
                     static_cast<unsigned long long>(frame_number_));
    }
    if (size == 0) return;
    mark_textures_dirty(address, end);
    // The CPU's data is newer than a render target over the same memory.
    // (A flush of everything says nothing about render targets.)
    if (size < 0x10000000) {
        uint64_t now = 0;
        for (auto& t : targets_) {
            if (t->address < end && t->end > address) t->overwritten = now ? now : (now = stamp());
        }
    }
    if (end - address > (64u << 20)) {
        programs_.clear();
        program_pages_.clear();
        return;
    }
    for_pages(address, end, [&](uint32_t page) {
        const auto pages = program_pages_.find(page);
        if (pages == program_pages_.end()) return;
        for (uint64_t key : pages->second) {
            const uint32_t p_address = static_cast<uint32_t>(key >> 32), p_size = static_cast<uint32_t>(key);
            if (p_address < end && uint64_t{p_address} + p_size > address) programs_.erase(key);
        }
    });
}

// ---------------------------------------------------------------- samplers

VkSampler Renderer::sampler(const uint32_t words[3], const float border[4], bool compare) {
    uint32_t key_words[8] = {words[0], words[1], words[2], compare};
    const uint32_t border_type = (words[0] >> 22) & 3;
    if (border_type == 3) {
        for (int i = 0; i < 4; ++i) std::memcpy(&key_words[4 + i], &border[i], 4);
    }
    const uint64_t key = hash_words(key_words, 8);
    if (auto it = samplers_.find(key); it != samplers_.end()) return it->second;

    const auto address_mode = [](uint32_t clamp) {
        switch (clamp) {
        case 0: return VK_SAMPLER_ADDRESS_MODE_REPEAT;
        case 1: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
        case 2: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        case 3: case 5: case 7: return VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE;
        default: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
        }
    };
    const uint32_t w0 = words[0], w1 = words[1];
    VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sci.addressModeU = address_mode(w0 & 7);
    sci.addressModeV = address_mode((w0 >> 3) & 7);
    sci.addressModeW = address_mode((w0 >> 6) & 7);
    const uint32_t mag = (w0 >> 9) & 7, min = (w0 >> 12) & 7, mip = (w0 >> 17) & 3;
    sci.magFilter = (mag & 1) ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    sci.minFilter = (min & 1) ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    sci.mipmapMode = mip == 2 ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sci.minLod = (w1 & 0x3FF) / 64.0f;
    sci.maxLod = ((w1 >> 10) & 0x3FF) / 64.0f;
    if (mip == 0) sci.maxLod = sci.minLod; // no mipmapping
    sci.mipLodBias = static_cast<float>(static_cast<int32_t>(w1 << 0) >> 20) / 64.0f;
    const uint32_t aniso = (w0 >> 19) & 7;
    static const int forced_aniso = [] {
        const char* value = std::getenv("TTT2_ANISO");
        if (!value || !*value) return 0;
        const int parsed = std::atoi(value);
        return parsed == 2 || parsed == 4 || parsed == 8 || parsed == 16 ? parsed : 0;
    }();
    if (forced_aniso > 1 && ((mag & 1) || (min & 1))) {
        sci.anisotropyEnable = VK_TRUE;
        sci.maxAnisotropy = std::min<float>(static_cast<float>(forced_aniso),
                                            ctx_.properties.limits.maxSamplerAnisotropy);
    } else if ((mag >= 2 || min >= 2) && aniso > 0) {
        sci.anisotropyEnable = VK_TRUE;
        sci.maxAnisotropy = std::min<float>(static_cast<float>(1u << std::min(aniso, 4u)),
                                            ctx_.properties.limits.maxSamplerAnisotropy);
    }
    if (compare) {
        sci.compareEnable = VK_TRUE;
        sci.compareOp = static_cast<VkCompareOp>((w0 >> 26) & 7);
    }
    VkSamplerCustomBorderColorCreateInfoEXT custom{VK_STRUCTURE_TYPE_SAMPLER_CUSTOM_BORDER_COLOR_CREATE_INFO_EXT};
    switch (border_type) {
    case 0: sci.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK; break;
    case 1: sci.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK; break;
    case 2: sci.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE; break;
    default:
        if (ctx_.custom_border_color) {
            sci.borderColor = VK_BORDER_COLOR_FLOAT_CUSTOM_EXT;
            for (int i = 0; i < 4; ++i) custom.customBorderColor.float32[i] = border[i];
            sci.pNext = &custom;
        } else {
            sci.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
        }
        break;
    }
    VkSampler s;
    VK_CHECK(vkCreateSampler(ctx_.device, &sci, nullptr, &s));
    samplers_[key] = s;
    return s;
}

} // namespace cafe::gpu::vk
