// PipelineState.hpp
// Phase 4: Checkpoint & Recovery – Pipeline state container definitions
#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <type_traits>

namespace NES::State {

// Forward declarations for operator states. Actual definitions should exist
// in StateDefinitions.hpp as part of prior phases.
struct AggregationState;
struct JoinState;
struct FilterState;
struct ProjectionState;

// Lightweight tagged union without external deps to keep this header stable.
// This mirrors the idea of a variant of operator states.
struct OperatorStateTag {
    enum class Kind : uint8_t {
        Aggregation,
        NestedLoopJoin,
        HashJoin,
        Filter,
        Projection,
        Unknown
    };
};

struct OperatorStateHeader {
    OperatorStateTag::Kind kind{OperatorStateTag::Kind::Unknown};
    uint64_t operatorId{0};
    uint32_t version{1};
};

// A generic container that can hold raw bytes of a specific operator state.
// This avoids introducing schema dependencies here and allows zero-copy write-out.
struct OperatorStateBlob {
    OperatorStateHeader header{};
    std::vector<uint8_t> bytes; // Serialized state payload for the operator
};

// Progress information needed for consistent recovery.
struct ProgressMetadata {
    uint64_t pipelineId{0};
    uint64_t lastWatermark{0};
    // Optional: per-origin/process counters
    struct OriginProgress {
        uint64_t originId{0};
        uint64_t processedRecords{0};
        uint64_t lastWatermark{0};
    };
    std::vector<OriginProgress> origins;

    uint32_t version{1};
};

// Top-level pipeline state for checkpoint/recovery.
struct PipelineState {
    // Bump when incompatible format changes are introduced
    uint32_t version{1};
    uint64_t queryId{0};
    uint64_t pipelineId{0};
    uint64_t createdTimestampNs{0};

    // Sequence of operator states in execution order
    std::vector<OperatorStateBlob> operators;

    // Watermarks and progress
    ProgressMetadata progress;
};

} // namespace NES::State
