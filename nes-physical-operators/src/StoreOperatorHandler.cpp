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

#include <StoreOperatorHandler.hpp>

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <string>
#include <system_error>
#include <memory>

#include <ErrorHandling.hpp>
#include <PipelineExecutionContext.hpp>
#include <Runtime/QueryTerminationType.hpp>

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <cstdlib>
#include <algorithm>
#include <thread>
#include <chrono>
#include <sys/uio.h>
#include <sys/statvfs.h>

namespace NES
{

namespace
{
constexpr const char MAGIC[8] = {'N', 'E', 'S', 'S', 'T', 'O', 'R', 'E'}; // 8 bytes
constexpr uint32_t VERSION = 1;
constexpr uint8_t ENDIANNESS_LE = 1; // we write LE
}

StoreOperatorHandler::StoreOperatorHandler(Config cfg) : OperatorHandler(false), config(std::move(cfg)) { }

void StoreOperatorHandler::start(PipelineExecutionContext& pec, uint32_t)
{
    openFile();
    if (config.header)
    {
        writeHeaderIfNeeded();
    }
    // Initialize per-worker shards on first start; size based on pipeline concurrency
    // Guard against 0 workers (tests); default to 1
    size_t numWorkers = std::max<uint64_t>(1, pec.getNumberOfWorkerThreads());
    initShards(numWorkers);

#if NES_WITH_IO_URING
    if (config.asyncBackend == Config::AsyncBackend::IO_URING)
    {
        // Defer lib include and initialization to .cpp to keep header decoupled
        struct io_uring;
        extern int uring_init(void** outRing, uint32_t depth);
        if (uring_init(&uring.ringPtr, uring.depth) != 0)
        {
            NES_WARN("io_uring init failed; falling back to POSIX writes");
            config.asyncBackend = Config::AsyncBackend::POSIX;
        }
        else
        {
            // Register shard buffers to enable write_fixed
            registerShardBuffers();
            uring.running.store(true);
            uring.cqThread = std::jthread([this](std::stop_token)
            {
                extern void uring_cq_loop(void* ringPtr, std::atomic<bool>& running);
                uring_cq_loop(uring.ringPtr, uring.running);
            });
        }
    }
#endif
}

void StoreOperatorHandler::stop(QueryTerminationType, PipelineExecutionContext&)
{
    // Flush all shards
    for (auto& s : shards)
    {
        flushShard(s);
    }

    if (fd >= 0)
    {
        ::fsync(fd);
        ::close(fd);
        fd = -1;
    }

    freeShards();

#if NES_WITH_IO_URING
    if (uring.ringPtr)
    {
        uring.running.store(false);
        // Join CQ thread by requesting stop
        if (uring.cqThread.joinable())
        {
            uring.cqThread.request_stop();
            uring.cqThread.join();
        }
        unregisterShardBuffers();
        extern void uring_shutdown(void* ringPtr);
        uring_shutdown(uring.ringPtr);
        uring.ringPtr = nullptr;
    }
#endif
}

void StoreOperatorHandler::ensureHeader(PipelineExecutionContext&)
{
    if (config.header)
    {
        writeHeaderIfNeeded();
    }
}

void StoreOperatorHandler::append(const uint8_t* data, size_t len)
{
    if (len == 0)
    {
        return;
    }
    if (fd < 0)
    {
        throw CannotOpenSink("StoreOperatorHandler not started; fd is invalid");
    }
    size_t writeLen = len;
    // For direct I/O, ensure length is aligned by padding with zeros
    std::unique_ptr<uint8_t[]> tmp;
    const uint8_t* src = data;
    if (config.directIO)
    {
        const size_t rem = writeLen % ioAlign;
        if (rem != 0)
        {
            const size_t pad = ioAlign - rem;
            tmp = std::unique_ptr<uint8_t[]>(new uint8_t[writeLen + pad]);
            std::memcpy(tmp.get(), data, writeLen);
            std::memset(tmp.get() + writeLen, 0, pad);
            writeLen += pad;
            src = tmp.get();
        }
    }
    const uint64_t off = tail.fetch_add(writeLen, std::memory_order_relaxed);
    ssize_t written = ::pwrite(fd, src, writeLen, static_cast<off_t>(off));
    if (written < 0 || static_cast<size_t>(written) != writeLen)
    {
        throw CannotOpenSink("pwrite failed: errno={} {}", errno, std::strerror(errno));
    }
    if (config.fdatasyncInterval > 0)
    {
        ++writesSinceSync;
        if (writesSinceSync >= config.fdatasyncInterval)
        {
            ::fdatasync(fd);
            writesSinceSync = 0;
        }
    }
}

void StoreOperatorHandler::openFile()
{
    int flags = O_CREAT | O_WRONLY;
    if (!config.append)
    {
        flags |= O_TRUNC;
    }
#ifdef O_DSYNC
    // leave to fdatasync interval instead of O_DSYNC for throughput
#endif
#ifdef O_DIRECT
    if (config.directIO)
    {
        flags |= O_DIRECT;
    }
#endif
    fd = ::open(config.filePath.c_str(), flags, 0644);
    if (fd < 0)
    {
        throw CannotOpenSink("Could not open output file: {} (errno={}, msg={})", config.filePath, errno, std::strerror(errno));
    }
    // Determine current size and initialize tail
    struct stat st
    {
    };
    if (::fstat(fd, &st) != 0)
    {
        const int err = errno;
        ::close(fd);
        fd = -1;
        throw CannotOpenSink("fstat failed for {}: errno={}, msg={}", config.filePath, err, std::strerror(err));
    }
    tail.store(static_cast<uint64_t>(st.st_size), std::memory_order_relaxed);
    headerWritten.store(st.st_size > 0, std::memory_order_relaxed);

    // Derive I/O alignment: start with HW page size
    long ps = ::sysconf(_SC_PAGESIZE);
    if (ps > 0)
    {
        ioPageSize = static_cast<size_t>(ps);
    }
    ioAlign = ioPageSize;
    // Optionally consider filesystem block size for better alignment (best-effort)
    struct statvfs vfs
    {
    };
    if (::statvfs(config.filePath.c_str(), &vfs) == 0 && vfs.f_bsize > 0)
    {
        // align to max(page, fs block)
        ioAlign = std::max(ioAlign, static_cast<size_t>(vfs.f_bsize));
    }

    // Phase 0: guardrails (no behavior change yet)
    // Ensure reasonable chunkMinBytes if provided via config
    if (config.chunkMinBytes == 0)
    {
        config.chunkMinBytes = 65536u; // default to 64KiB
    }
    // If direct I/O is requested, recommend 4KiB alignment for future batching phases
    if (config.directIO && (config.chunkMinBytes % ioAlign) != 0)
    {
        // Keep non-fatal to avoid disrupting existing workflows; future phases will enforce
        NES_DEBUG(
            "StoreOperatorHandler: direct_io=true but chunkMinBytes={} is not {}-aligned (will be adjusted when batching is active)",
            config.chunkMinBytes,
            ioAlign);
    }
}

uint8_t* StoreOperatorHandler::allocAligned(size_t size, bool& alignedOut)
{
    alignedOut = false;
    // If direct I/O, align to system-derived ioAlign
    const size_t align = config.directIO ? ioAlign : alignof(std::max_align_t);
    void* ptr = nullptr;
    if (posix_memalign(&ptr, align, size) == 0)
    {
        alignedOut = (align >= ioAlign);
        return static_cast<uint8_t*>(ptr);
    }
    // std::aligned_alloc requires size to be a multiple of alignment
    const size_t rounded = ((size + align - 1) / align) * align;
    return static_cast<uint8_t*>(std::aligned_alloc(align, rounded));
}

void StoreOperatorHandler::initShards(size_t numWorkers)
{
    if (!shards.empty())
    {
        return;
    }
    shards.resize(numWorkers);
    // Round capacity up to a multiple of ioAlign to guarantee aligned flushes
    const size_t base = std::max<size_t>(config.chunkMinBytes, ioAlign);
    const size_t cap = ((base + ioAlign - 1) / ioAlign) * ioAlign;
    for (auto& s : shards)
    {
        for (int i = 0; i < 2; ++i)
        {
            bool aligned = false;
            auto& slot = s.slots[i];
            slot.buf = allocAligned(cap, aligned);
            slot.capacity = cap;
            slot.fill = 0;
            slot.aligned = aligned;
            slot.inFlight.store(false, std::memory_order_relaxed);
            slot.bufIndex = static_cast<unsigned>(-1);
        }
        s.active = 0;
    }
}

void StoreOperatorHandler::freeShards()
{
    for (auto& s : shards)
    {
        for (int i = 0; i < 2; ++i)
        {
            auto& slot = s.slots[i];
            if (slot.buf)
            {
                std::free(slot.buf);
                slot.buf = nullptr;
            }
            slot.capacity = 0;
            slot.fill = 0;
            slot.aligned = false;
            slot.inFlight.store(false, std::memory_order_relaxed);
            slot.bufIndex = static_cast<unsigned>(-1);
        }
        s.active = 0;
    }
    shards.clear();
}

void StoreOperatorHandler::flushShard(Shard& s)
{
    // Flush both slots (active first), if they contain data
    size_t shardIdx = static_cast<size_t>(&s - shards.data());
    int active = s.active;
    int other = s.active ^ 1;
    flushShardSlot(shardIdx, active);
    flushShardSlot(shardIdx, other);
}

void StoreOperatorHandler::flushShardSlot(size_t shardIdx, int slotIdx)
{
    auto& s = shards[shardIdx];
    auto& slot = s.slots[slotIdx];
    if (slot.fill == 0)
    {
        return;
    }
    // Compute aligned length if O_DIRECT is active
    size_t writeLen = slot.fill;
    if (config.directIO)
    {
        const size_t rem = writeLen % ioAlign;
        if (rem != 0)
        {
            const size_t pad = ioAlign - rem;
            // Ensure we have space to zero-pad within the slot capacity
            if (writeLen + pad <= slot.capacity)
            {
                std::memset(slot.buf + writeLen, 0, pad);
                writeLen += pad;
            }
        }
    }
    const uint64_t off = tail.fetch_add(writeLen, std::memory_order_relaxed);
#if NES_WITH_IO_URING
    if (config.asyncBackend == Config::AsyncBackend::IO_URING && uring.ringPtr)
    {
        slot.inFlight.store(true, std::memory_order_release);
        extern int uring_submit_write_fixed(void* ringPtr, int fd, unsigned buf_index, size_t len, uint64_t off, uint64_t userData);
        int rc = -1;
        if (uring.buffersRegistered && slot.bufIndex != static_cast<unsigned>(-1))
        {
            rc = uring_submit_write_fixed(uring.ringPtr, fd, slot.bufIndex, writeLen, off, reinterpret_cast<uint64_t>(&slot.inFlight));
        }
        else
        {
            extern int uring_submit_write(void* ringPtr, int fd, const void* buf, size_t len, uint64_t off, uint64_t userData);
            rc = uring_submit_write(uring.ringPtr, fd, slot.buf, writeLen, off, reinterpret_cast<uint64_t>(&slot.inFlight));
        }
        if (rc != 0)
        {
            // Fallback to POSIX write on failure
            ssize_t written = ::pwrite(fd, slot.buf, writeLen, static_cast<off_t>(off));
            if (written < 0 || static_cast<size_t>(written) != writeLen)
            {
                throw CannotOpenSink("pwrite(slot, fallback) failed: errno={} {}", errno, std::strerror(errno));
            }
            slot.inFlight.store(false, std::memory_order_release);
        }
        if (config.fdatasyncInterval > 0)
        {
            ++writesSinceSync;
            if (writesSinceSync >= config.fdatasyncInterval)
            {
                extern int uring_submit_fsync(void* ringPtr, int fd);
                (void) uring_submit_fsync(uring.ringPtr, fd);
                writesSinceSync = 0;
            }
        }
        slot.fill = 0;
        return;
    }
#endif
    // POSIX path
    ssize_t written = ::pwrite(fd, slot.buf, writeLen, static_cast<off_t>(off));
    if (written < 0 || static_cast<size_t>(written) != writeLen)
    {
        throw CannotOpenSink("pwrite(slot) failed: errno={} {}", errno, std::strerror(errno));
    }
    if (config.fdatasyncInterval > 0)
    {
        ++writesSinceSync;
        if (writesSinceSync >= config.fdatasyncInterval)
        {
            ::fdatasync(fd);
            writesSinceSync = 0;
        }
    }
    slot.fill = 0;
}

uint8_t* StoreOperatorHandler::reserve(uint32_t workerIndex, uint32_t len)
{
    if (shards.empty())
    {
        // conservative default to at least one shard
        initShards(1);
    }
    const size_t sidx = workerIndex % shards.size();
    auto& s = shards[sidx];
    auto& slot = s.slots[s.active];
    if (len > slot.capacity)
    {
        flushShard(s);
        throw CannotOpenSink("StoreOperatorHandler: oversized row {} > slot capacity {} not supported", len, slot.capacity);
    }
    if (slot.fill + len > slot.capacity)
    {
        // Flush current slot, swap and wait for availability
        flushShardSlot(sidx, s.active);
        s.active ^= 1;
        auto& other = s.slots[s.active];
        while (other.inFlight.load(std::memory_order_acquire))
        {
            std::this_thread::yield();
        }
        other.fill = 0;
        return other.buf;
    }
    return slot.buf + slot.fill;
}

void StoreOperatorHandler::commit(uint32_t workerIndex, uint32_t len)
{
    auto& s = shards[workerIndex % shards.size()];
    auto& slot = s.slots[s.active];
    slot.fill += len;
    if (slot.fill >= slot.capacity)
    {
        flushShardSlot(workerIndex % shards.size(), s.active);
        s.active ^= 1;
    }
}

void StoreOperatorHandler::flush(uint32_t workerIndex)
{
    auto& s = shards[workerIndex % shards.size()];
    flushShard(s);
}

void StoreOperatorHandler::commitBuffer(uint32_t workerIndex)
{
    if (shards.empty())
    {
        return;
    }
    auto& s = shards[workerIndex % shards.size()];
    // Flush both slots for this shard
    flushShard(s);

#if NES_WITH_IO_URING
    if (config.asyncBackend == Config::AsyncBackend::IO_URING && uring.ringPtr)
    {
        // Wait until outstanding submissions for these slots complete
        for (int i = 0; i < 2; ++i)
        {
            while (s.slots[i].inFlight.load(std::memory_order_acquire))
            {
                std::this_thread::yield();
            }
        }
    }
#endif

    if (config.flushOnClose)
    {
        // Force durability of file contents up to current tail
        ::fdatasync(fd);
    }
}

void StoreOperatorHandler::writeHeaderIfNeeded()
{
    bool expected = false;
    if (!headerWritten.compare_exchange_strong(expected, true, std::memory_order_acq_rel))
    {
        return; // another thread wrote the header or file already non-empty
    }

    // Only write header if file is empty
    // (We still double-check via tail==0)
    if (tail.load(std::memory_order_relaxed) > 0)
    {
        return;
    }

    // Build header buffer
    const uint64_t fingerprint = fnv1a64(config.schemaText.c_str(), config.schemaText.size());
    const uint32_t schemaLen = static_cast<uint32_t>(config.schemaText.size());

    const size_t headerSize = sizeof(MAGIC) + sizeof(uint32_t) + sizeof(uint8_t) + sizeof(uint32_t) + sizeof(uint64_t) + sizeof(uint32_t)
        + schemaLen;
    std::string buf;
    buf.resize(headerSize);
    size_t off = 0;
    std::memcpy(buf.data() + off, MAGIC, sizeof(MAGIC));
    off += sizeof(MAGIC);
    std::memcpy(buf.data() + off, &VERSION, sizeof(uint32_t));
    off += sizeof(uint32_t);
    std::memcpy(buf.data() + off, &ENDIANNESS_LE, sizeof(uint8_t));
    off += sizeof(uint8_t);
    uint32_t flags = 0;
    std::memcpy(buf.data() + off, &flags, sizeof(uint32_t));
    off += sizeof(uint32_t);
    std::memcpy(buf.data() + off, &fingerprint, sizeof(uint64_t));
    off += sizeof(uint64_t);
    std::memcpy(buf.data() + off, &schemaLen, sizeof(uint32_t));
    off += sizeof(uint32_t);
    std::memcpy(buf.data() + off, config.schemaText.data(), schemaLen);
    off += schemaLen;

    // Compute padded header len if using direct IO to ensure page-aligned data start
    size_t paddedHeader = headerSize;
    if (config.directIO)
    {
        const size_t rem = paddedHeader % ioAlign;
        if (rem != 0)
        {
            paddedHeader += ioAlign - rem;
        }
    }

    // Materialize padded buffer (zero pad tail)
    std::string out;
    out.resize(paddedHeader);
    std::memset(out.data(), 0, out.size());
    std::memcpy(out.data(), buf.data(), buf.size());

    // Append header at offset 0; ensure tail moves accordingly
    // Reserve offset 0 explicitly if file is empty
    const uint64_t off0 = tail.fetch_add(paddedHeader, std::memory_order_relaxed);
    if (off0 != 0)
    {
        // Another writer raced and wrote some bytes; do not write header again
        return;
    }
    ssize_t written = ::pwrite(fd, out.data(), out.size(), 0);
    if (written < 0 || static_cast<size_t>(written) != out.size())
    {
        throw CannotOpenSink("Writing store header failed: errno={} {}", errno, std::strerror(errno));
    }
}

#if NES_WITH_IO_URING
void StoreOperatorHandler::registerShardBuffers()
{
    if (!uring.ringPtr)
    {
        return;
    }
    // Build iov list for all shard slots
    std::vector<iovec> iov;
    iov.reserve(shards.size() * 2);
    for (auto& s : shards)
    {
        for (int i = 0; i < 2; ++i)
        {
            iovec v{};
            v.iov_base = s.slots[i].buf;
            v.iov_len = s.slots[i].capacity;
            iov.push_back(v);
        }
    }
    extern int uring_register_buffers(void* ringPtr, const struct iovec* iov, unsigned nr);
    int rc = uring_register_buffers(uring.ringPtr, iov.data(), static_cast<unsigned>(iov.size()));
    if (rc == 0)
    {
        uring.buffersRegistered = true;
        // Assign buffer indices
        unsigned idx = 0;
        for (auto& s : shards)
        {
            for (int i = 0; i < 2; ++i)
            {
                s.slots[i].bufIndex = idx++;
            }
        }
    }
    else
    {
        NES_WARN("io_uring: register_buffers failed (rc={}); falling back to non-fixed writes", rc);
        uring.buffersRegistered = false;
        for (auto& s : shards)
        {
            for (int i = 0; i < 2; ++i)
            {
                s.slots[i].bufIndex = static_cast<unsigned>(-1);
            }
        }
    }
}

void StoreOperatorHandler::unregisterShardBuffers()
{
    if (!uring.ringPtr || !uring.buffersRegistered)
    {
        return;
    }
    extern int uring_unregister_buffers(void* ringPtr);
    (void) uring_unregister_buffers(uring.ringPtr);
    uring.buffersRegistered = false;
}
#endif

uint64_t StoreOperatorHandler::fnv1a64(const char* data, size_t len)
{
    uint64_t hash = 1469598103934665603ull; // FNV offset basis
    for (size_t i = 0; i < len; ++i)
    {
        hash ^= static_cast<uint8_t>(data[i]);
        hash *= 1099511628211ull; // FNV prime
    }
    return hash;
}

}
