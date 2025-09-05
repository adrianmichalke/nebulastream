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
#include <vector>
#include <cstring>
#include <filesystem>

#include <Nautilus/Recovery/RecoveryManager.hpp>
#include <Nautilus/Recovery/CheckpointManager.hpp>
#include <Nautilus/Recovery/CheckpointCoordinator.hpp>
#include <Nautilus/State/Reflection/PipelineState.hpp>
#include <Nautilus/State/Reflection/StateDefinitions.hpp>
#include <Nautilus/State/Serialization/StateSerializer.hpp>
#include <Nautilus/DataStructures/OffsetBasedHashMap.hpp>

#include <Aggregation/SerializableAggregationOperatorHandler.hpp>
#include <Aggregation/SerializableAggregationSlice.hpp>
#include <SliceStore/DefaultTimeBasedSliceStore.hpp>
#include <Runtime/BufferManager.hpp>
#include <Identifiers/Identifiers.hpp>

using namespace NES;
using namespace NES::Recovery;
using namespace NES::State;

class AggregationRecoveryTest : public ::testing::Test {
protected:
    void SetUp() override {
        bufferManager = BufferManager::create(10 * 1024 * 1024, 100);
        testDir = std::filesystem::temp_directory_path() / ("agg_recovery_test_" + std::to_string(getpid()));
        std::filesystem::create_directories(testDir);
    }
    
    void TearDown() override {
        bufferManager.reset();
        std::filesystem::remove_all(testDir);
    }
    
    std::shared_ptr<BufferManager> bufferManager;
    std::filesystem::path testDir;
};

TEST_F(AggregationRecoveryTest, EmptyStateSerialization) {
    // Create an aggregation handler
    std::vector<OriginId> inputOrigins = {OriginId(1)};
    OriginId outputOrigin(2);
    auto sliceStore = std::make_unique<DefaultTimeBasedSliceStore>(1000, 1000);
    
    auto aggHandler = std::make_shared<SerializableAggregationOperatorHandler>(
        inputOrigins, outputOrigin, std::move(sliceStore));
    
    // Set up basic configuration
    auto& state = aggHandler->getState();
    state.config.keySize = sizeof(int64_t);
    state.config.valueSize = sizeof(int64_t);
    state.config.numberOfBuckets = 16;
    state.config.pageSize = 4096;
    
    // Add operations
    Operation op1{1, "sum", OperationParams(rfl::make_field<"sum">(SumParams{"field1", true}))};
    state.operations.push_back(op1);
    
    Operation op2{2, "count", OperationParams(rfl::make_field<"count">(CountParams{false}))};
    state.operations.push_back(op2);
    
    // Capture initial empty state
    aggHandler->captureState();
    
    // Serialize the state
    auto serialized = aggHandler->serialize(bufferManager.get());
    ASSERT_GT(serialized.getBufferSize(), 0);
    
    // Deserialize to new handler
    auto sliceStore2 = std::make_unique<DefaultTimeBasedSliceStore>(1000, 1000);
    auto recovered = SerializableAggregationOperatorHandler::deserialize(
        serialized, inputOrigins, outputOrigin, std::move(sliceStore2));
    
    ASSERT_NE(recovered, nullptr);
    
    // Verify state matches
    const auto& originalState = aggHandler->getState();
    const auto& recoveredState = recovered->getState();
    
    EXPECT_EQ(originalState.metadata.operatorType, recoveredState.metadata.operatorType);
    EXPECT_EQ(originalState.config.keySize, recoveredState.config.keySize);
    EXPECT_EQ(originalState.config.valueSize, recoveredState.config.valueSize);
    EXPECT_EQ(originalState.config.numberOfBuckets, recoveredState.config.numberOfBuckets);
    EXPECT_EQ(originalState.config.pageSize, recoveredState.config.pageSize);
    EXPECT_EQ(originalState.operations.size(), recoveredState.operations.size());
    EXPECT_EQ(originalState.hashMaps.size(), recoveredState.hashMaps.size());
    EXPECT_EQ(originalState.windows.size(), recoveredState.windows.size());
}

