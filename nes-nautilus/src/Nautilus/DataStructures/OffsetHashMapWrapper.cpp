/*
    Licensed under the Apache License, Version 2.0
*/

#include <Nautilus/DataStructures/OffsetHashMapWrapper.hpp>
#include <cstring>

namespace NES::DataStructures {

using NES::Nautilus::Interface::AbstractHashMapEntry;
using NES::Nautilus::Interface::HashFunction;

AbstractHashMapEntry* OffsetHashMapWrapper::insertEntry(HashFunction::HashValue::raw_type hash,
                                                        AbstractBufferProvider* /*bufferProvider*/)
{
    // Allocate a new entry in the underlying offset-based hashmap and return an opaque handle to it.
    // Keys/values are initialized with zeros here; OffsetHashMapRef will populate actual bytes in onInsert.
    auto& state = impl_->extractState();
    // Reuse static buffers to avoid repeated allocations for common sizes
    thread_local std::vector<uint8_t> keyBuf;
    thread_local std::vector<uint8_t> valBuf;
    if (keyBuf.size() < state.keySize) keyBuf.resize(state.keySize, 0u);
    if (valBuf.size() < state.valueSize) valBuf.resize(state.valueSize, 0u);

    const uint32_t offset = impl_->insert(static_cast<uint64_t>(hash), keyBuf.data(), valBuf.data());
    auto* entry = getOffsetEntry(offset);
    return reinterpret_cast<AbstractHashMapEntry*>(entry);
}

} // namespace NES::DataStructures
