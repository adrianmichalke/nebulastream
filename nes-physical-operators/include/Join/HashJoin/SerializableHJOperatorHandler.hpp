/*
    Licensed under the Apache License, Version 2.0
*/

#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <vector>

#include <Identifiers/Identifiers.hpp>
#include <Join/StreamJoinOperatorHandler.hpp>
#include <Nautilus/State/Reflection/StateDefinitions.hpp>
#include <Nautilus/State/Serialization/StateSerializer.hpp>
#include <Runtime/Execution/OperatorHandler.hpp>
#include <SliceStore/Slice.hpp>
#include <SliceStore/WindowSlicesStoreInterface.hpp>

namespace NES {

class SerializableHJOperatorHandler final : public StreamJoinOperatorHandler {
public:
    SerializableHJOperatorHandler(
        const std::vector<OriginId>& inputOrigins,
        OriginId outputOriginId,
        std::unique_ptr<WindowSlicesStoreInterface> sliceAndWindowStore);

    SerializableHJOperatorHandler(
        const std::vector<OriginId>& inputOrigins,
        OriginId outputOriginId,
        std::unique_ptr<WindowSlicesStoreInterface> sliceAndWindowStore,
        const State::JoinState& initialState);

    // State accessors
    const State::JoinState& getState() const { return state_; }
    State::JoinState& getState() { return state_; }

    // Serialization
    TupleBuffer serialize(AbstractBufferProvider* bufferProvider) const {
        return NES::Serialization::StateSerializer<State::JoinState>::serialize(state_, bufferProvider);
    }
    static std::shared_ptr<SerializableHJOperatorHandler> deserialize(
        const TupleBuffer& buffer,
        const std::vector<OriginId>& inputOrigins,
        OriginId outputOriginId,
        std::unique_ptr<WindowSlicesStoreInterface> sliceAndWindowStore);

    // Slice creation for serializable hash join
    [[nodiscard]] std::function<std::vector<std::shared_ptr<Slice>>(SliceStart, SliceEnd)>
    getCreateNewSlicesFunction(const CreateNewSlicesArguments& newSlicesArguments) const override;

    // Snapshot/rehydrate hooks (to be implemented in later phases)
    void captureState();
    void rehydrateFromState();

private:
    void emitSlicesToProbe(
        Slice& sliceLeft,
        Slice& sliceRight,
        const WindowInfo& windowInfo,
        const SequenceData& sequenceData,
        PipelineExecutionContext* pipelineCtx) override;

    State::JoinState state_{};
};

} // namespace NES