TEST_F(AggregationRecoveryTest, AggregationWithDataSerialization) {
    // Create aggregation handler with specific config
    std::vector<OriginId> inputOrigins = {OriginId(1)};
    OriginId outputOrigin(2);
    auto sliceStore = std::make_unique<DefaultTimeBasedSliceStore>(1000, 1000);
    
    auto aggHandler = std::make_shared<SerializableAggregationOperatorHandler>(
        inputOrigins, outputOrigin, std::move(sliceStore));
    
    // Set up configuration
    auto& state = aggHandler->getState();
    state.config.keySize = sizeof(int64_t);
    state.config.valueSize = sizeof(int64_t) * 2; // For sum and count
    state.config.numberOfBuckets = 32;
    state.config.pageSize = 8192;
    
    // Add operations
    Operation sumOp{1, "sum", OperationParams(rfl::make_field<"sum">(SumParams{"revenue", true}))};
    state.operations.push_back(sumOp);
    
    Operation countOp{2, "count", OperationParams(rfl::make_field<"count">(CountParams{false}))};
    state.operations.push_back(countOp);
    
    Operation avgOp{3, "avg", OperationParams(rfl::make_field<"avg">(AvgParams{"price", false}))};
    state.operations.push_back(avgOp);
    
    // Create/register slice via handler factory and then populate it
    CreateNewHashMapSliceArgs args{{}, sizeof(int64_t), sizeof(int64_t) * 2, 8192, 32};
    auto created = aggHandler->getSliceAndWindowStore().getSlicesOrCreate(
        Timestamp(1000), aggHandler->getCreateNewSlicesFunction(args));
    ASSERT_FALSE(created.empty());
    auto slice = std::dynamic_pointer_cast<SerializableAggregationSlice>(created.front());
    ASSERT_TRUE(static_cast<bool>(slice));

    // Add data to hash map (mutate slice stored in slice store)
    auto& hashMapState = slice->getHashMapState(WorkerThreadId(0));
    hashMapState.tupleCount = 100;
    hashMapState.memory.resize(4096);
    // Fill with pattern data
    for (size_t i = 0; i < hashMapState.memory.size(); ++i) {
        hashMapState.memory[i] = static_cast<uint8_t>((i * 3 + 7) % 256);
    }
    hashMapState.chains.resize(32, 0);
    hashMapState.keySize = sizeof(int64_t);
    hashMapState.valueSize = sizeof(int64_t) * 2;
    hashMapState.bucketCount = 32;
    hashMapState.entrySize = sizeof(OffsetEntry) + sizeof(int64_t) + sizeof(int64_t) * 2;
    
    // Materialize state directly on handler for serialization
    state.hashMaps.clear();
    state.windows.clear();
    state.hashMaps.push_back({hashMapState.memory, hashMapState.chains, hashMapState.entrySize, 
                              hashMapState.tupleCount, hashMapState.keySize, hashMapState.valueSize,
                              hashMapState.bucketCount, hashMapState.varSizedMemory, hashMapState.varSizedOffset});
    state.windows.push_back({1000, 2000, 0, 0, 1, 0, 0});
    state.metadata.processedRecords = 100;
    state.metadata.lastWatermark = 2000;
    
    // Verify state was set
    ASSERT_EQ(aggHandler->getState().hashMaps.size(), 1);
    EXPECT_EQ(aggHandler->getState().hashMaps[0].tupleCount, 100);
    
    // Serialize
    auto serialized = aggHandler->serialize(bufferManager.get());
    
    // Deserialize
    auto sliceStore2 = std::make_unique<DefaultTimeBasedSliceStore>(1000, 1000);
    auto recovered = SerializableAggregationOperatorHandler::deserialize(
        serialized, inputOrigins, outputOrigin, std::move(sliceStore2));
    
    // Verify recovered state
    ASSERT_EQ(recovered->getState().hashMaps.size(), 1);
    EXPECT_EQ(recovered->getState().hashMaps[0].tupleCount, 100);
    EXPECT_EQ(recovered->getState().hashMaps[0].memory.size(), 4096);
    EXPECT_EQ(recovered->getState().hashMaps[0].keySize, sizeof(int64_t));
    EXPECT_EQ(recovered->getState().hashMaps[0].valueSize, sizeof(int64_t) * 2);
    EXPECT_EQ(recovered->getState().operations.size(), 3);
    EXPECT_EQ(recovered->getState().metadata.processedRecords, 100);
    EXPECT_EQ(recovered->getState().metadata.lastWatermark, 2000);
    
    // Verify memory content pattern
    for (size_t i = 0; i < 100; ++i) {
        EXPECT_EQ(recovered->getState().hashMaps[0].memory[i], 
                  static_cast<uint8_t>((i * 3 + 7) % 256));
    }
}

