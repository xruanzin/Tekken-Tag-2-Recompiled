#include "host/thread.h"

#include "cafe/runtime.h"

#include <memory>
#include <new>

#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <process.h>
#else
#include <pthread.h>
#endif

namespace cafe::host {

struct Start {
    ThreadEntry entry;
    void* argument;
};

#ifdef _WIN32
struct JoinableThread { HANDLE handle = nullptr; };

unsigned __stdcall trampoline(void* opaque) {
    std::unique_ptr<Start> start(static_cast<Start*>(opaque));
    start->entry(start->argument);
    return 0;
}

static HANDLE start_thread(ThreadEntry entry, void* argument, size_t stack_size) {
    auto start = std::make_unique<Start>(Start{entry, argument});
    uintptr_t value = _beginthreadex(nullptr, static_cast<unsigned>(stack_size), trampoline,
                                     start.get(), 0, nullptr);
    if (value == 0) cafe::fatal("cannot create host thread (Windows error %lu)", GetLastError());
    start.release();
    return reinterpret_cast<HANDLE>(value);
}

void start_detached_thread(ThreadEntry entry, void* argument, size_t stack_size) {
    HANDLE handle = start_thread(entry, argument, stack_size);
    CloseHandle(handle);
}

JoinableThread* start_joinable_thread(ThreadEntry entry, void* argument, size_t stack_size) {
    auto* thread = new JoinableThread;
    thread->handle = start_thread(entry, argument, stack_size);
    return thread;
}

void join_thread(JoinableThread* thread) {
    if (!thread) return;
    WaitForSingleObject(thread->handle, INFINITE);
    CloseHandle(thread->handle);
    delete thread;
}

#else
struct JoinableThread { pthread_t handle{}; };

void* trampoline(void* opaque) {
    std::unique_ptr<Start> start(static_cast<Start*>(opaque));
    start->entry(start->argument);
    return nullptr;
}

static pthread_t start_thread(ThreadEntry entry, void* argument, size_t stack_size, bool detached) {
    auto start = std::make_unique<Start>(Start{entry, argument});
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    if (stack_size != 0) pthread_attr_setstacksize(&attr, stack_size);
    if (detached) pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_t thread{};
    if (pthread_create(&thread, &attr, trampoline, start.get()) != 0) {
        pthread_attr_destroy(&attr);
        cafe::fatal("cannot create host thread");
    }
    pthread_attr_destroy(&attr);
    start.release();
    return thread;
}

void start_detached_thread(ThreadEntry entry, void* argument, size_t stack_size) {
    (void)start_thread(entry, argument, stack_size, true);
}

JoinableThread* start_joinable_thread(ThreadEntry entry, void* argument, size_t stack_size) {
    auto* thread = new JoinableThread;
    thread->handle = start_thread(entry, argument, stack_size, false);
    return thread;
}

void join_thread(JoinableThread* thread) {
    if (!thread) return;
    pthread_join(thread->handle, nullptr);
    delete thread;
}
#endif

} // namespace cafe::host
