// Renderer setup, command recording and synchronisation, scan-out and
// presentation.

#include "gpu/vulkan/renderer.h"

#include "gpu/vulkan/png.h"
#include "host/window.h"
#include "host/platform.h"

#include "cafe/runtime.h"

#include <shaderc/shaderc.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>

namespace cafe::gpu {

std::unique_ptr<Backend> make_vulkan_backend() { return std::make_unique<vk::Renderer>(); }

namespace vk {
namespace {

double now_seconds() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

constexpr VkDeviceSize kMiB = 1ull << 20;
constexpr VkDeviceSize kGiB = 1ull << 30;

VkDeviceSize adaptive_upload_ring_size(const Context& ctx) {
    const uint64_t total = host::system_memory_total_bytes();
    const uint64_t available = host::system_memory_available_bytes();

    VkDeviceSize target = 192ull * kMiB;
    if (total >= 12ull * kGiB) target = 256ull * kMiB;
    if (total >= 16ull * kGiB) target = 384ull * kMiB;
    if (total >= 24ull * kGiB) target = 512ull * kMiB;
    if (total >= 48ull * kGiB) target = 768ull * kMiB;

    // Integrated GPUs share system memory. Give them a healthy staging ring
    // without stealing so much RAM that the title or the driver starts paging.
    if (ctx.properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU) {
        target = std::min<VkDeviceSize>(target, 384ull * kMiB);
    }

    // Never consume more than roughly one eighth of currently available RAM.
    if (available != 0) {
        const VkDeviceSize pressure_cap =
            std::max<VkDeviceSize>(128ull * kMiB, (available / 8 / (32ull * kMiB)) * (32ull * kMiB));
        target = std::min(target, pressure_cap);
    }
    return std::clamp<VkDeviceSize>(target, 128ull * kMiB, 768ull * kMiB);
}

uint64_t device_local_heap_bytes(const Context& ctx) {
    uint64_t bytes = 0;
    for (uint32_t i = 0; i < ctx.memory.memoryHeapCount; ++i) {
        if (ctx.memory.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) {
            bytes += ctx.memory.memoryHeaps[i].size;
        }
    }
    return bytes;
}

} // namespace

Renderer::Renderer() {
    ctx_.init(host::vulkan_instance_extensions());
    {
        char gpu_info[512]{};
        std::snprintf(gpu_info, sizeof(gpu_info),
                      "GPU device=%s vendor=0x%04X type=%u driver=0x%08X",
                      ctx_.properties.deviceName, ctx_.properties.vendorID,
                      static_cast<unsigned>(ctx_.properties.deviceType),
                      ctx_.properties.driverVersion);
        cafe::diagnostic_trace(gpu_info);
    }
    init_formats(ctx_.physical);
    guest_.init(ctx_);
    const VkDeviceSize upload_ring_size = adaptive_upload_ring_size(ctx_);
    ring_.init(ctx_, upload_ring_size);
    {
        const uint64_t total = host::system_memory_total_bytes();
        const uint64_t available = host::system_memory_available_bytes();
        const uint64_t local_heap = device_local_heap_bytes(ctx_);
        char memory_info[320]{};
        std::snprintf(memory_info, sizeof(memory_info),
                      "MEMORY policy=automatic system_total=%lluMB system_available=%lluMB "
                      "device_local=%lluMB upload_ring=%lluMB gpu_type=%u",
                      static_cast<unsigned long long>(total >> 20),
                      static_cast<unsigned long long>(available >> 20),
                      static_cast<unsigned long long>(local_heap >> 20),
                      static_cast<unsigned long long>(upload_ring_size >> 20),
                      static_cast<unsigned>(ctx_.properties.deviceType));
        cafe::diagnostic_trace(memory_info);
    }
    VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    VK_CHECK(vkCreateSemaphore(ctx_.device, &sci, nullptr, &acquired_));

    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.queueFamilyIndex = ctx_.queue_family;
    pci.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT | VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    VK_CHECK(vkCreateCommandPool(ctx_.device, &pci, nullptr, &pool_));
    for (Slot& slot : slots_) {
        VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        cai.commandPool = pool_;
        cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cai.commandBufferCount = 1;
        VK_CHECK(vkAllocateCommandBuffers(ctx_.device, &cai, &slot.cmd));
    }
    VkSemaphoreTypeCreateInfo tci{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
    tci.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    VkSemaphoreCreateInfo tsci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    tsci.pNext = &tci;
    VK_CHECK(vkCreateSemaphore(ctx_.device, &tsci, nullptr, &timeline_));
    VkQueryPoolCreateInfo qci{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
    qci.queryType = VK_QUERY_TYPE_TIMESTAMP;
    qci.queryCount = 2 * kSlots;
    VK_CHECK(vkCreateQueryPool(ctx_.device, &qci, nullptr, &busy_pool_));

    if (const char* f = std::getenv("TTT2_TRACE_FRAME")) trace_frame_ = std::strtoull(f, nullptr, 10);
    // Render scale: TTT2_SCALE=1-4, else enough to cover the display
    // (2 on 1080p and 1440p, 3 on 4K).
    if (const char* sc = std::getenv("TTT2_SCALE")) {
        scale_ = static_cast<uint32_t>(std::clamp(std::atoi(sc), 1, 4));
    } else {
        uint32_t dw = 0, dh = 0;
        host::display_size(dw, dh);
        scale_ = std::clamp((dh + 719) / 720, 1u, 4u);
    }
    std::fprintf(stderr, "ttt2: GPU: rendering at %ux%u (scale %u)\n", 1280 * scale_, 720 * scale_, scale_);
    {
        char render_info[192]{};
        std::snprintf(render_info, sizeof(render_info),
                      "GPU render_size=%ux%u scale=%u",
                      1280 * scale_, 720 * scale_, scale_);
        cafe::diagnostic_trace(render_info);
    }
    compiler_ = shaderc_compiler_initialize();
    if (const char* value = std::getenv("TTT2_ASYNC_SHADERS")) {
        async_shaders_ = *value != '\0' && *value != '0';
    }
    if (async_shaders_) {
        const uint32_t logical = std::max(1u, std::thread::hardware_concurrency());
        // Do not oversubscribe 6C/12T systems: texture detile, pipeline
        // creation, renderer, AX and guest PPC work can all become active at
        // the same time. Two shader workers leave real CPU headroom for frame
        // pacing and audio. Larger hosts may use three.
        const uint32_t workers = logical >= 16 ? 3u : (logical >= 6 ? 2u : 1u);
        std::fprintf(stderr, "ttt2: GPU: asynchronous shader compilation enabled (%u workers)\n", workers);
        char shader_workers[128]{};
        std::snprintf(shader_workers, sizeof(shader_workers),
                      "GPU shader_compile_workers=%u logical_cpus=%u", workers, logical);
        cafe::diagnostic_trace(shader_workers);
        shader_compile_threads_.reserve(workers);
        for (uint32_t i = 0; i < workers; ++i) {
            shader_compile_threads_.emplace_back([this] { shader_worker_main(); });
        }
    }
    load_pipeline_cache();
    // Pipeline creation is a separate expensive driver operation. Keep it
    // away from the render thread even when shaderc itself is synchronous.
    async_pipelines_ = true;
    if (const char* value = std::getenv("TTT2_ASYNC_PIPELINES")) {
        async_pipelines_ = *value != '\0' && *value != '0';
    }
    // Minimize time-to-first-frame on a cold cache. Once the persistent
    // Vulkan cache is warm, later launches use fully optimized pipelines.
    // This is especially important for the zero-install dynamic Player,
    // where CPU translation is already part of cold-start latency.
    fast_pipelines_ = !pipeline_cache_warm_;
    if (const char* value = std::getenv("TTT2_FAST_PIPELINES")) {
        fast_pipelines_ = *value != '\0' && *value != '0';
    }
    skip_pending_compiles_ = async_shaders_ || async_pipelines_;
    if (const char* value = std::getenv("TTT2_SKIP_PENDING_COMPILES")) {
        skip_pending_compiles_ = *value != '\0' && *value != '0';
    }
    if (const char* value = std::getenv("TTT2_COMPILE_WAIT_BUDGET_MS")) {
        compile_wait_budget_max_ms_ = std::clamp(std::atof(value), 0.0, 16.0);
    }
    compile_wait_budget_ms_ = compile_wait_budget_max_ms_;
    cafe::diagnostic_trace(fast_pipelines_
                               ? "GPU pipeline mode=fast-create"
                               : "GPU pipeline mode=fully-optimized");
    cafe::diagnostic_trace(skip_pending_compiles_
                               ? "GPU pending shader/pipeline mode=nonblocking"
                               : "GPU pending shader/pipeline mode=blocking");
    // Upscaling every intermediate GX2 target is both expensive and unsafe
    // for several render-to-texture effects. In particular, textures that
    // reinterpret GPU-written effect memory assume native Wii U texel/block
    // dimensions. Keep intermediate half/quarter-resolution buffers native by
    // default whenever the final scene is upscaled; the environment variable
    // remains an explicit override for diagnostics.
    native_effect_buffers_ = scale_ > 1;
    if (const char* value = std::getenv("TTT2_NATIVE_EFFECT_BUFFERS")) {
        native_effect_buffers_ = *value != '\0' && *value != '0';
    }
    if (async_pipelines_) {
        std::fprintf(stderr, "ttt2: GPU: asynchronous Vulkan pipeline compilation enabled%s\n",
                     fast_pipelines_ ? " (fast-create)" : "");
        pipeline_compile_thread_ = std::thread([this] { pipeline_worker_main(); });
    }
    if (native_effect_buffers_ && scale_ > 1) {
        std::fprintf(stderr, "ttt2: GPU: intermediate effect buffers stay at native resolution\n");
    }
    surface_ = host::create_vulkan_surface(ctx_.instance);
    init_interpolation();
    start_presenter();
    window_start_ = now_seconds();
}

Renderer::~Renderer() {
    {
        std::lock_guard lock(shader_compile_mutex_);
        shader_compile_stop_ = true;
    }
    shader_compile_cv_.notify_all();
    for (std::thread& worker : shader_compile_threads_) {
        if (worker.joinable()) worker.join();
    }

    {
        std::lock_guard lock(pipeline_compile_mutex_);
        pipeline_compile_stop_ = true;
    }
    pipeline_compile_cv_.notify_all();
    if (pipeline_compile_thread_.joinable()) pipeline_compile_thread_.join();

    save_pipeline_cache();
    if (compiler_ != nullptr) shaderc_compiler_release(compiler_);
}

// ------------------------------------------------------------- commands

VkCommandBuffer Renderer::cmd() {
    if (!recording_) {
        Slot& slot = slots_[slot_];
        if (slot.value != 0) {
            wait_value(slot.value);
            // Its GPU execution time, for the statistics.
            uint64_t ts[2];
            if (vkGetQueryPoolResults(ctx_.device, busy_pool_, 2 * slot_, 2, sizeof ts, ts, 8, VK_QUERY_RESULT_64_BIT) ==
                    VK_SUCCESS &&
                ts[1] >= ts[0]) {
                const double busy = (ts[1] - ts[0]) * ctx_.properties.limits.timestampPeriod * 1e-9;
                busy_seconds_ += busy;
                static const double auto_profile_ms = [] {
                    const char* value = std::getenv("TTT2_AUTO_PROFILE_MS");
                    return value ? std::max(std::atof(value), 0.0) : 0.0;
                }();
                if (auto_profile_ms > 0.0 && busy * 1000.0 >= auto_profile_ms &&
                    profile_frame_ == UINT64_MAX && !profiling_ && !auto_profile_pending_) {
                    auto_profile_pending_ = true;
                    std::fprintf(stderr,
                                 "ttt2: gpu: slow submission %.2f ms; profiling the next title frame\n",
                                 busy * 1000.0);
                }
            }
        }
        cmd_ = slot.cmd;
        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        VK_CHECK(vkBeginCommandBuffer(cmd_, &bi));
        recording_ = true;
        vkCmdResetQueryPool(cmd_, busy_pool_, 2 * slot_, 2);
        vkCmdWriteTimestamp2(cmd_, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, busy_pool_, 2 * slot_);
    }
    return cmd_;
}

void Renderer::wait_value(uint64_t value) {
    uint64_t done = 0;
    VK_CHECK(vkGetSemaphoreCounterValue(ctx_.device, timeline_, &done));
    if (done >= value) return;
    const double begin = now_seconds();
    VkSemaphoreWaitInfo wi{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
    wi.semaphoreCount = 1;
    wi.pSemaphores = &timeline_;
    wi.pValues = &value;
    const VkResult r = vkWaitSemaphores(ctx_.device, &wi, 10'000'000'000ull);
    if (r != VK_SUCCESS) fatal("Vulkan: the GPU did not finish a submission (%d)", static_cast<int>(r));
    wait_seconds_ += now_seconds() - begin;
    ++waits_;
}

uint64_t Renderer::submit(bool wait) {
    if (recording_) {
        end_rendering();
        vkCmdWriteTimestamp2(cmd_, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, busy_pool_, 2 * slot_ + 1);
        VK_CHECK(vkEndCommandBuffer(cmd_));
        const uint64_t value = ++submitted_;
        VkTimelineSemaphoreSubmitInfo ti{VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
        ti.signalSemaphoreValueCount = 1;
        ti.pSignalSemaphoreValues = &value;
        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        si.pNext = &ti;
        si.commandBufferCount = 1;
        si.pCommandBuffers = &cmd_;
        si.signalSemaphoreCount = 1;
        si.pSignalSemaphores = &timeline_;
        {
            std::lock_guard lock(queue_mutex_);
            VK_CHECK(vkQueueSubmit(ctx_.queue, 1, &si, VK_NULL_HANDLE));
        }
        slots_[slot_].value = value;
        slot_ = (slot_ + 1) % kSlots;
        for (uint32_t c = 0; c < UploadRing::kChunks; ++c) {
            if (chunks_touched_ & (1u << c)) chunk_value_[c] = value;
        }
        chunks_touched_ = 0;
        recording_ = false;
        bound_pipeline_ = nullptr;
    }
    if (wait) wait_value(submitted_);
    return submitted_;
}

uint64_t Renderer::flush() { return submit(false); }

// From the command processor's retirement thread.
void Renderer::wait_for(uint64_t value) {
    VkSemaphoreWaitInfo wi{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
    wi.semaphoreCount = 1;
    wi.pSemaphores = &timeline_;
    wi.pValues = &value;
    const VkResult r = vkWaitSemaphores(ctx_.device, &wi, 10'000'000'000ull);
    if (r != VK_SUCCESS) fatal("Vulkan: the GPU did not finish a submission (%d)", static_cast<int>(r));
}

void Renderer::sync() { submit(true); }

void Renderer::barrier() {
    VkMemoryBarrier2 mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    constexpr VkPipelineStageFlags2 kFrameStages =
        VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT |
        VK_PIPELINE_STAGE_2_TRANSFER_BIT;
    mb.srcStageMask = kFrameStages;
    mb.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
    mb.dstStageMask = kFrameStages;
    mb.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
    VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    di.memoryBarrierCount = 1;
    di.pMemoryBarriers = &mb;
    vkCmdPipelineBarrier2(cmd(), &di);
}

uint8_t* Renderer::upload(VkDeviceSize size, VkDeviceSize alignment, VkDeviceSize& offset) {
    if (uint8_t* p = ring_.allocate(size, alignment, offset)) {
        chunks_touched_ |= 1u << ring_.chunk();
        return p;
    }
    // Bigger than a chunk (a capture or texture at a high render scale): a
    // run of consecutive chunks, once the GPU finished with all of them.
    if (size > ring_.chunk_size()) {
        const uint32_t count = static_cast<uint32_t>((size + ring_.chunk_size() - 1) / ring_.chunk_size());
        if (count > UploadRing::kChunks) {
            fatal("Vulkan: an upload of %llu bytes does not fit the upload ring", static_cast<unsigned long long>(size));
        }
        uint32_t first = ring_.next_chunk();
        if (first + count > UploadRing::kChunks) first = 0;
        bool touched = false;
        for (uint32_t c = first; c < first + count; ++c) touched |= (chunks_touched_ >> c) & 1;
        if (touched) submit(false);
        for (uint32_t c = first; c < first + count; ++c) {
            wait_value(chunk_value_[c]);
            chunks_touched_ |= 1u << c;
        }
        return ring_.take_span(first, count, size, offset);
    }
    // The next chunk, once the GPU finished with it (it rarely has not).
    const uint32_t next = ring_.next_chunk();
    if (chunks_touched_ & (1u << next)) submit(false); // the commands being recorded use it
    wait_value(chunk_value_[next]);
    ring_.advance();
    if (uint8_t* p = ring_.allocate(size, alignment, offset)) {
        chunks_touched_ |= 1u << ring_.chunk();
        return p;
    }
    fatal("Vulkan: an upload of %llu bytes does not fit an upload chunk", static_cast<unsigned long long>(size));
}

void Renderer::end_rendering() {
    if (!rendering_) return;
    vkCmdEndRendering(cmd_);
    rendering_ = false;
    bound_pipeline_ = nullptr;
    barrier();
}

void Renderer::end_rendering_and_barrier() {
    const bool was_rendering = rendering_;
    end_rendering();
    if (!was_rendering) barrier();
}

Image Renderer::create_image(VkFormat format, VkImageAspectFlags aspect, uint32_t width, uint32_t height,
                             uint32_t layers, uint32_t levels, VkImageUsageFlags usage, VkImageViewType view_type,
                             bool shared) {
    Image img;
    img.format = format;
    img.aspect = aspect;
    img.width = std::max(width, 1u);
    img.height = std::max(height, 1u);
    img.layers = std::max(layers, 1u);
    img.levels = std::max(levels, 1u);
    img.view_type = view_type;
    VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ici.imageType = view_type == VK_IMAGE_VIEW_TYPE_3D ? VK_IMAGE_TYPE_3D
                    : (view_type == VK_IMAGE_VIEW_TYPE_1D || view_type == VK_IMAGE_VIEW_TYPE_1D_ARRAY) ? VK_IMAGE_TYPE_1D
                                                                                                           : VK_IMAGE_TYPE_2D;
    if (view_type == VK_IMAGE_VIEW_TYPE_CUBE || view_type == VK_IMAGE_VIEW_TYPE_CUBE_ARRAY) {
        ici.flags |= VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
    }
    ici.format = format;
    ici.extent = {img.width, ici.imageType == VK_IMAGE_TYPE_1D ? 1 : img.height,
                  ici.imageType == VK_IMAGE_TYPE_3D ? img.layers : 1};
    ici.mipLevels = img.levels;
    ici.arrayLayers = ici.imageType == VK_IMAGE_TYPE_3D ? 1 : img.layers;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = usage;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    const uint32_t families[2] = {ctx_.queue_family, ctx_.present_family};
    if (shared && ctx_.present_family != UINT32_MAX && ctx_.present_family != ctx_.queue_family) {
        ici.sharingMode = VK_SHARING_MODE_CONCURRENT;
        ici.queueFamilyIndexCount = 2;
        ici.pQueueFamilyIndices = families;
    }
    VK_CHECK(vkCreateImage(ctx_.device, &ici, nullptr, &img.image));
    img.memory = ctx_.allocate_image_memory(img.image);

    VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vci.image = img.image;
    vci.viewType = view_type;
    vci.format = format;
    vci.subresourceRange = {aspect, 0, img.levels, 0, ici.arrayLayers};
    VK_CHECK(vkCreateImageView(ctx_.device, &vci, nullptr, &img.view));

    // Every image lives in the general layout.
    end_rendering();
    VkImageMemoryBarrier2 ib{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    ib.srcStageMask = VK_PIPELINE_STAGE_2_NONE;
    ib.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    ib.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
    ib.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    ib.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    ib.srcQueueFamilyIndex = ib.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    ib.image = img.image;
    ib.subresourceRange = vci.subresourceRange;
    VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    di.imageMemoryBarrierCount = 1;
    di.pImageMemoryBarriers = &ib;
    vkCmdPipelineBarrier2(cmd(), &di);
    return img;
}

void Renderer::destroy_image(Image& img) {
    // Only for images no recorded command uses.
    if (img.view) vkDestroyImageView(ctx_.device, img.view, nullptr);
    if (img.image) vkDestroyImage(ctx_.device, img.image, nullptr);
    if (img.memory) vkFreeMemory(ctx_.device, img.memory, nullptr);
    img = Image{};
}

// ------------------------------------------------------------- scan-out

void Renderer::copy_to_scan_buffer(const gx2::ColorBuffer& buffer, uint32_t scan_target) {
    const int index = scan_target == gx2::kScanDrc ? 1 : 0;
    Target* t = color_target(buffer);
    if (t == nullptr) return;
    const uint32_t width = std::min<uint32_t>(buffer.surface.width * t->scale, t->image.width);
    const uint32_t height = std::min<uint32_t>(buffer.surface.height * t->scale, t->image.height);
    Image& scan = scan_[index];
    if (scan.image == VK_NULL_HANDLE || scan.width != width || scan.height != height || scan.format != t->image.format) {
        if (scan.image != VK_NULL_HANDLE) {
            submit(true);
            destroy_image(scan);
        }
        scan = create_image(t->image.format, VK_IMAGE_ASPECT_COLOR_BIT, width, height, 1, 1,
                            VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                VK_IMAGE_USAGE_SAMPLED_BIT,
                            VK_IMAGE_VIEW_TYPE_2D);
    }
    end_rendering_and_barrier();
    VkImageCopy region{};
    region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.extent = {width, height, 1};
    vkCmdCopyImage(cmd(), t->image.image, VK_IMAGE_LAYOUT_GENERAL, scan.image, VK_IMAGE_LAYOUT_GENERAL, 1, &region);
    barrier();
    scan_valid_[index] = true;
    profile_mark("copy to scan buffer", t->address, width, height);
    if (tracing()) {
        std::fprintf(stderr, "trace: copy to scan %u from 0x%08X fmt 0x%03X %ux%u\n", scan_target, t->address, t->format,
                     width, height);
    }
}

// --------------------------------------------------------- presentation

// Saves an 8-bit RGBA/BGRA image as a PNG (debugging); waits for the GPU.
void Renderer::save_image(const Image& img, const std::string& path) {
    const bool bgra = img.format == VK_FORMAT_B8G8R8A8_UNORM || img.format == VK_FORMAT_B8G8R8A8_SRGB;
    const bool rgba = img.format == VK_FORMAT_R8G8B8A8_UNORM || img.format == VK_FORMAT_R8G8B8A8_SRGB;
    const bool depth = img.format == VK_FORMAT_D32_SFLOAT || img.format == VK_FORMAT_D32_SFLOAT_S8_UINT;
    if (!bgra && !rgba && !depth) return;
    const VkDeviceSize bytes = VkDeviceSize{img.width} * img.height * 4;
    VkDeviceSize offset = 0;
    upload(bytes, 16, offset);
    const uint8_t* data = ring_.mapped() + offset;
    end_rendering_and_barrier();
    VkBufferImageCopy region{};
    region.bufferOffset = offset;
    region.imageSubresource = {static_cast<VkImageAspectFlags>(depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT), 0, 0, 1};
    region.imageExtent = {img.width, img.height, 1};
    vkCmdCopyImageToBuffer(cmd(), img.image, VK_IMAGE_LAYOUT_GENERAL, ring_.buffer(), 1, &region);
    submit(true);
    std::vector<uint8_t> pixels(data, data + bytes);
    for (size_t i = 0; i < pixels.size(); i += 4) {
        if (depth) {
            float d;
            std::memcpy(&d, &pixels[i], 4);
            const uint8_t v = static_cast<uint8_t>(std::clamp(d, 0.0f, 1.0f) * 255.0f);
            pixels[i] = pixels[i + 1] = pixels[i + 2] = v;
        } else if (bgra) {
            std::swap(pixels[i], pixels[i + 2]);
        }
        pixels[i + 3] = 255;
    }
    write_png(path, pixels.data(), img.width, img.height);
}

// TTT2_CAPTURE=<directory>: every TTT2_CAPTURE_INTERVAL seconds (default
// 5) the TV and GamePad images are saved as PNG files. The render targets
// of a traced frame (TTT2_TRACE_FRAME/AT) are saved there too.
void Renderer::capture() {
    static const char* dir = [] {
        const char* d = std::getenv("TTT2_CAPTURE");
        return (d != nullptr && *d != '\0') ? d : nullptr;
    }();
    static const double interval = [] {
        const char* v = std::getenv("TTT2_CAPTURE_INTERVAL");
        return v ? std::atof(v) : 5.0;
    }();
    static double last = now_seconds();
    static uint32_t index = 0;
    if (dir != nullptr && tracing()) {
        for (const auto& t : targets_) {
            char name[96];
            std::snprintf(name, sizeof(name), "/frame%llu_%s_%08X_%03X_%ux%u.png",
                          static_cast<unsigned long long>(frame_number_), t->depth ? "depth" : "color", t->address,
                          t->format, t->pitch, t->height);
            save_image(t->image, std::string(dir) + name);
        }
    }
    if (dir == nullptr || now_seconds() - last < interval) return;
    last = now_seconds();
    for (int s = 0; s < 2; ++s) {
        if (!scan_valid_[s]) continue;
        char name[64];
        std::snprintf(name, sizeof(name), "/%s_%04u.png", s == 0 ? "tv" : "drc", index);
        save_image(scan_[s], std::string(dir) + name);
    }
    captured_frame_ = static_cast<int64_t>(frame_number_);
    captured_index_ = index;
    ++index;
    // TTT2_CAPTURE_SEQUENCE=<frames>: also every image shown for the next
    // frames, in the order shown, as seq_<index>_<frame>_<n>.png (the real
    // image is the last n of its frame).
    static const uint32_t sequence = [] {
        const char* v = std::getenv("TTT2_CAPTURE_SEQUENCE");
        return v ? static_cast<uint32_t>(std::atoi(v)) : 0u;
    }();
    if (sequence_left_ == 0 && sequence > 0) {
        sequence_left_ = sequence;
        sequence_frame_ = 0;
        sequence_index_ = captured_index_;
    }
}

void Renderer::save_sequence_image(uint32_t n) {
    char name[64];
    std::snprintf(name, sizeof(name), "/seq_%04u_%03u_%u.png", sequence_index_, sequence_frame_, n);
    save_image(scan_[0], std::string(std::getenv("TTT2_CAPTURE")) + name);
}

// ------------------------------------------------------------ profiling

void Renderer::profile_mark(const char* what, uint32_t a, uint32_t b, uint32_t c, uint32_t d) {
    if (!profiling_ || profile_marks_.size() >= 8191) return;
    vkCmdWriteTimestamp2(cmd(), VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, profile_pool_,
                         static_cast<uint32_t>(profile_marks_.size()));
    profile_marks_.push_back({what, a, b, c, d});
}

void Renderer::profile_report() {
    const uint32_t n = static_cast<uint32_t>(profile_marks_.size());
    std::vector<uint64_t> ts(n);
    if (n < 2 || vkGetQueryPoolResults(ctx_.device, profile_pool_, 0, n, n * 8, ts.data(), 8,
                                       VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT) != VK_SUCCESS) {
        return;
    }
    const double ns = ctx_.properties.limits.timestampPeriod;
    struct Item {
        double ms;
        uint32_t index;
    };
    std::vector<Item> items;
    std::map<std::string, std::pair<double, uint32_t>> by_kind;
    double total = 0;
    for (uint32_t i = 1; i < n; ++i) {
        // Marks in different submissions are not comparable when the GPU idled between them.
        const double ms = ts[i] >= ts[i - 1] ? (ts[i] - ts[i - 1]) * ns / 1e6 : 0.0;
        items.push_back({ms, i});
        auto& k = by_kind[profile_marks_[i].what];
        k.first += ms;
        k.second += 1;
        total += ms;
    }
    std::fprintf(stderr, "profile: frame %llu, %u operations, %.2f ms of GPU time\n",
                 static_cast<unsigned long long>(frame_number_), n - 1, total);
    for (const auto& [what, v] : by_kind) {
        std::fprintf(stderr, "profile:   %-28s %5u x  %7.3f ms\n", what.c_str(), v.second, v.first);
    }
    std::sort(items.begin(), items.end(), [](const Item& x, const Item& y) { return x.ms > y.ms; });
    for (size_t i = 0; i < std::min<size_t>(items.size(), 40); ++i) {
        const ProfileMark& m = profile_marks_[items[i].index];
        std::fprintf(stderr, "profile:   #%-4u %7.3f ms  %s 0x%08X 0x%08X 0x%08X 0x%08X\n",
                     items[i].index, items[i].ms, m.what, m.a, m.b, m.c, m.d);
    }
}

void Renderer::swap() {
    capture();
    show_frame();
    if (profiling_) {
        submit(true);
        profile_report();
        profiling_ = false;
    }
    if (auto_profile_pending_) {
        auto_profile_pending_ = false;
        profile_frame_ = frame_number_ + 1;
        if (profile_pool_ == VK_NULL_HANDLE) {
            VkQueryPoolCreateInfo qci{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
            qci.queryType = VK_QUERY_TYPE_TIMESTAMP;
            qci.queryCount = 8192;
            VK_CHECK(vkCreateQueryPool(ctx_.device, &qci, nullptr, &profile_pool_));
        }
        vkCmdResetQueryPool(cmd(), profile_pool_, 0, 8192);
        profile_marks_.clear();
        profiling_ = true;
        profile_mark("start");
    }
    static const double profile_at = [] {
        const char* v = std::getenv("TTT2_PROFILE_AT");
        return v ? std::atof(v) : -1.0;
    }();
    static const double profile_start = now_seconds();
    if (profile_at >= 0 && profile_frame_ == UINT64_MAX && now_seconds() - profile_start >= profile_at) {
        profile_frame_ = frame_number_ + 1;
        if (profile_pool_ == VK_NULL_HANDLE) {
            VkQueryPoolCreateInfo qci{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
            qci.queryType = VK_QUERY_TYPE_TIMESTAMP;
            qci.queryCount = 8192;
            VK_CHECK(vkCreateQueryPool(ctx_.device, &qci, nullptr, &profile_pool_));
        }
        vkCmdResetQueryPool(cmd(), profile_pool_, 0, 8192);
        profile_marks_.clear();
        profiling_ = true;
        profile_mark("start");
    }
    if (tracing()) std::fprintf(stderr, "trace: swap (end of frame %llu)\n", static_cast<unsigned long long>(frame_number_));
    // TTT2_TRACE_AT=seconds: trace the first frame after that time.
    static const double trace_at = [] {
        const char* v = std::getenv("TTT2_TRACE_AT");
        return v ? std::atof(v) : -1.0;
    }();
    static const double started = now_seconds();
    if (trace_at >= 0 && trace_frame_ == UINT64_MAX && now_seconds() - started >= trace_at) {
        trace_frame_ = frame_number_ + 1;
    }
    ++frame_number_;
    ++frames_;
    compile_wait_budget_ms_ = compile_wait_budget_max_ms_;
    const double t = now_seconds();
    // Persist newly compiled pipelines quickly. This matters especially on
    // Windows, where a crash/Alt-F4 shortly after entering a new stage used
    // to lose all newly learned driver pipelines.
    static double last_cache_save = t;
    if (new_pipelines_.load(std::memory_order_acquire) != 0 && t - last_cache_save >= 2.0) {
        save_pipeline_cache();
        last_cache_save = t;
    }
    if (t - window_start_ >= 10.0) {
        save_pipeline_cache();

        const double frame_count = static_cast<double>(std::max<uint64_t>(frames_, 1));
        const double fps = frames_ / (t - window_start_);
        const double draws_per_frame = double(draws_) / frame_count;
        const double skipped_per_frame = double(skipped_draws_) / frame_count;
        const double waits_per_frame = double(waits_) / frame_count;
        const double wait_ms = wait_seconds_ * 1000.0 / frame_count;
        const double busy_ms = busy_seconds_ * 1000.0 / frame_count;
        const double replay_ms = replay_seconds_ * 1000.0 / frame_count;
        const double match_ms = match_seconds_ * 1000.0 / frame_count;
        const double shader_wait_ms = shader_wait_seconds_ * 1000.0 / frame_count;
        const double pipeline_wait_ms = pipeline_wait_seconds_ * 1000.0 / frame_count;
        const double shader_waits_per_frame = double(shader_waits_) / frame_count;
        const double pipeline_waits_per_frame = double(pipeline_waits_) / frame_count;

        // last_run.log can be unavailable on GUI-subsystem Windows launches.
        // Mirror the useful performance counters into the raw boot trace,
        // whose Win32 writer is independent of stdio.
        char perf[768]{};
        std::snprintf(
            perf, sizeof(perf),
            "PERF fps=%.1f draws/frame=%.0f skipped/frame=%.0f gpu_waits/frame=%.1f "
            "gpu_wait_ms/frame=%.2f gpu_busy_ms/frame=%.2f shader_waits/frame=%.2f "
            "shader_wait_ms/frame=%.2f pipeline_waits/frame=%.2f pipeline_wait_ms/frame=%.2f "
            "targets=%zu textures=%zu shaders=%zu pipelines=%zu replay_ms/frame=%.2f match_ms/frame=%.2f",
            fps, draws_per_frame, skipped_per_frame, waits_per_frame, wait_ms, busy_ms,
            shader_waits_per_frame, shader_wait_ms, pipeline_waits_per_frame, pipeline_wait_ms,
            targets_.size(), textures_.size(), shaders_.size(), pipelines_.size(),
            replay_ms, match_ms);
        cafe::diagnostic_trace(perf);

        std::fprintf(stderr, "ttt2: gpu: %.1f frames/s, %.0f draws per frame (%.0f skipped), %zu targets, %zu textures, "
                             "%zu shaders, %zu pipelines; %.1f GPU waits, %.1f ms waiting (%.1f ms GPU busy) per frame; %llu/%llu extra frames blended (%llu skipped: GPU behind), %.1f MB recorded; replay %.1f ms (matching %.1f)\n",
                     fps, draws_per_frame, skipped_per_frame, targets_.size(), textures_.size(),
                     shaders_.size(), pipelines_.size(), waits_per_frame,
                     wait_ms, busy_ms,
                     static_cast<unsigned long long>(blended_replays_),
                     static_cast<unsigned long long>(replays_), static_cast<unsigned long long>(skipped_replays_),
                     frame_records_[current_record_ ^ 1].used / 1e6,
                     replay_ms, match_ms);
        if (!swap_delays_.empty()) {
            std::sort(swap_delays_.begin(), swap_delays_.end());
            const auto pct = [&](size_t p) { return swap_delays_[std::min(swap_delays_.size() - 1, swap_delays_.size() * p / 100)]; };
            static const bool trace = std::getenv("TTT2_TRACE_PACING") != nullptr;
            if (trace) {
                std::fprintf(stderr, "pacing: swap after its vblank 50%% %.1f, 90%% %.1f, 99%% %.1f, max %.1f ms\n", pct(50),
                             pct(90), pct(99), swap_delays_.back());
            }
            swap_delays_.clear();
        }
        replay_seconds_ = match_seconds_ = 0;
        replays_ = blended_replays_ = skipped_replays_ = 0;
        window_start_ = t;
        frames_ = draws_ = skipped_draws_ = waits_ = 0;
        shader_waits_ = pipeline_waits_ = 0;
        wait_seconds_ = busy_seconds_ = 0;
        shader_wait_seconds_ = pipeline_wait_seconds_ = 0;
    }
}

} // namespace vk
} // namespace cafe::gpu
