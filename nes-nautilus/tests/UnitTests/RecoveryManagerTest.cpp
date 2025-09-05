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
#include <filesystem>
#include <chrono>

#include <Nautilus/Recovery/RecoveryManager.hpp>
#include <Nautilus/Recovery/CheckpointCoordinator.hpp>
#include <Nautilus/Recovery/CheckpointManager.hpp>
#include <Nautilus/State/Reflection/PipelineState.hpp>
#include <Nautilus/State/Reflection/StateDefinitions.hpp>
#include <Nautilus/State/Serialization/StateSerializer.hpp>
#include <Nautilus/DataStructures/OffsetBasedHashMap.hpp>
#include <Nautilus/DataStructures/SerializablePagedVector.hpp>

#include <Aggregation/SerializableAggregationOperatorHandler.hpp>
#include <Join/HashJoin/SerializableHJOperatorHandler.hpp>
#include <Join/NestedLoopJoin/SerializableNLJOperatorHandler.hpp>
#include <SliceStore/DefaultTimeBasedSliceStore.hpp>
#include <Runtime/BufferManager.hpp>
#include <Identifiers/Identifiers.hpp>
#include <ErrorHandling.hpp>

using namespace NES;
using namespace NES::State;
using namespace NES::Recovery;
using namespace NES::DataStructures;

class RecoveryManagerTest : public ::testing::Test {
protected:
    void SetUp() override {
        bufferManager = BufferManager::create(10 * 1024 * 1024, 100);
        testDir = std::filesystem::temp_directory_path() / "recovery_test";
        std::filesystem::create_directories(testDir);
    }
    
    void TearDown() override {
        bufferManager.reset();
        std::filesystem::remove_all(testDir);
    }
    
    std::shared_ptr<BufferManager> bufferManager;
    std::filesystem::path testDir;
    
    std::shared_ptr<SerializableAggregationOperatorHandler> createAggregationHandler() {
        std::vector<OriginId> inputOrigins = {OriginId(1)};
        auto outputOrigin = OriginId(2);
        auto sliceStore = std::make_unique<DefaultTimeBasedSliceStore>(1000, 1000);
        
        auto handler = std::make_shared<SerializableAggregationOperatorHandler>(
            inputOrigins, outputOrigin, std::move(sliceStore));
        
        auto& state = handler->getState();
        state.config.keySize = sizeof(int64_t);
        state.config.valueSize = sizeof(int64_t);
        state.config.numberOfBuckets = 16;
        state.config.pageSize = 4096;
        
        Operation op1{1, "sum", OperationParams(rfl::make_field<"sum">(SumParams{"field1", true}))};
        state.operations.push_back(op1);
        
        Operation op2{2, "count", OperationParams(rfl::make_field<"count">(CountParams{false}))};
        state.operations.push_back(op2);
        
        return handler;
    }
    
    void populateAggregationData(SerializableAggregationOperatorHandler* handler) {
        AggregationState::HashMap hm1;
        hm1.keySize = 8;
        hm1.valueSize = 8;
        hm1.bucketCount = 16;
        hm1.entrySize = sizeof(OffsetEntry) + 8 + 8;
        hm1.chains.resize(16, 0);
        hm1.tupleCount = 10;
        
        hm1.memory.resize(1024);
        for (size_t i = 0; i < 100; ++i) {
            hm1.memory[i] = static_cast<uint8_t>(i % 256);
        }
        
        auto& state = handler->getState();
        state.hashMaps.push_back(hm1);
        
        AggregationState::Window w1;
        w1.start = 1000;
        w1.end = 2000;
        w1.state = 0;
        w1.firstHashMapIndex = 0;
        w1.hashMapCount = 1;
        state.windows.push_back(w1);
        
        state.metadata.processedRecords = 100;
        state.metadata.lastWatermark = 2000;
    }
};

