/*
    Licensed under the Apache License, Version 2.0
*/

#include <gtest/gtest.h>
#include <Nautilus/State/Reflection/StateDefinitions.hpp>
#include <Nautilus/State/Serialization/StateSerializer.hpp>
#include <Nautilus/State/Conversion/JoinStateConverters.hpp>

#include <Join/NestedLoopJoin/SerializableNLJOperatorHandler.hpp>
#include <Join/HashJoin/SerializableHJOperatorHandler.hpp>
#include <SliceStore/DefaultTimeBasedSliceStore.hpp>
#include <Runtime/BufferManager.hpp>

using namespace NES;

class JoinSerializableStateTest : public ::testing::Test {
protected:
    void SetUp() override {
        bufferManager = BufferManager::create(1024 * 1024, 16);
    }
    std::shared_ptr<BufferManager> bufferManager;
};

TEST_F(JoinSerializableStateTest, NLJ_Empty_RoundTrip_Handler) {
    std::vector<OriginId> inputs{OriginId(1), OriginId(2)};
    OriginId out{3};
    auto sliceStore = std::make_unique<DefaultTimeBasedSliceStore>(1000, 1000);
    auto handler = std::make_shared<SerializableNLJOperatorHandler>(inputs, out, std::move(sliceStore));
    handler->setWorkerThreads(2);

    // Capture (empty) and serialize
    handler->captureState();
    auto tb = handler->serialize(bufferManager.get());

    // Deserialize and reserialize
    auto restored = SerializableNLJOperatorHandler::deserialize(tb, inputs, out, std::make_unique<DefaultTimeBasedSliceStore>(1000, 1000));
    auto tb2 = restored->serialize(bufferManager.get());

    ASSERT_EQ(tb.getBufferSize(), tb2.getBufferSize());
    EXPECT_EQ(std::memcmp(tb.getBuffer(), tb2.getBuffer(), tb.getBufferSize()), 0);
}

TEST_F(JoinSerializableStateTest, HJ_Empty_RoundTrip_Handler) {
    std::vector<OriginId> inputs{OriginId(10), OriginId(11)};
    OriginId out{12};
    auto sliceStore = std::make_unique<DefaultTimeBasedSliceStore>(1000, 1000);
    auto handler = std::make_shared<SerializableHJOperatorHandler>(inputs, out, std::move(sliceStore));
    handler->setWorkerThreads(2);

    handler->captureState();
    auto tb = handler->serialize(bufferManager.get());
    auto restored = SerializableHJOperatorHandler::deserialize(tb, inputs, out, std::make_unique<DefaultTimeBasedSliceStore>(1000, 1000));
    auto tb2 = restored->serialize(bufferManager.get());

    ASSERT_EQ(tb.getBufferSize(), tb2.getBufferSize());
    EXPECT_EQ(std::memcmp(tb.getBuffer(), tb2.getBuffer(), tb.getBufferSize()), 0);
}

TEST_F(JoinSerializableStateTest, JoinState_ManualContent_RoundTrip) {
    State::JoinState js{};
    js.metadata.operatorId = 42;
    js.metadata.operatorType = 2;
    js.metadata.processedRecords = 1234;
    js.metadata.lastWatermark = 7777;
    js.config.keySize = 8;
    js.config.valueSize = 16;
    js.config.numberOfBuckets = 64;
    js.config.pageSize = 4096;

    // Add one window referencing one left/right vector and two hash maps
    State::JoinState::VectorState left{};
    left.totalEntries = 5;
    left.entrySize = 16;
    left.pageSize = 4096;
    js.leftVectors.push_back(left);

    State::JoinState::VectorState right{};
    right.totalEntries = 7;
    right.entrySize = 16;
    right.pageSize = 4096;
    js.rightVectors.push_back(right);

    State::JoinState::HashMap hmL{};
    hmL.entrySize = 32;
    hmL.keySize = 8;
    hmL.valueSize = 16;
    hmL.bucketCount = 8;
    hmL.tupleCount = 2;
    hmL.chains = {0,0,0,0,0,0,0,0};
    js.hashMaps.push_back(hmL);

    State::JoinState::HashMap hmR = hmL;
    js.hashMaps.push_back(hmR);

    State::JoinState::Window w{};
    w.start = 1000;
    w.end = 2000;
    w.state = 0;
    w.firstLeftVectorIndex = 0;
    w.leftVectorCount = 1;
    w.firstRightVectorIndex = 0;
    w.rightVectorCount = 1;
    w.firstHashMapIndex = 0;
    w.hashMapCount = 2;
    js.windows.push_back(w);

    auto tb = Serialization::StateSerializer<State::JoinState>::serialize(js, bufferManager.get());
    auto deser = Serialization::StateSerializer<State::JoinState>::deserialize(tb);
    ASSERT_TRUE(deser.has_value());
    const auto& got = deser.value();
    EXPECT_EQ(got.metadata.operatorId, 42u);
    EXPECT_EQ(got.metadata.processedRecords, 1234u);
    ASSERT_EQ(got.windows.size(), 1u);
    EXPECT_EQ(got.windows[0].start, 1000u);
    EXPECT_EQ(got.leftVectors.size(), 1u);
    EXPECT_EQ(got.rightVectors.size(), 1u);
    EXPECT_EQ(got.hashMaps.size(), 2u);
}