TEST_F(AggregationRecoveryTest, WatermarkPreservation) {
    // Create aggregation handler
    std::vector<OriginId> inputOrigins = {OriginId(1), OriginId(2)};
    OriginId outputOrigin(3);
    auto sliceStore = std::make_unique<DefaultTimeBasedSliceStore>(1000, 1000);
    
    auto aggHandler = std::make_shared<SerializableAggregationOperatorHandler>(
        inputOrigins, outputOrigin, std::move(sliceStore));
    
    // Set watermark and other metadata
    auto& state = aggHandler->getState();
    state.metadata.lastWatermark = 12345;
    state.metadata.processedRecords = 5000;
    state.metadata.droppedRecords = 10;
    
    // Set configuration
    state.config.keySize = sizeof(int32_t);
    state.config.valueSize = sizeof(int64_t);
    state.config.numberOfBuckets = 16;
    state.config.pageSize = 4096;
    
    // Add an operation
    Operation minOp{1, "min", OperationParams(rfl::make_field<"min">(MinParams{"temperature", true}))};
    state.operations.push_back(minOp);
    
    // Capture and serialize
    aggHandler->captureState();
    auto serialized = aggHandler->serialize(bufferManager.get());
    
    // Deserialize
    auto sliceStore2 = std::make_unique<DefaultTimeBasedSliceStore>(1000, 1000);
    auto recovered = SerializableAggregationOperatorHandler::deserialize(
        serialized, inputOrigins, outputOrigin, std::move(sliceStore2));
    
    // Verify watermark and metadata preserved
    EXPECT_EQ(recovered->getState().metadata.lastWatermark, 12345);
    EXPECT_EQ(recovered->getState().metadata.processedRecords, 5000);
    EXPECT_EQ(recovered->getState().metadata.droppedRecords, 10);
}

