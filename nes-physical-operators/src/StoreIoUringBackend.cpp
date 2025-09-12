/*
    Licensed under the Apache License, Version 2.0 (the "License");
    you may not use this file except in compliance with the License.
    You may obtain a copy of the License at

        https://www.apache.org/licenses/LICENSE-2.0

    Unless required by applicable law or agreed to in writing, software
    distributed under the License is distributed on an "AS IS" BASIS,
    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
    See the License for the specific language governing permissions and
    limitations under the License.
*/

#include <cstdint>
#include <atomic>

#include <ErrorHandling.hpp>

#include <sys/types.h>

#include <cstring>

#if NES_WITH_IO_URING
#include <liburing.h>

extern "C" {

int uring_init(void** outRing, uint32_t depth)
{
    if (!outRing)
    {
        return -1;
    }
    auto* ring = new io_uring();
    std::memset(ring, 0, sizeof(io_uring));
    int ret = io_uring_queue_init(static_cast<unsigned>(depth), ring, 0);
    if (ret < 0)
    {
        delete ring;
        return ret;
    }
    *outRing = ring;
    return 0;
}

int uring_register_buffers(void* ringPtr, const struct iovec* iov, unsigned nr)
{
    auto* ring = reinterpret_cast<io_uring*>(ringPtr);
    if (!ring)
    {
        return -1;
    }
    // Use low-level register on ring_fd for maximum compatibility
    return io_uring_register_buffers(ring->ring_fd, iov, nr);
}

int uring_unregister_buffers(void* ringPtr)
{
    auto* ring = reinterpret_cast<io_uring*>(ringPtr);
    if (!ring)
    {
        return -1;
    }
    return io_uring_unregister_buffers(ring);
}

int uring_submit_write(void* ringPtr, int fd, const void* buf, size_t len, uint64_t off, uint64_t userData)
{
    auto* ring = reinterpret_cast<io_uring*>(ringPtr);
    if (!ring)
    {
        return -1;
    }
    io_uring_sqe* sqe = io_uring_get_sqe(ring);
    if (!sqe)
    {
        // try to submit and wait for at least one completion to free an SQE
        (void) io_uring_submit_and_wait(ring, 1);
        sqe = io_uring_get_sqe(ring);
        if (!sqe)
        {
            return -1;
        }
    }
    io_uring_prep_write(sqe, fd, buf, static_cast<unsigned>(len), static_cast<off_t>(off));
    sqe->user_data = userData;
    int ret = io_uring_submit(ring);
    return (ret < 0) ? ret : 0;
}

int uring_submit_write_fixed(void* ringPtr, int fd, unsigned buf_index, size_t len, uint64_t off, uint64_t userData)
{
    auto* ring = reinterpret_cast<io_uring*>(ringPtr);
    if (!ring)
    {
        return -1;
    }
    io_uring_sqe* sqe = io_uring_get_sqe(ring);
    if (!sqe)
    {
        (void) io_uring_submit_and_wait(ring, 1);
        sqe = io_uring_get_sqe(ring);
        if (!sqe)
        {
            return -1;
        }
    }
    io_uring_prep_write_fixed(sqe, fd, nullptr, static_cast<unsigned>(len), static_cast<off_t>(off), buf_index);
    sqe->user_data = userData;
    int ret = io_uring_submit(ring);
    return (ret < 0) ? ret : 0;
}

int uring_submit_fsync(void* ringPtr, int fd)
{
    auto* ring = reinterpret_cast<io_uring*>(ringPtr);
    if (!ring)
    {
        return -1;
    }
    io_uring_sqe* sqe = io_uring_get_sqe(ring);
    if (!sqe)
    {
        (void) io_uring_submit_and_wait(ring, 1);
        sqe = io_uring_get_sqe(ring);
        if (!sqe)
        {
            return -1;
        }
    }
    io_uring_prep_fsync(sqe, fd, IORING_FSYNC_DATASYNC);
    sqe->user_data = 0ull; // no buffer to free
    int ret = io_uring_submit(ring);
    return (ret < 0) ? ret : 0;
}

void uring_cq_loop(void* ringPtr, std::atomic<bool>& running)
{
    auto* ring = reinterpret_cast<io_uring*>(ringPtr);
    if (!ring)
    {
        return;
    }
    timespec ts{};
    ts.tv_sec = 0;
    ts.tv_nsec = 200 * 1000 * 1000; // 200ms
    while (running.load(std::memory_order_relaxed))
    {
        io_uring_cqe* cqe = nullptr;
        int ret = io_uring_wait_cqe_timeout(ring, &cqe, &ts);
        if (ret == -ETIME)
        {
            continue;
        }
        if (ret < 0)
        {
            break; // error
        }
        if (!cqe)
        {
            continue;
        }
        // Mark slot as available via atomic flag (user_data points to std::atomic<bool>)
        uint64_t ud = cqe->user_data;
        if (ud != 0ull)
        {
            auto* flag = reinterpret_cast<std::atomic<bool>*>(ud);
            flag->store(false, std::memory_order_release);
        }
        io_uring_cqe_seen(ring, cqe);
    }
    // drain remaining completions
    while (true)
    {
        io_uring_cqe* cqe = nullptr;
        int ret = io_uring_peek_cqe(ring, &cqe);
        if (ret < 0 || !cqe)
        {
            break;
        }
        uint64_t ud = cqe->user_data;
        if (ud != 0ull)
        {
            auto* flag = reinterpret_cast<std::atomic<bool>*>(ud);
            flag->store(false, std::memory_order_release);
        }
        io_uring_cqe_seen(ring, cqe);
    }
}

void uring_shutdown(void* ringPtr)
{
    auto* ring = reinterpret_cast<io_uring*>(ringPtr);
    if (!ring)
    {
        return;
    }
    io_uring_queue_exit(ring);
    delete ring;
}

} // extern "C"

#else

// Stubs to satisfy linker if compiled without io_uring
extern "C" {
int uring_init(void**, uint32_t) { return -1; }
int uring_register_buffers(void*, const struct iovec*, unsigned) { return -1; }
int uring_unregister_buffers(void*) { return -1; }
int uring_submit_write(void*, int, const void*, size_t, uint64_t, uint64_t) { return -1; }
int uring_submit_write_fixed(void*, int, unsigned, size_t, uint64_t, uint64_t) { return -1; }
int uring_submit_fsync(void*, int) { return -1; }
void uring_cq_loop(void*, std::atomic<bool>&) { }
void uring_shutdown(void*) { }
}

#endif
