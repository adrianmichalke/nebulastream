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

#include <gtest/gtest.h>
#include <memory>
#include <Nautilus/State/Reflection/StateDefinitions.hpp>
#include <Nautilus/State/Serialization/StateSerializer.hpp>
#include <Nautilus/Operations/SimpleAggregationOperator.hpp>
#include <Nautilus/Operations/OperationRegistry.hpp>
#include <Runtime/BufferManager.hpp>

using namespace NES;
using namespace NES::State;
using namespace NES::Serialization;
using namespace NES::Operations;

class StateSerializationTest : public ::testing::Test {
protected:
    void SetUp() override {
        bufferManager = BufferManager::create(1024 * 1024, 10);
    }
    
    void TearDown() override {
        bufferManager.reset();
    }
    
    std::shared_ptr<BufferManager> bufferManager;
};

TEST_F(StateSerializationTest, testBasicAggregationStateSerialization) {
    AggregationState originalState;
    originalState.metadata = {
        .operatorId = 42,
        .operatorType = 1,
        .processedRecords = 1000,
        .lastWatermark = 5000,
        .version = 1
    };
    
    originalState.config = {
        .keySize = 8,
        .valueSize = 16,
        .numberOfBuckets = 512,
        .pageSize = 4096
    };
    
    // Add a simple hashMap entry
    State::AggregationState::HashMap hashMap;
    hashMap.memory = {1, 2, 3, 4, 5};
    hashMap.chains = {0, 16, 32};
    hashMap.tupleCount = 100;
    hashMap.keySize = 8;
    hashMap.valueSize = 8;
    hashMap.bucketCount = 32;
    hashMap.entrySize = 32;
    originalState.hashMaps.push_back(std::move(hashMap));
    
    // Add a simple operation
    State::Operation op{
        1001,
        "sum",
        State::OperationParams{rfl::Field<"sum", State::SumParams>{State::SumParams{"testField", true}}}
    };
    originalState.operations.push_back(std::move(op));
    
    auto serialized = StateSerializer<AggregationState>::serialize(originalState, bufferManager.get());
    auto deserializedResult = StateSerializer<AggregationState>::deserialize(serialized);
    
    ASSERT_TRUE(deserializedResult.has_value());
    
    const auto& deserialized = deserializedResult.value();
    
    EXPECT_EQ(deserialized.metadata.operatorId, originalState.metadata.operatorId);
    EXPECT_EQ(deserialized.metadata.processedRecords, originalState.metadata.processedRecords);
    EXPECT_EQ(deserialized.config.keySize, originalState.config.keySize);
    EXPECT_EQ(deserialized.config.numberOfBuckets, originalState.config.numberOfBuckets);
    EXPECT_EQ(deserialized.hashMaps.size(), originalState.hashMaps.size());
    EXPECT_EQ(deserialized.hashMaps[0].tupleCount, originalState.hashMaps[0].tupleCount);
    EXPECT_EQ(deserialized.operations.size(), originalState.operations.size());
    EXPECT_EQ(deserialized.operations[0].name, originalState.operations[0].name);
}

TEST_F(StateSerializationTest, testZeroCopyStateView) {
    AggregationState originalState;
    originalState.metadata.operatorId = 123;
    originalState.metadata.processedRecords = 500;
    originalState.config.keySize = 4;
    originalState.config.valueSize = 8;
    
    auto serialized = StateSerializer<AggregationState>::serialize(originalState, bufferManager.get());
    
    StateView<AggregationState> view(serialized);
    
    EXPECT_EQ(view->metadata.operatorId, 123);
    EXPECT_EQ(view->metadata.processedRecords, 500);
    EXPECT_EQ(view->config.keySize, 4);
    EXPECT_EQ(view->config.valueSize, 8);
}

TEST_F(StateSerializationTest, testSimpleAggregationOperatorSerialization) {
    SimpleAggregationOperator op;
    
    TupleBuffer mockInput;
    for (int i = 0; i < 10; ++i) {
        op.processTuple(mockInput);
    }
    
    EXPECT_EQ(op.getState().metadata.processedRecords, 10);
    
    auto serialized = op.serialize(bufferManager.get());
    auto deserializedOp = SimpleAggregationOperator::deserialize(serialized);
    
    EXPECT_EQ(deserializedOp->getState().metadata.processedRecords, 10);
    EXPECT_EQ(deserializedOp->getState().operations.size(), op.getState().operations.size());
}

TEST_F(StateSerializationTest, testLargeStateRoundTrip) {
    AggregationState largeState;
    largeState.metadata.operatorId = 999;
    largeState.metadata.processedRecords = 100000;
    
    largeState.hashMaps.reserve(10);
    for (int i = 0; i < 10; ++i) {
        AggregationState::HashMap hashMap;
        hashMap.memory.resize(1024, static_cast<uint8_t>(i));
        hashMap.chains.resize(256, i * 100);
        hashMap.tupleCount = i * 1000;
        largeState.hashMaps.push_back(std::move(hashMap));
    }
    
    largeState.windows.reserve(50);
    for (int i = 0; i < 50; ++i) {
        largeState.windows.push_back({
            .start = static_cast<uint64_t>(i * 1000),
            .end = static_cast<uint64_t>((i + 1) * 1000),
            .state = static_cast<uint32_t>(i % 3)
        });
    }
    
    auto start = std::chrono::high_resolution_clock::now();
    auto serialized = StateSerializer<AggregationState>::serialize(largeState, bufferManager.get());
    auto serializationTime = std::chrono::high_resolution_clock::now() - start;
    
    start = std::chrono::high_resolution_clock::now();
    auto deserializedResult = StateSerializer<AggregationState>::deserialize(serialized);
    auto deserializationTime = std::chrono::high_resolution_clock::now() - start;
    
    ASSERT_TRUE(deserializedResult.has_value());
    
    const auto& deserialized = deserializedResult.value();
    EXPECT_EQ(deserialized.metadata.processedRecords, 100000);
    EXPECT_EQ(deserialized.hashMaps.size(), 10);
    EXPECT_EQ(deserialized.windows.size(), 50);
    
    auto serializationMs = std::chrono::duration_cast<std::chrono::milliseconds>(serializationTime).count();
    auto deserializationMs = std::chrono::duration_cast<std::chrono::milliseconds>(deserializationTime).count();
    
    std::cout << "Serialization time: " << serializationMs << "ms" << std::endl;
    std::cout << "Deserialization time: " << deserializationMs << "ms" << std::endl;
    std::cout << "Buffer size: " << serialized.getBufferSize() << " bytes" << std::endl;
    
    EXPECT_LT(deserializationMs, 50);
}

TEST_F(StateSerializationTest, testOperationRegistryIntegration) {
    SimpleAggregationOperator op;
    op.addOperation(OperationID::SUM_INT64, OperationParams{rfl::Field<"sum", SumParams>{SumParams{"testField", true}}});
    op.addOperation(OperationID::COUNT);
    
    TupleBuffer mockInput;
    op.processTuple(mockInput);
    
    auto serialized = op.serialize(bufferManager.get());
    auto deserializedOp = SimpleAggregationOperator::deserialize(serialized);
    
    EXPECT_EQ(deserializedOp->getState().operations.size(), 3);
    EXPECT_EQ(deserializedOp->getState().metadata.processedRecords, 1);
}