TEST_F(RecoveryManagerTest, BasicAggregationRecoveryTest) {
    auto originalHandler = createAggregationHandler();
    populateAggregationData(originalHandler.get());
    
    PipelineState ps;
    ps.version = 1;
    ps.queryId = 42;
    ps.pipelineId = 1;
    ps.createdTimestampNs = std::chrono::system_clock::now().time_since_epoch().count();
    
    originalHandler->captureState();
    auto serialized = originalHandler->serialize(bufferManager.get());
    
    OperatorStateBlob blob;
    blob.header.operatorId = 1;
    blob.header.kind = OperatorStateTag::Kind::Aggregation;
    blob.header.version = 1;
    blob.bytes.resize(serialized.getBufferSize());
    std::memcpy(blob.bytes.data(), serialized.getBuffer(), serialized.getBufferSize());
    ps.operators.push_back(blob);
    
    ps.progress.version = 1;
    ps.progress.lastWatermark = 2000;
    
    std::string checkpointPath = (testDir / "test.checkpoint").string();
    CheckpointCoordinator::writeCheckpoint(ps, checkpointPath);
    
    RecoveryManager::RecreateArgs args;
    args.bufferProvider = bufferManager.get();
    
    std::vector<OriginId> inputOrigins = {OriginId(1)};
    auto outputOrigin = OriginId(1);
    args.operatorArgsById[1] = OperatorConstructionArgs{
        .inputOrigins = inputOrigins,
        .outputOriginId = outputOrigin,
        .makeSliceStore = []() { return std::make_unique<DefaultTimeBasedSliceStore>(1000, 1000); }
    };
    
    auto result = RecoveryManager::recoverPipeline(checkpointPath, args);
    
    ASSERT_EQ(result.handlers.size(), 1);
    ASSERT_EQ(result.pipelineState.operators.size(), 1);
    
    auto* recoveredHandler = dynamic_cast<SerializableAggregationOperatorHandler*>(result.handlers[0].get());
    ASSERT_NE(recoveredHandler, nullptr);
    
    const auto& recoveredState = recoveredHandler->getState();
    const auto& originalState = originalHandler->getState();
    
    EXPECT_EQ(recoveredState.metadata.processedRecords, originalState.metadata.processedRecords);
    EXPECT_EQ(recoveredState.metadata.lastWatermark, originalState.metadata.lastWatermark);
    EXPECT_EQ(recoveredState.operations.size(), originalState.operations.size());
    EXPECT_EQ(recoveredState.hashMaps.size(), originalState.hashMaps.size());
    EXPECT_EQ(recoveredState.windows.size(), originalState.windows.size());
    
    if (!recoveredState.hashMaps.empty()) {
        EXPECT_EQ(recoveredState.hashMaps[0].tupleCount, originalState.hashMaps[0].tupleCount);
        EXPECT_EQ(recoveredState.hashMaps[0].memory.size(), originalState.hashMaps[0].memory.size());
    }
}

TEST_F(RecoveryManagerTest, HashJoinRecoveryTest) {
    std::vector<OriginId> inputOrigins = {OriginId(1), OriginId(1)};
    auto outputOrigin = OriginId(1);
    auto sliceStore = std::make_unique<DefaultTimeBasedSliceStore>(1000, 1000);
    
    auto originalHandler = std::make_shared<SerializableHJOperatorHandler>(
        inputOrigins, outputOrigin, std::move(sliceStore));
    
    auto& state = originalHandler->getState();
    state.config.keySize = 8;
    state.config.valueSize = 16;
    state.config.numberOfBuckets = 32;
    state.config.pageSize = 8192;
    
    JoinState::HashMap leftHM;
    leftHM.keySize = 8;
    leftHM.valueSize = 16;
    leftHM.bucketCount = 32;
    leftHM.tupleCount = 50;
    leftHM.memory.resize(2048);
    leftHM.chains.resize(32, 0);
    
    JoinState::HashMap rightHM;
    rightHM.keySize = 8;
    rightHM.valueSize = 16;
    rightHM.bucketCount = 32;
    rightHM.tupleCount = 75;
    rightHM.memory.resize(2048);
    rightHM.chains.resize(32, 0);
    
    state.hashMaps.push_back(leftHM);
    state.hashMaps.push_back(rightHM);
    
    JoinState::Window w;
    w.start = 5000;
    w.end = 10000;
    w.firstHashMapIndex = 0;
    w.hashMapCount = 2;
    state.windows.push_back(w);
    
    state.metadata.processedRecords = 125;
    state.metadata.lastWatermark = 10000;
    
    PipelineState ps;
    ps.version = 1;
    ps.queryId = 43;
    
    originalHandler->captureState();
    auto serialized = originalHandler->serialize(bufferManager.get());
    
    OperatorStateBlob blob;
    blob.header.operatorId = 2;
    blob.header.kind = OperatorStateTag::Kind::HashJoin;
    blob.header.version = 1;
    blob.bytes.resize(serialized.getBufferSize());
    std::memcpy(blob.bytes.data(), serialized.getBuffer(), serialized.getBufferSize());
    ps.operators.push_back(blob);
    
    ps.progress.lastWatermark = 10000;
    
    std::string checkpointPath = (testDir / "hj_test.checkpoint").string();
    CheckpointCoordinator::writeCheckpoint(ps, checkpointPath);
    
    RecoveryManager::RecreateArgs args;
    args.bufferProvider = bufferManager.get();
    args.operatorArgsById[2] = OperatorConstructionArgs{
        .inputOrigins = inputOrigins,
        .outputOriginId = outputOrigin,
        .makeSliceStore = []() { return std::make_unique<DefaultTimeBasedSliceStore>(1000, 1000); }
    };
    
    auto result = RecoveryManager::recoverPipeline(checkpointPath, args);
    
    ASSERT_EQ(result.handlers.size(), 1);
    auto* recoveredHandler = dynamic_cast<SerializableHJOperatorHandler*>(result.handlers[0].get());
    ASSERT_NE(recoveredHandler, nullptr);
    
    const auto& recoveredState = recoveredHandler->getState();
    EXPECT_EQ(recoveredState.metadata.processedRecords, 125);
    EXPECT_EQ(recoveredState.metadata.lastWatermark, 10000);
    EXPECT_EQ(recoveredState.hashMaps.size(), 2);
    EXPECT_EQ(recoveredState.windows.size(), 1);
}

