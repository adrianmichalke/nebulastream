/*
    Licensed under the Apache License, Version 2.0
*/

#include <Nautilus/DataStructures/SerializablePagedVector.hpp>
#include <Runtime/AbstractBufferProvider.hpp>
#include <type_traits>

namespace NES::DataStructures {

SerializablePagedVector::SerializablePagedVector(PagedVectorState& state) : state_(state) {}

void SerializablePagedVector::appendPageIfFull(AbstractBufferProvider* /*bufferProvider*/, const MemoryLayout* memoryLayout)
{
    if (memoryLayout) {
        state_.pageSize = memoryLayout->getBufferSize();
        state_.entrySize = memoryLayout->getTupleSize();
    }
    if (state_.pageSize == 0 || state_.entrySize == 0) return;

    if (state_.pages.empty()) {
        PagedVectorState::PageData page{};
        page.buffer.resize(state_.pageSize);
        page.bufferSize = state_.pageSize;
        page.numberOfEntries = 0;
        page.cumulativeSum = 0;
        state_.pages.emplace_back(std::move(page));
        cachedBuffers_.resize(1);
        createTupleBuffer(0);
        return;
    }

    const size_t last = state_.pages.size() - 1;
    createTupleBuffer(last);
    auto& tb = *cachedBuffers_[last];
    const uint64_t tuples = tb.getNumberOfTuples();
    const uint64_t capacity = state_.pageSize / state_.entrySize;
    if (tuples >= capacity) {
        PagedVectorState::PageData page{};
        page.buffer.resize(state_.pageSize);
        page.bufferSize = state_.pageSize;
        page.numberOfEntries = 0;
        page.cumulativeSum = state_.totalEntries;
        state_.pages.emplace_back(std::move(page));
        cachedBuffers_.resize(state_.pages.size());
        createTupleBuffer(state_.pages.size() - 1);
    }
}

void SerializablePagedVector::moveAllPages(SerializablePagedVector& other)
{
    for (auto& p : other.state_.pages) {
        state_.pages.emplace_back(std::move(p));
    }
    other.state_.pages.clear();
    cachedBuffers_.clear();
    cachedBuffers_.resize(state_.pages.size());
    updateCumulativeSums();
}

void SerializablePagedVector::copyFrom(const SerializablePagedVector& other)
{
    state_ = other.state_;
    cachedBuffers_.clear();
    cachedBuffers_.resize(state_.pages.size());
}

const TupleBuffer* SerializablePagedVector::getTupleBufferForEntry(uint64_t entryPos) const
{
    if (state_.pages.empty()) return nullptr;
    const_cast<SerializablePagedVector*>(this)->updateCumulativeSums();
    const size_t idx = findPageIndex(entryPos);
    createTupleBuffer(idx);
    return cachedBuffers_[idx].get();
}

std::optional<uint64_t> SerializablePagedVector::getBufferPosForEntry(uint64_t entryPos) const
{
    if (state_.pages.empty()) return std::nullopt;
    const_cast<SerializablePagedVector*>(this)->updateCumulativeSums();
    const size_t idx = findPageIndex(entryPos);
    const auto& page = state_.pages[idx];
    return entryPos - page.cumulativeSum;
}

uint64_t SerializablePagedVector::getTotalNumberOfEntries() const
{
    const_cast<SerializablePagedVector*>(this)->updateCumulativeSums();
    return state_.totalEntries;
}

const TupleBuffer& SerializablePagedVector::getLastPage() const
{
    const size_t idx = state_.pages.empty() ? 0 : state_.pages.size() - 1;
    createTupleBuffer(idx);
    return *cachedBuffers_[idx];
}

const TupleBuffer& SerializablePagedVector::getFirstPage() const
{
    const size_t idx = 0;
    createTupleBuffer(idx);
    return *cachedBuffers_[idx];
}

const PagedVectorState::PageData& SerializablePagedVector::getPageData(size_t index) const { return state_.pages[index]; }

size_t SerializablePagedVector::findPageIndex(uint64_t entryPos) const
{
    for (size_t i = 0; i < state_.pages.size(); ++i) {
        const auto& p = state_.pages[i];
        const uint64_t end = p.cumulativeSum + p.numberOfEntries;
        if (entryPos < end) return i;
    }
    return state_.pages.empty() ? 0 : state_.pages.size() - 1;
}

void SerializablePagedVector::setMemoryLayout(const MemoryLayout* layout)
{
    if (layout) {
        state_.pageSize = layout->getBufferSize();
        state_.entrySize = layout->getTupleSize();
    }
    serializeMemoryLayout(layout);
}

std::unique_ptr<MemoryLayout> SerializablePagedVector::getMemoryLayout() const
{
    // Not required for our current join paths; return nullptr to avoid abstract construction
    return nullptr;
}

void SerializablePagedVector::updateCumulativeSums()
{
    uint64_t sum = 0;
    for (size_t i = 0; i < state_.pages.size(); ++i) {
        createTupleBuffer(i);
        const TupleBuffer& tb = *cachedBuffers_[i];
        const uint64_t tuples = tb.getNumberOfTuples();
        state_.pages[i].numberOfEntries = tuples;
        state_.pages[i].cumulativeSum = sum;
        sum += tuples;
    }
    state_.totalEntries = sum;
}

void SerializablePagedVector::createTupleBuffer(size_t pageIndex) const
{
    if (pageIndex >= cachedBuffers_.size()) cachedBuffers_.resize(pageIndex + 1);
    if (cachedBuffers_[pageIndex]) return;
    auto& page = state_.pages[pageIndex];
    // Wrap page.buffer memory into a TupleBuffer view
    auto* nonConst = const_cast<uint8_t*>(reinterpret_cast<const uint8_t*>(page.buffer.data()));
    TupleBuffer tb = TupleBuffer::reinterpretAsTupleBuffer(nonConst);
    cachedBuffers_[pageIndex] = std::make_unique<TupleBuffer>(std::move(tb));
}

void SerializablePagedVector::serializeMemoryLayout(const MemoryLayout* /*layout*/)
{
    state_.hasMemoryLayout = false;
}

} // namespace NES::DataStructures

