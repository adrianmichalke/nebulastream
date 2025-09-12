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

#pragma once

#include <atomic>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>
#include <thread>

#include <Runtime/Execution/OperatorHandler.hpp>

namespace NES
{

// POSIX-based writer operator handler for the Store operator.
class StoreOperatorHandler final : public OperatorHandler
{
public:
    // Backend selection for asynchronous writes (scaffolding for future phases)
    enum class AsyncBackend : uint8_t
    {
        POSIX = 0,
        IO_URING = 1
    };

    struct Config
    {
        std::string filePath;
        bool append{true};
        bool header{true};
        bool directIO{false};
        uint32_t fdatasyncInterval{0};
        std::string schemaText; // canonical schema string for header

        // Phase 0 toggles (not yet changing behavior)
        uint32_t chunkMinBytes{65536};
        AsyncBackend asyncBackend{AsyncBackend::POSIX};
        bool flushOnClose{false};
    };

    explicit StoreOperatorHandler(Config cfg);
    ~StoreOperatorHandler() override = default;

    void start(PipelineExecutionContext& pipelineExecutionContext, uint32_t localStateVariableId) override;
    void stop(QueryTerminationType terminationType, PipelineExecutionContext& pipelineExecutionContext) override;

    // Ensures a single header is written if file is empty and header=true.
    void ensureHeader(PipelineExecutionContext& pec);

    // Append a contiguous buffer using atomic offset reservation and pwrite.
    void append(const uint8_t* data, size_t len);

    // Phase 1: per-worker shard staging (copy-free encode via reserve/commit)
    uint8_t* reserve(uint32_t workerIndex, uint32_t len);
    void commit(uint32_t workerIndex, uint32_t len);
    void flush(uint32_t workerIndex);
    void commitBuffer(uint32_t workerIndex);

private:
    void openFile();
    void writeHeaderIfNeeded();
    static uint64_t fnv1a64(const char* data, size_t len);

    struct Shard
    {
        struct Slot
        {
            uint8_t* buf{nullptr};
            size_t capacity{0};
            size_t fill{0};
            bool aligned{false};
            std::atomic<bool> inFlight{false};
            unsigned bufIndex{static_cast<unsigned>(-1)}; // io_uring registered buffer index

            Slot() = default;
            Slot(const Slot&) = delete;
            Slot& operator=(const Slot&) = delete;
            Slot(Slot&& other) noexcept
                : buf(other.buf)
                , capacity(other.capacity)
                , fill(other.fill)
                , aligned(other.aligned)
                , inFlight(other.inFlight.load(std::memory_order_relaxed))
                , bufIndex(other.bufIndex)
            {
                other.buf = nullptr;
                other.capacity = 0;
                other.fill = 0;
                other.aligned = false;
                other.inFlight.store(false, std::memory_order_relaxed);
                other.bufIndex = static_cast<unsigned>(-1);
            }
            Slot& operator=(Slot&& other) noexcept
            {
                if (this != &other)
                {
                    buf = other.buf;
                    capacity = other.capacity;
                    fill = other.fill;
                    aligned = other.aligned;
                    inFlight.store(other.inFlight.load(std::memory_order_relaxed), std::memory_order_relaxed);
                    bufIndex = other.bufIndex;
                    other.buf = nullptr;
                    other.capacity = 0;
                    other.fill = 0;
                    other.aligned = false;
                    other.inFlight.store(false, std::memory_order_relaxed);
                    other.bufIndex = static_cast<unsigned>(-1);
                }
                return *this;
            }
        };
        Slot slots[2]{};
        int active{0};

        Shard() = default;
        Shard(const Shard&) = delete;
        Shard& operator=(const Shard&) = delete;
        Shard(Shard&& other) noexcept
            : active(other.active)
        {
            slots[0] = std::move(other.slots[0]);
            slots[1] = std::move(other.slots[1]);
            other.active = 0;
        }
        Shard& operator=(Shard&& other) noexcept
        {
            if (this != &other)
            {
                slots[0] = std::move(other.slots[0]);
                slots[1] = std::move(other.slots[1]);
                active = other.active;
                other.active = 0;
            }
            return *this;
        }
    };
    void initShards(size_t numWorkers);
    void freeShards();
    void flushShard(Shard& s);
    void flushShardSlot(size_t shardIdx, int slotIdx);
    uint8_t* allocAligned(size_t size, bool& alignedOut);
    void registerShardBuffers();
    void unregisterShardBuffers();

    int fd{-1};
    std::atomic<uint64_t> tail{0};
    std::atomic<bool> headerWritten{false};
    Config config;
    uint64_t writesSinceSync{0};
    std::vector<Shard> shards;
    // I/O alignment derived from system page size (and optionally device block size)
    size_t ioPageSize{4096};
    size_t ioAlign{4096};

#if NES_WITH_IO_URING
    struct UringCtx
    {
        void* ringPtr{nullptr}; // opaque to avoid leaking liburing in header
        uint32_t depth{256};
        bool buffersRegistered{false};
        std::atomic<bool> running{false};
        std::jthread cqThread;
    };
    UringCtx uring;
#endif
};

}