TEST_F(RecoveryManagerTest, NestedLoopJoinRecoveryTest) {
    std::vector<OriginId> inputOrigins = {OriginId(1), OriginId(1)};
    auto outputOrigin = OriginId(1);
    auto sliceStore = std::make_unique<DefaultTimeBasedSliceStore>(1000, 1000);
    
    auto originalHandler = std::make_shared<SerializableNLJOperatorHandler>(
        inputOrigins, outputOrigin, std::move(sliceStore));
    
    auto& state = originalHandler->getState();
    state.config.pageSize = 4096;
    
    PagedVectorState leftVec;
    leftVec.pageSize = 4096;
    leftVec.entrySize = 32;
    leftVec.totalEntries = 100;
    leftVec.pages.emplace_back();
    leftVec.pages[0].numberOfEntries = 100;
    leftVec.pages[0].buffer.resize(4096);
    leftVec.pages[0].bufferSize = 4096;
    
    PagedVectorState rightVec;
    rightVec.pageSize = 4096;
    rightVec.entrySize = 32;
    rightVec.totalEntries = 150;
    rightVec.pages.emplace_back();
    rightVec.pages[0].numberOfEntries = 150;
    rightVec.pages[0].buffer.resize(4096);
    rightVec.pages[0].bufferSize = 4096;
    
    state.leftVectors.push_back(leftVec);
    state.rightVectors.push_back(rightVec);
    
    JoinState::Window w;
    w.start = 2000;
    w.end = 4000;
    w.firstLeftVectorIndex = 0;
    w.leftVectorCount = 1;
    w.firstRightVectorIndex = 0;
    w.rightVectorCount = 1;
    state.windows.push_back(w);
    
    state.metadata.processedRecords = 250;
    state.metadata.lastWatermark = 4000;
    
    PipelineState ps;
    ps.version = 1;
    ps.queryId = 44;
    
    originalHandler->captureState();
    auto serialized = originalHandler->serialize(bufferManager.get());
    
    OperatorStateBlob blob;
    blob.header.operatorId = 3;
    blob.header.kind = OperatorStateTag::Kind::NestedLoopJoin;
    blob.header.version = 1;
    blob.bytes.resize(serialized.getBufferSize());
    std::memcpy(blob.bytes.data(), serialized.getBuffer(), serialized.getBufferSize());
    ps.operators.push_back(blob);
    
    ps.progress.lastWatermark = 4000;
    
    std::string checkpointPath = (testDir / "nlj_test.checkpoint").string();
    CheckpointCoordinator::writeCheckpoint(ps, checkpointPath);
    
    RecoveryManager::RecreateArgs args;
    args.bufferProvider = bufferManager.get();
    args.operatorArgsById[3] = OperatorConstructionArgs{
        .inputOrigins = inputOrigins,
        .outputOriginId = outputOrigin,
        .makeSliceStore = []() { return std::make_unique<DefaultTimeBasedSliceStore>(1000, 1000); }
    };
    
    auto result = RecoveryManager::recoverPipeline(checkpointPath, args);
    
    ASSERT_EQ(result.handlers.size(), 1);
    auto* recoveredHandler = dynamic_cast<SerializableNLJOperatorHandler*>(result.handlers[0].get());
    ASSERT_NE(recoveredHandler, nullptr);
    
    const auto& recoveredState = recoveredHandler->getState();
    EXPECT_EQ(recoveredState.metadata.processedRecords, 250);
    EXPECT_EQ(recoveredState.metadata.lastWatermark, 4000);
    EXPECT_EQ(recoveredState.leftVectors.size(), 1);
    EXPECT_EQ(recoveredState.rightVectors.size(), 1);
    EXPECT_EQ(recoveredState.windows.size(), 1);
}