TEST_F(AggregationRecoveryTest, MultipleWindowsRecovery) {
    // Create aggregation handler
    std::vector<OriginId> inputOrigins = {OriginId(1)};
    OriginId outputOrigin(2);
    auto sliceStore = std::make_unique<DefaultTimeBasedSliceStore>(1000, 1000);
    
    auto aggHandler = std::make_shared<SerializableAggregationOperatorHandler>(
        inputOrigins, outputOrigin, std::move(sliceStore));
    
    auto& state = aggHandler->getState();
    
    // Set configuration
    state.config.keySize = sizeof(int64_t);
    state.config.valueSize = sizeof(int64_t);
    state.config.numberOfBuckets = 16;
    state.config.pageSize = 4096;
    
    // Add operations
    Operation maxOp{1, "max", OperationParams(rfl::make_field<"max">(MaxParams{"score", false}))};
    state.operations.push_back(maxOp);
    
    // Add multiple windows to state
    state.windows.push_back({1000, 2000, 0, 0, 2, 0, 0});  // Window 1: 2 hash maps
    state.windows.push_back({2000, 3000, 0, 2, 3, 0, 0});  // Window 2: 3 hash maps
    state.windows.push_back({3000, 4000, 0, 5, 1, 0, 0});  // Window 3: 1 hash map
    
    // Add hash maps for each window (total: 2 + 3 + 1 = 6)
    for (int i = 0; i < 6; ++i) {
        AggregationState::HashMap hm{};
        hm.keySize = sizeof(int64_t);
        hm.valueSize = sizeof(int64_t);
        hm.bucketCount = 16;
        hm.entrySize = sizeof(OffsetEntry) + sizeof(int64_t) + sizeof(int64_t);
        hm.tupleCount = 10 * (i + 1);  // Different tuple counts
        hm.memory.resize(512 * (i + 1));  // Different memory sizes
        hm.chains.resize(16, 0);
        
        // Fill with unique pattern
        for (size_t j = 0; j < hm.memory.size(); ++j) {
            hm.memory[j] = static_cast<uint8_t>((i * 17 + j * 3) % 256);
        }
        
        state.hashMaps.push_back(hm);
    }
    
    state.metadata.processedRecords = 600;
    state.metadata.lastWatermark = 4000;
    
    // Serialize
    auto serialized = aggHandler->serialize(bufferManager.get());
    
    // Deserialize
    auto sliceStore2 = std::make_unique<DefaultTimeBasedSliceStore>(1000, 1000);
    auto recovered = SerializableAggregationOperatorHandler::deserialize(
        serialized, inputOrigins, outputOrigin, std::move(sliceStore2));
    
    // Verify windows
    ASSERT_EQ(recovered->getState().windows.size(), 3);
    EXPECT_EQ(recovered->getState().windows[0].start, 1000);
    EXPECT_EQ(recovered->getState().windows[0].end, 2000);
    EXPECT_EQ(recovered->getState().windows[0].hashMapCount, 2);
    EXPECT_EQ(recovered->getState().windows[1].start, 2000);
    EXPECT_EQ(recovered->getState().windows[1].end, 3000);
    EXPECT_EQ(recovered->getState().windows[1].hashMapCount, 3);
    EXPECT_EQ(recovered->getState().windows[2].start, 3000);
    EXPECT_EQ(recovered->getState().windows[2].end, 4000);
    EXPECT_EQ(recovered->getState().windows[2].hashMapCount, 1);
    
    // Verify hash maps
    ASSERT_EQ(recovered->getState().hashMaps.size(), 6);
    for (int i = 0; i < 6; ++i) {
        EXPECT_EQ(recovered->getState().hashMaps[i].tupleCount, 10 * (i + 1));
        EXPECT_EQ(recovered->getState().hashMaps[i].memory.size(), 512 * (i + 1));
        // Verify first few bytes of pattern
        EXPECT_EQ(recovered->getState().hashMaps[i].memory[0], 
                  static_cast<uint8_t>((i * 17) % 256));
    }
    
    EXPECT_EQ(recovered->getState().metadata.processedRecords, 600);
    EXPECT_EQ(recovered->getState().metadata.lastWatermark, 4000);
}

TEST_F(AggregationRecoveryTest, ComplexOperationsRecovery) {
    // Create aggregation handler with all operation types
    std::vector<OriginId> inputOrigins = {OriginId(1)};
    OriginId outputOrigin(2);
    auto sliceStore = std::make_unique<DefaultTimeBasedSliceStore>(1000, 1000);
    
    auto aggHandler = std::make_shared<SerializableAggregationOperatorHandler>(
        inputOrigins, outputOrigin, std::move(sliceStore));
    
    auto& state = aggHandler->getState();
    
    // Set configuration
    state.config.keySize = sizeof(int64_t);
    state.config.valueSize = sizeof(int64_t) * 10;  // Space for multiple operations
    state.config.numberOfBuckets = 64;
    state.config.pageSize = 16384;
    
    // Add all types of operations
    Operation sumOp{1, "sum", OperationParams(rfl::make_field<"sum">(SumParams{"sales", true}))};
    state.operations.push_back(sumOp);
    
    Operation countOp{2, "count", OperationParams(rfl::make_field<"count">(CountParams{true}))};
    state.operations.push_back(countOp);
    
    Operation avgOp{3, "avg", OperationParams(rfl::make_field<"avg">(AvgParams{"rating", true}))};
    state.operations.push_back(avgOp);
    
    Operation minOp{4, "min", OperationParams(rfl::make_field<"min">(MinParams{"price", false}))};
    state.operations.push_back(minOp);
    
    Operation maxOp{5, "max", OperationParams(rfl::make_field<"max">(MaxParams{"quantity", true}))};
    state.operations.push_back(maxOp);
    
    // Serialize
    auto serialized = aggHandler->serialize(bufferManager.get());
    
    // Deserialize
    auto sliceStore2 = std::make_unique<DefaultTimeBasedSliceStore>(1000, 1000);
    auto recovered = SerializableAggregationOperatorHandler::deserialize(
        serialized, inputOrigins, outputOrigin, std::move(sliceStore2));
    
    // Verify all operations recovered
    ASSERT_EQ(recovered->getState().operations.size(), 5);
    
    const auto& ops = recovered->getState().operations;
    EXPECT_EQ(ops[0].id, 1);
    EXPECT_EQ(ops[0].name, "sum");
    EXPECT_EQ(ops[1].id, 2);
    EXPECT_EQ(ops[1].name, "count");
    EXPECT_EQ(ops[2].id, 3);
    EXPECT_EQ(ops[2].name, "avg");
    EXPECT_EQ(ops[3].id, 4);
    EXPECT_EQ(ops[3].name, "min");
    EXPECT_EQ(ops[4].id, 5);
    EXPECT_EQ(ops[4].name, "max");
    
    // Verify configuration
    EXPECT_EQ(recovered->getState().config.keySize, sizeof(int64_t));
    EXPECT_EQ(recovered->getState().config.valueSize, sizeof(int64_t) * 10);
    EXPECT_EQ(recovered->getState().config.numberOfBuckets, 64);
    EXPECT_EQ(recovered->getState().config.pageSize, 16384);
}

