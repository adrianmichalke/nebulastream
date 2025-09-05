/*
    Licensed under the Apache License, Version 2.0 (the "License");
*/

#pragma once

#include <cstdint>
#include <vector>

#include <Nautilus/DataStructures/OffsetBasedHashMap.hpp>
#include <Nautilus/State/Reflection/StateDefinitions.hpp>

namespace NES::State {

// Convert OffsetHashMapState (runtime) -> AggregationState::HashMap (serializable)
inline AggregationState::HashMap toState(const DataStructures::OffsetHashMapState& s) {
    AggregationState::HashMap h{};
    h.memory = s.memory;                    // deep copy
    h.chains = s.chains;                    // deep copy
    h.entrySize = s.entrySize;
    h.tupleCount = s.tupleCount;
    h.keySize = s.keySize;
    h.valueSize = s.valueSize;
    h.bucketCount = s.bucketCount;
    h.varSizedMemory = s.varSizedMemory;    // deep copy
    h.varSizedOffset = s.varSizedOffset;
    return h;
}

// Convert AggregationState::HashMap (serializable) -> OffsetHashMapState (runtime)
inline DataStructures::OffsetHashMapState fromState(const AggregationState::HashMap& h) {
    DataStructures::OffsetHashMapState s{};
    s.memory = h.memory;                    // deep copy
    s.chains = h.chains;                    // deep copy
    s.entrySize = h.entrySize;
    s.tupleCount = h.tupleCount;
    s.keySize = h.keySize;
    s.valueSize = h.valueSize;
    s.bucketCount = h.bucketCount;
    s.varSizedMemory = h.varSizedMemory;    // deep copy
    s.varSizedOffset = h.varSizedOffset;
    return s;
}

} // namespace NES::State