TEST_F(RecoveryManagerTest, WatermarkPreservationTest) {
    auto originalHandler = createAggregationHandler();
    populateAggregationData(originalHandler.get());
    
    const uint64_t expectedWatermark = 12345;
    originalHandler->getState().metadata.lastWatermark = expectedWatermark;
    
    PipelineState ps;
    ps.version = 1;
    ps.queryId = 45;
    ps.progress.lastWatermark = expectedWatermark;
    
    ProgressMetadata::OriginProgress op1;
    op1.originId = 100;
    op1.processedRecords = 500;
    op1.lastWatermark = expectedWatermark - 100;
    ps.progress.origins.push_back(op1);
    
    ProgressMetadata::OriginProgress op2;
    op2.originId = 101;
    op2.processedRecords = 600;
    op2.lastWatermark = expectedWatermark;
    ps.progress.origins.push_back(op2);
    
    originalHandler->captureState();
    auto serialized = originalHandler->serialize(bufferManager.get());
    
    OperatorStateBlob blob;
    blob.header.operatorId = 4;
    blob.header.kind = OperatorStateTag::Kind::Aggregation;
    blob.bytes.resize(serialized.getBufferSize());
    std::memcpy(blob.bytes.data(), serialized.getBuffer(), serialized.getBufferSize());
    ps.operators.push_back(blob);
    
    std::string checkpointPath = (testDir / "watermark_test.checkpoint").string();
    CheckpointCoordinator::writeCheckpoint(ps, checkpointPath);
    
    RecoveryManager::RecreateArgs args;
    args.bufferProvider = bufferManager.get();
    
    std::vector<OriginId> inputOrigins = {OriginId(100), OriginId(101)};
    args.operatorArgsById[4] = OperatorConstructionArgs{
        .inputOrigins = inputOrigins,
        .outputOriginId = OriginId(1),
        .makeSliceStore = []() { return std::make_unique<DefaultTimeBasedSliceStore>(1000, 1000); }
    };
    
    auto result = RecoveryManager::recoverPipeline(checkpointPath, args);
    
    ASSERT_EQ(result.handlers.size(), 1);
    auto* recoveredHandler = dynamic_cast<SerializableAggregationOperatorHandler*>(result.handlers[0].get());
    ASSERT_NE(recoveredHandler, nullptr);
    
    EXPECT_EQ(recoveredHandler->getState().metadata.lastWatermark, expectedWatermark);
    
    EXPECT_EQ(result.pipelineState.progress.lastWatermark, expectedWatermark);
    EXPECT_EQ(result.pipelineState.progress.origins.size(), 2);
    EXPECT_EQ(result.pipelineState.progress.origins[0].lastWatermark, expectedWatermark - 100);
    EXPECT_EQ(result.pipelineState.progress.origins[1].lastWatermark, expectedWatermark);
}

TEST_F(RecoveryManagerTest, CorruptedCheckpointHandlingTest) {
    auto handler = createAggregationHandler();
    populateAggregationData(handler.get());
    
    PipelineState ps;
    ps.version = 1;
    ps.queryId = 46;
    
    handler->captureState();
    auto serialized = handler->serialize(bufferManager.get());
    
    OperatorStateBlob blob;
    blob.header.operatorId = 5;
    blob.header.kind = OperatorStateTag::Kind::Aggregation;
    blob.bytes.resize(serialized.getBufferSize());
    std::memcpy(blob.bytes.data(), serialized.getBuffer(), serialized.getBufferSize());
    ps.operators.push_back(blob);
    
    std::string checkpointPath = (testDir / "corrupt_test.checkpoint").string();
    CheckpointCoordinator::writeCheckpoint(ps, checkpointPath);
    
    {
        std::ofstream file(checkpointPath, std::ios::binary | std::ios::app);
        file << "CORRUPTED_DATA";
    }
    
    RecoveryManager::RecreateArgs args;
    args.bufferProvider = bufferManager.get();
    
    args.operatorArgsById[5] = OperatorConstructionArgs{
        .inputOrigins = {OriginId(1)},
        .outputOriginId = OriginId(1),
        .makeSliceStore = []() { return std::make_unique<DefaultTimeBasedSliceStore>(1000, 1000); }
    };
    
    EXPECT_THROW({
        auto result = RecoveryManager::recoverPipeline(checkpointPath, args);
    }, std::exception);
}