TEST_F(AggregationRecoveryTest, RecoveryManagerIntegrationWithAggregation) {
    // Create a pipeline state with aggregation operator
    PipelineState ps{};
    ps.version = 1;
    ps.queryId = 456;
    ps.pipelineId = 1;
    ps.createdTimestampNs = 2000000;
    
    // Add aggregation operator blob
    OperatorStateBlob aggBlob{};
    aggBlob.header.operatorId = 10;
    aggBlob.header.kind = OperatorStateTag::Kind::Aggregation;
    aggBlob.header.version = 1;
    
    // Create and serialize aggregation state
    AggregationState aggState{};
    aggState.metadata.operatorType = 1;
    aggState.metadata.lastWatermark = 7500;
    aggState.metadata.processedRecords = 1500;
    aggState.config.keySize = 8;
    aggState.config.valueSize = 16;
    aggState.config.numberOfBuckets = 32;
    aggState.config.pageSize = 4096;
    
    // Add operations
    Operation op1{1, "sum", OperationParams(rfl::make_field<"sum">(SumParams{"amount", true}))};
    aggState.operations.push_back(op1);
    Operation op2{2, "count", OperationParams(rfl::make_field<"count">(CountParams{false}))};
    aggState.operations.push_back(op2);
    
    // Add a window with hash map
    aggState.windows.push_back({5000, 10000, 0, 0, 1, 0, 0});
    
    AggregationState::HashMap hm{};
    hm.keySize = 8;
    hm.valueSize = 16;
    hm.bucketCount = 32;
    hm.entrySize = sizeof(OffsetEntry) + 8 + 16;
    hm.tupleCount = 250;
    hm.memory.resize(2048);
    hm.chains.resize(32, 0);
    
    // Fill with test data
    for (size_t i = 0; i < hm.memory.size(); ++i) {
        hm.memory[i] = static_cast<uint8_t>((i * 7 + 13) % 256);
    }
    
    aggState.hashMaps.push_back(hm);
    
    auto aggBuffer = Serialization::StateSerializer<AggregationState>::serialize(aggState, bufferManager.get());
    aggBlob.bytes.resize(aggBuffer.getBufferSize());
    std::memcpy(aggBlob.bytes.data(), aggBuffer.getBuffer(), aggBuffer.getBufferSize());
    ps.operators.push_back(aggBlob);
    
    // Add progress metadata
    ps.progress.version = 1;
    ps.progress.lastWatermark = 7500;
    
    ProgressMetadata::OriginProgress origin{};
    origin.originId = 100;
    origin.processedRecords = 1500;
    origin.lastWatermark = 7500;
    ps.progress.origins.push_back(origin);
    
    // Write checkpoint
    auto checkpointPath = testDir / "agg_integration.checkpoint";
    CheckpointCoordinator::writeCheckpoint(ps, checkpointPath);
    
    // Set up recovery args
    RecoveryManager::RecreateArgs args{};
    args.bufferProvider = bufferManager.get();
    
    // Add construction args for aggregation operator
    args.operatorArgsById[10] = {
        .inputOrigins = {OriginId(100)},
        .outputOriginId = OriginId(101),
        .makeSliceStore = []() { return std::make_unique<DefaultTimeBasedSliceStore>(1000, 1000); }
    };
    
    // Recover pipeline
    auto result = RecoveryManager::recoverPipeline(checkpointPath, args);
    
    // Verify recovery
    ASSERT_EQ(result.handlers.size(), 1);
    EXPECT_EQ(result.pipelineState.operators.size(), 1);
    EXPECT_EQ(result.pipelineState.queryId, 456);
    
    // Verify aggregation handler
    auto* aggHandler = dynamic_cast<SerializableAggregationOperatorHandler*>(result.handlers[0].get());
    ASSERT_NE(aggHandler, nullptr);
    
    const auto& recoveredState = aggHandler->getState();
    EXPECT_EQ(recoveredState.metadata.lastWatermark, 7500);
    EXPECT_EQ(recoveredState.metadata.processedRecords, 1500);
    EXPECT_EQ(recoveredState.operations.size(), 2);
    EXPECT_EQ(recoveredState.windows.size(), 1);
    EXPECT_EQ(recoveredState.hashMaps.size(), 1);
    EXPECT_EQ(recoveredState.hashMaps[0].tupleCount, 250);
    
    // Verify operations
    EXPECT_EQ(recoveredState.operations[0].name, "sum");
    EXPECT_EQ(recoveredState.operations[1].name, "count");
    
    // Verify memory pattern
    EXPECT_EQ(recoveredState.hashMaps[0].memory[0], static_cast<uint8_t>(13 % 256));
    EXPECT_EQ(recoveredState.hashMaps[0].memory[1], static_cast<uint8_t>(20 % 256));
}

