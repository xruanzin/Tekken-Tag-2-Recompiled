#pragma once

#include <cstddef>

namespace cafe::host {

using ThreadEntry = void (*)(void*);
struct JoinableThread;

void start_detached_thread(ThreadEntry entry, void* argument, size_t stack_size);
JoinableThread* start_joinable_thread(ThreadEntry entry, void* argument, size_t stack_size);
void join_thread(JoinableThread* thread);

} // namespace cafe::host