TEST_F(RecoveryManagerTest, MultipleOperatorRecoveryTest) {
    PipelineState ps;
    ps.version = 1;
    ps.queryId = 47;
    
    auto aggHandler = createAggregationHandler();
    populateAggregationData(aggHandler.get());
    aggHandler->captureState();
    auto aggSerialized = aggHandler->serialize(bufferManager.get());
    
    OperatorStateBlob aggBlob;
    aggBlob.header.operatorId = 10;
    aggBlob.header.kind = OperatorStateTag::Kind::Aggregation;
    aggBlob.bytes.resize(aggSerialized.getBufferSize());
    std::memcpy(aggBlob.bytes.data(), aggSerialized.getBuffer(), aggSerialized.getBufferSize());
    ps.operators.push_back(aggBlob);
    
    auto hjHandler = std::make_shared<SerializableHJOperatorHandler>(
        std::vector<OriginId>{OriginId(1), OriginId(1)},
        OriginId(1),
        std::make_unique<DefaultTimeBasedSliceStore>(1000, 1000));
    
    hjHandler->getState().metadata.processedRecords = 200;
    hjHandler->captureState();
    auto hjSerialized = hjHandler->serialize(bufferManager.get());
    
    OperatorStateBlob hjBlob;
    hjBlob.header.operatorId = 11;
    hjBlob.header.kind = OperatorStateTag::Kind::HashJoin;
    hjBlob.bytes.resize(hjSerialized.getBufferSize());
    std::memcpy(hjBlob.bytes.data(), hjSerialized.getBuffer(), hjSerialized.getBufferSize());
    ps.operators.push_back(hjBlob);
    
    std::string checkpointPath = (testDir / "multi_test.checkpoint").string();
    CheckpointCoordinator::writeCheckpoint(ps, checkpointPath);
    
    RecoveryManager::RecreateArgs args;
    args.bufferProvider = bufferManager.get();
    
    args.operatorArgsById[10] = OperatorConstructionArgs{
        .inputOrigins = {OriginId(1)},
        .outputOriginId = OriginId(1),
        .makeSliceStore = []() { return std::make_unique<DefaultTimeBasedSliceStore>(1000, 1000); }
    };
    
    args.operatorArgsById[11] = OperatorConstructionArgs{
        .inputOrigins = {OriginId(1), OriginId(1)},
        .outputOriginId = OriginId(1),
        .makeSliceStore = []() { return std::make_unique<DefaultTimeBasedSliceStore>(1000, 1000); }
    };
    
    auto result = RecoveryManager::recoverPipeline(checkpointPath, args);
    
    ASSERT_EQ(result.handlers.size(), 2);
    ASSERT_EQ(result.pipelineState.operators.size(), 2);
    
    auto* recoveredAgg = dynamic_cast<SerializableAggregationOperatorHandler*>(result.handlers[0].get());
    ASSERT_NE(recoveredAgg, nullptr);
    EXPECT_EQ(recoveredAgg->getState().metadata.processedRecords, 100);
    
    auto* recoveredHJ = dynamic_cast<SerializableHJOperatorHandler*>(result.handlers[1].get());
    ASSERT_NE(recoveredHJ, nullptr);
    EXPECT_EQ(recoveredHJ->getState().metadata.processedRecords, 200);
}

TEST_F(RecoveryManagerTest, EmptyCheckpointTest) {
    PipelineState ps;
    ps.version = 1;
    ps.queryId = 48;
    ps.progress.lastWatermark = 0;
    
    std::string checkpointPath = (testDir / "empty_test.checkpoint").string();
    CheckpointCoordinator::writeCheckpoint(ps, checkpointPath);
    
    RecoveryManager::RecreateArgs args;
    args.bufferProvider = bufferManager.get();
    
    auto result = RecoveryManager::recoverPipeline(checkpointPath, args);
    
    EXPECT_EQ(result.handlers.size(), 0);
    EXPECT_EQ(result.pipelineState.operators.size(), 0);
    EXPECT_EQ(result.pipelineState.queryId, 48);
}