TEST_F(AggregationRecoveryTest, LargeStateRecovery) {
    // Test with very large aggregation state
    std::vector<OriginId> inputOrigins = {OriginId(1)};
    OriginId outputOrigin(2);
    auto sliceStore = std::make_unique<DefaultTimeBasedSliceStore>(1000, 1000);
    
    auto aggHandler = std::make_shared<SerializableAggregationOperatorHandler>(
        inputOrigins, outputOrigin, std::move(sliceStore));
    
    auto& state = aggHandler->getState();
    
    // Large configuration
    state.config.keySize = sizeof(int64_t);
    state.config.valueSize = sizeof(int64_t) * 5;
    state.config.numberOfBuckets = 1024;
    state.config.pageSize = 65536;
    
    // Add operation
    Operation sumOp{1, "sum", OperationParams(rfl::make_field<"sum">(SumParams{"large_field", true}))};
    state.operations.push_back(sumOp);
    
    // Add many windows
    for (int w = 0; w < 10; ++w) {
        state.windows.push_back({
            static_cast<uint64_t>(w * 1000), 
            static_cast<uint64_t>((w + 1) * 1000),
            0,
            static_cast<uint64_t>(w * 5),
            5,
            0,
            0
        });
        
        // Add 5 hash maps per window
        for (int h = 0; h < 5; ++h) {
            AggregationState::HashMap hm{};
            hm.keySize = sizeof(int64_t);
            hm.valueSize = sizeof(int64_t) * 5;
            hm.bucketCount = 1024;
            hm.entrySize = sizeof(OffsetEntry) + sizeof(int64_t) + sizeof(int64_t) * 5;
            hm.tupleCount = 1000 + w * 100 + h * 10;
            
            // Large memory allocation
            hm.memory.resize(65536);
            hm.chains.resize(1024, 0);
            
            // Fill with pattern
            for (size_t i = 0; i < hm.memory.size(); ++i) {
                hm.memory[i] = static_cast<uint8_t>((w * 256 + h * 16 + i) % 256);
            }
            
            state.hashMaps.push_back(hm);
        }
    }
    
    state.metadata.processedRecords = 100000;
    state.metadata.lastWatermark = 10000;
    
    // Serialize
    auto serialized = aggHandler->serialize(bufferManager.get());
    
    // Should be large
    EXPECT_GT(serialized.getBufferSize(), 3 * 1024 * 1024);  // > 3MB
    
    // Deserialize
    auto sliceStore2 = std::make_unique<DefaultTimeBasedSliceStore>(1000, 1000);
    auto recovered = SerializableAggregationOperatorHandler::deserialize(
        serialized, inputOrigins, outputOrigin, std::move(sliceStore2));
    
    // Verify recovery
    ASSERT_EQ(recovered->getState().windows.size(), 10);
    ASSERT_EQ(recovered->getState().hashMaps.size(), 50);
    EXPECT_EQ(recovered->getState().metadata.processedRecords, 100000);
    
    // Spot check some hash maps
    EXPECT_EQ(recovered->getState().hashMaps[0].tupleCount, 1000);
    EXPECT_EQ(recovered->getState().hashMaps[49].tupleCount, 1490);
    EXPECT_EQ(recovered->getState().hashMaps[25].memory.size(), 65536);
}

TEST_F(AggregationRecoveryTest, VariableSizedDataRecovery) {
    // Test with variable-sized keys/values
    std::vector<OriginId> inputOrigins = {OriginId(1)};
    OriginId outputOrigin(2);
    auto sliceStore = std::make_unique<DefaultTimeBasedSliceStore>(1000, 1000);
    
    auto aggHandler = std::make_shared<SerializableAggregationOperatorHandler>(
        inputOrigins, outputOrigin, std::move(sliceStore));
    
    auto& state = aggHandler->getState();
    
    // Variable-sized configuration (key_size = 0 indicates variable)
    state.config.keySize = 0;  // Variable-sized keys
    state.config.valueSize = sizeof(int64_t);
    state.config.numberOfBuckets = 16;
    state.config.pageSize = 4096;
    
    // Add operation
    Operation countOp{1, "count", OperationParams(rfl::make_field<"count">(CountParams{false}))};
    state.operations.push_back(countOp);
    
    // Create hash map with variable-sized data
    AggregationState::HashMap hm{};
    hm.keySize = 0;  // Variable
    hm.valueSize = sizeof(int64_t);
    hm.bucketCount = 16;
    hm.entrySize = 0;  // Variable
    hm.tupleCount = 25;
    
    // Regular memory for fixed parts
    hm.memory.resize(1024);
    hm.chains.resize(16, 0);
    
    // Variable-sized memory for string keys
    hm.varSizedMemory.resize(512);
    hm.varSizedOffset = 256;  // Current offset in var-sized memory
    
    // Fill with test data
    for (size_t i = 0; i < hm.varSizedMemory.size(); ++i) {
        hm.varSizedMemory[i] = static_cast<uint8_t>((i * 11) % 256);
    }
    
    state.hashMaps.push_back(hm);
    state.windows.push_back({1000, 2000, 0, 0, 1, 0, 0});
    
    // Serialize
    auto serialized = aggHandler->serialize(bufferManager.get());
    
    // Deserialize
    auto sliceStore2 = std::make_unique<DefaultTimeBasedSliceStore>(1000, 1000);
    auto recovered = SerializableAggregationOperatorHandler::deserialize(
        serialized, inputOrigins, outputOrigin, std::move(sliceStore2));
    
    // Verify variable-sized data recovered
    ASSERT_EQ(recovered->getState().hashMaps.size(), 1);
    const auto& recoveredHM = recovered->getState().hashMaps[0];
    EXPECT_EQ(recoveredHM.keySize, 0);  // Variable-sized
    EXPECT_EQ(recoveredHM.varSizedMemory.size(), 512);
    EXPECT_EQ(recoveredHM.varSizedOffset, 256);
    EXPECT_EQ(recoveredHM.tupleCount, 25);
    
    // Verify var-sized memory content
    for (size_t i = 0; i < 10; ++i) {
        EXPECT_EQ(recoveredHM.varSizedMemory[i], static_cast<uint8_t>((i * 11) % 256));
    }
}