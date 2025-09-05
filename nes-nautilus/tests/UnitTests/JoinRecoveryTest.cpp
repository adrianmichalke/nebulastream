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

#include <Nautilus/Recovery/RecoveryManager.hpp>
#include <Nautilus/Recovery/CheckpointManager.hpp>
#include <Nautilus/Recovery/CheckpointCoordinator.hpp>
#include <Nautilus/State/Reflection/PipelineState.hpp>
#include <Nautilus/State/Reflection/StateDefinitions.hpp>
#include <Nautilus/State/Serialization/StateSerializer.hpp>
#include <Nautilus/DataStructures/OffsetBasedHashMap.hpp>
#include <Nautilus/DataStructures/SerializablePagedVector.hpp>

#include <Join/HashJoin/SerializableHJOperatorHandler.hpp>
#include <Join/HashJoin/SerializableHJSlice.hpp>
#include <Join/NestedLoopJoin/SerializableNLJOperatorHandler.hpp>
#include <Join/NestedLoopJoin/NLJSlice.hpp>
#include <SliceStore/DefaultTimeBasedSliceStore.hpp>
#include <Runtime/BufferManager.hpp>
#include <Identifiers/Identifiers.hpp>

using namespace NES;
using namespace NES::Recovery;
using namespace NES::State;

class JoinRecoveryTest : public ::testing::Test {
protected:
    void SetUp() override {
        bufferManager = BufferManager::create(1024 * 1024, 100);
        testDir = "/tmp/join_recovery_test_" + std::to_string(getpid());
        std::filesystem::create_directories(testDir);
    }
    
    void TearDown() override {
        std::filesystem::remove_all(testDir);
    }
    
    std::shared_ptr<BufferManager> bufferManager;
    std::filesystem::path testDir;
};

TEST_F(JoinRecoveryTest, HashJoinEmptyStateSerialization) {
    // Create a hash join handler
    std::vector<OriginId> inputOrigins = {OriginId(1), OriginId(2)};
    OriginId outputOrigin(3);
    auto sliceStore = std::make_unique<DefaultTimeBasedSliceStore>(1000, 1000);
    
    auto hjHandler = std::make_shared<SerializableHJOperatorHandler>(
        inputOrigins, outputOrigin, std::move(sliceStore));
    
    // Capture initial empty state
    hjHandler->captureState();
    
    // Serialize the state
    auto serialized = hjHandler->serialize(bufferManager.get());
    ASSERT_GT(serialized.getBufferSize(), 0);
    
    // Deserialize to new handler
    auto sliceStore2 = std::make_unique<DefaultTimeBasedSliceStore>(1000, 1000);
    auto recovered = SerializableHJOperatorHandler::deserialize(
        serialized, inputOrigins, outputOrigin, std::move(sliceStore2));
    
    ASSERT_NE(recovered, nullptr);
    
    // Verify state matches
    const auto& originalState = hjHandler->getState();
    const auto& recoveredState = recovered->getState();
    
    EXPECT_EQ(originalState.metadata.operatorType, recoveredState.metadata.operatorType);
    EXPECT_EQ(originalState.config.keySize, recoveredState.config.keySize);
    EXPECT_EQ(originalState.config.valueSize, recoveredState.config.valueSize);
    EXPECT_EQ(originalState.config.numberOfBuckets, recoveredState.config.numberOfBuckets);
    EXPECT_EQ(originalState.hashMaps.size(), recoveredState.hashMaps.size());
    EXPECT_EQ(originalState.windows.size(), recoveredState.windows.size());
}

TEST_F(JoinRecoveryTest, HashJoinWithDataSerialization) {
    // Create hash join handler with specific config
    std::vector<OriginId> inputOrigins = {OriginId(1), OriginId(2)};
    OriginId outputOrigin(3);
    auto sliceStore = std::make_unique<DefaultTimeBasedSliceStore>(1000, 1000);
    
    auto hjHandler = std::make_shared<SerializableHJOperatorHandler>(
        inputOrigins, outputOrigin, std::move(sliceStore));
    
    // Set up configuration
    hjHandler->getState().config.keySize = sizeof(int64_t);
    hjHandler->getState().config.valueSize = sizeof(int64_t);
    hjHandler->getState().config.numberOfBuckets = 16;
    hjHandler->getState().config.pageSize = 4096;
    
    // Create/register slice via handler factory and then mutate that slice's state
    CreateNewHashMapSliceArgs args{{}, sizeof(int64_t), sizeof(int64_t), 4096, 16};
    auto created = hjHandler->getSliceAndWindowStore().getSlicesOrCreate(
        Timestamp(100), hjHandler->getCreateNewSlicesFunction(args));
    ASSERT_FALSE(created.empty());
    auto slice = std::dynamic_pointer_cast<SerializableHJSlice>(created.front());
    ASSERT_TRUE(static_cast<bool>(slice));

    // Add data to left hash map (mutate slice stored in slice store)
    auto& leftState = slice->getHashMapState(WorkerThreadId(0), JoinBuildSideType::Left);
    leftState.tupleCount = 10;
    leftState.memory.resize(1024);
    std::memset(leftState.memory.data(), 0xAA, leftState.memory.size());
    
    // Add data to right hash map
    auto& rightState = slice->getHashMapState(WorkerThreadId(0), JoinBuildSideType::Right);
    rightState.tupleCount = 20;
    rightState.memory.resize(2048);
    std::memset(rightState.memory.data(), 0xBB, rightState.memory.size());
    
    // Materialize state directly on handler for serialization
    auto& hjState = hjHandler->getState();
    hjState.hashMaps.clear();
    hjState.windows.clear();
    hjState.config.keySize = sizeof(int64_t);
    hjState.config.valueSize = sizeof(int64_t);
    hjState.config.numberOfBuckets = 16;
    hjState.config.pageSize = 4096;
    hjState.hashMaps.push_back({leftState.memory, leftState.chains, leftState.entrySize, leftState.tupleCount,
                                leftState.keySize, leftState.valueSize, leftState.bucketCount,
                                leftState.varSizedMemory, leftState.varSizedOffset});
    hjState.hashMaps.push_back({rightState.memory, rightState.chains, rightState.entrySize, rightState.tupleCount,
                                rightState.keySize, rightState.valueSize, rightState.bucketCount,
                                rightState.varSizedMemory, rightState.varSizedOffset});
    hjState.windows.push_back({100, 200, 0, 0, 2, 0, 0, 0, 0});
    hjState.metadata.processedRecords = leftState.tupleCount + rightState.tupleCount;
    
    // Verify state was set
    ASSERT_EQ(hjHandler->getState().hashMaps.size(), 2); // Left and right
    EXPECT_EQ(hjHandler->getState().hashMaps[0].tupleCount, 10);
    EXPECT_EQ(hjHandler->getState().hashMaps[1].tupleCount, 20);
    
    // Serialize
    auto serialized = hjHandler->serialize(bufferManager.get());
    
    // Deserialize
    auto sliceStore2 = std::make_unique<DefaultTimeBasedSliceStore>(1000, 1000);
    auto recovered = SerializableHJOperatorHandler::deserialize(
        serialized, inputOrigins, outputOrigin, std::move(sliceStore2));
    
    // Verify recovered state
    ASSERT_EQ(recovered->getState().hashMaps.size(), 2);
    EXPECT_EQ(recovered->getState().hashMaps[0].tupleCount, 10);
    EXPECT_EQ(recovered->getState().hashMaps[0].memory.size(), 1024);
    EXPECT_EQ(recovered->getState().hashMaps[1].tupleCount, 20);
    EXPECT_EQ(recovered->getState().hashMaps[1].memory.size(), 2048);
    
    // Verify memory content
    EXPECT_EQ(recovered->getState().hashMaps[0].memory[0], 0xAA);
    EXPECT_EQ(recovered->getState().hashMaps[1].memory[0], 0xBB);
}

TEST_F(JoinRecoveryTest, NestedLoopJoinEmptySerialization) {
    // Create NLJ handler
    std::vector<OriginId> inputOrigins = {OriginId(1), OriginId(2)};
    OriginId outputOrigin(3);
    auto sliceStore = std::make_unique<DefaultTimeBasedSliceStore>(1000, 1000);
    
    auto nljHandler = std::make_shared<SerializableNLJOperatorHandler>(
        inputOrigins, outputOrigin, std::move(sliceStore));
    
    // Capture empty state
    nljHandler->captureState();
    
    // Serialize
    auto serialized = nljHandler->serialize(bufferManager.get());
    ASSERT_GT(serialized.getBufferSize(), 0);
    
    // Deserialize
    auto sliceStore2 = std::make_unique<DefaultTimeBasedSliceStore>(1000, 1000);
    auto recovered = SerializableNLJOperatorHandler::deserialize(
        serialized, inputOrigins, outputOrigin, std::move(sliceStore2));
    
    ASSERT_NE(recovered, nullptr);
    
    // Verify state
    const auto& originalState = nljHandler->getState();
    const auto& recoveredState = recovered->getState();
    
    EXPECT_EQ(originalState.metadata.operatorType, recoveredState.metadata.operatorType);
    EXPECT_EQ(originalState.leftVectors.size(), recoveredState.leftVectors.size());
    EXPECT_EQ(originalState.rightVectors.size(), recoveredState.rightVectors.size());
    EXPECT_EQ(originalState.windows.size(), recoveredState.windows.size());
}

TEST_F(JoinRecoveryTest, NestedLoopJoinWithVectorData) {
    // Create NLJ handler
    std::vector<OriginId> inputOrigins = {OriginId(1), OriginId(2)};
    OriginId outputOrigin(3);
    auto sliceStore = std::make_unique<DefaultTimeBasedSliceStore>(1000, 1000);
    
    auto nljHandler = std::make_shared<SerializableNLJOperatorHandler>(
        inputOrigins, outputOrigin, std::move(sliceStore));
    
    // Set configuration
    nljHandler->getState().config.pageSize = 4096;
    
    // Create/register NLJ slice via handler factory then mutate
    auto created = nljHandler->getSliceAndWindowStore().getSlicesOrCreate(
        Timestamp(100),
        nljHandler->getCreateNewSlicesFunction(CreateNewHashMapSliceArgs{{}, 0, 0, 0, 0}));
    ASSERT_FALSE(created.empty());
    auto slice = std::dynamic_pointer_cast<NLJSlice>(created.front());
    ASSERT_TRUE(static_cast<bool>(slice));
    
    // Add data to left vector
    auto* leftVec = slice->getPagedVectorRefLeft(WorkerThreadId(0));
    if (leftVec) {
        auto& leftState = leftVec->extractState();
        leftState.pageSize = 4096;
        leftState.entrySize = sizeof(int64_t);
        leftState.totalEntries = 100;
        leftState.pages.emplace_back();
        leftState.pages[0].buffer.resize(4096);
        leftState.pages[0].bufferSize = 4096;
        leftState.pages[0].numberOfEntries = 100;
        std::memset(leftState.pages[0].buffer.data(), 0xCC, leftState.pages[0].buffer.size());
    }
    
    // Add data to right vector
    auto* rightVec = slice->getPagedVectorRefRight(WorkerThreadId(0));
    if (rightVec) {
        auto& rightState = rightVec->extractState();
        rightState.pageSize = 4096;
        rightState.entrySize = sizeof(int64_t);
        rightState.totalEntries = 200;
        rightState.pages.emplace_back();
        rightState.pages[0].buffer.resize(4096);
        rightState.pages[0].bufferSize = 4096;
        rightState.pages[0].numberOfEntries = 200;
        std::memset(rightState.pages[0].buffer.data(), 0xDD, rightState.pages[0].buffer.size());
    }
    
    // Materialize state directly on handler for serialization
    auto& nljState = nljHandler->getState();
    nljState.leftVectors.clear();
    nljState.rightVectors.clear();
    nljState.windows.clear();
    nljState.leftVectors.push_back(slice->getPagedVectorRefLeft(WorkerThreadId(0))->extractState());
    nljState.rightVectors.push_back(slice->getPagedVectorRefRight(WorkerThreadId(0))->extractState());
    nljState.windows.push_back({100, 200, 0, 0, 0, 0, 1, 0, 1});
    nljState.metadata.processedRecords = 100 + 200;
    
    // Verify capture
    ASSERT_GE(nljHandler->getState().leftVectors.size(), 1);
    ASSERT_GE(nljHandler->getState().rightVectors.size(), 1);
    EXPECT_EQ(nljHandler->getState().leftVectors[0].totalEntries, 100);
    EXPECT_EQ(nljHandler->getState().rightVectors[0].totalEntries, 200);
    
    // Serialize
    auto serialized = nljHandler->serialize(bufferManager.get());
    
    // Deserialize
    auto sliceStore2 = std::make_unique<DefaultTimeBasedSliceStore>(1000, 1000);
    auto recovered = SerializableNLJOperatorHandler::deserialize(
        serialized, inputOrigins, outputOrigin, std::move(sliceStore2));
    
    // Verify recovered state
    ASSERT_GE(recovered->getState().leftVectors.size(), 1);
    ASSERT_GE(recovered->getState().rightVectors.size(), 1);
    EXPECT_EQ(recovered->getState().leftVectors[0].totalEntries, 100);
    EXPECT_EQ(recovered->getState().rightVectors[0].totalEntries, 200);
    EXPECT_EQ(recovered->getState().leftVectors[0].pages[0].buffer[0], 0xCC);
    EXPECT_EQ(recovered->getState().rightVectors[0].pages[0].buffer[0], 0xDD);
}

TEST_F(JoinRecoveryTest, HashJoinWatermarkPreservation) {
    // Create hash join handler
    std::vector<OriginId> inputOrigins = {OriginId(1), OriginId(2)};
    OriginId outputOrigin(3);
    auto sliceStore = std::make_unique<DefaultTimeBasedSliceStore>(1000, 1000);
    
    auto hjHandler = std::make_shared<SerializableHJOperatorHandler>(
        inputOrigins, outputOrigin, std::move(sliceStore));
    
    // Set watermark in state
    hjHandler->getState().metadata.lastWatermark = 5000;
    
    // Capture and serialize
    hjHandler->captureState();
    auto serialized = hjHandler->serialize(bufferManager.get());
    
    // Deserialize
    auto sliceStore2 = std::make_unique<DefaultTimeBasedSliceStore>(1000, 1000);
    auto recovered = SerializableHJOperatorHandler::deserialize(
        serialized, inputOrigins, outputOrigin, std::move(sliceStore2));
    
    // Verify watermark preserved
    EXPECT_EQ(recovered->getState().metadata.lastWatermark, 5000);
}

TEST_F(JoinRecoveryTest, MultipleWindowsRecovery) {
    // Create hash join handler
    std::vector<OriginId> inputOrigins = {OriginId(1), OriginId(2)};
    OriginId outputOrigin(3);
    auto sliceStore = std::make_unique<DefaultTimeBasedSliceStore>(1000, 1000);
    
    auto hjHandler = std::make_shared<SerializableHJOperatorHandler>(
        inputOrigins, outputOrigin, std::move(sliceStore));
    
    // Add multiple windows to state
    hjHandler->getState().windows.push_back({100, 200, 0, 0, 2, 0, 0, 0, 0});
    hjHandler->getState().windows.push_back({200, 300, 0, 2, 2, 0, 0, 0, 0});
    hjHandler->getState().windows.push_back({300, 400, 0, 4, 2, 0, 0, 0, 0});
    
    // Add hash maps for each window
    for (int i = 0; i < 6; ++i) {
        JoinState::HashMap hm{};
        hm.keySize = sizeof(int64_t);
        hm.valueSize = sizeof(int64_t);
        hm.bucketCount = 16;
        hm.tupleCount = 10 * (i + 1);
        hjHandler->getState().hashMaps.push_back(hm);
    }
    
    // Serialize
    auto serialized = hjHandler->serialize(bufferManager.get());
    
    // Deserialize
    auto sliceStore2 = std::make_unique<DefaultTimeBasedSliceStore>(1000, 1000);
    auto recovered = SerializableHJOperatorHandler::deserialize(
        serialized, inputOrigins, outputOrigin, std::move(sliceStore2));
    
    // Verify windows
    ASSERT_EQ(recovered->getState().windows.size(), 3);
    EXPECT_EQ(recovered->getState().windows[0].start, 100);
    EXPECT_EQ(recovered->getState().windows[0].end, 200);
    EXPECT_EQ(recovered->getState().windows[1].start, 200);
    EXPECT_EQ(recovered->getState().windows[1].end, 300);
    EXPECT_EQ(recovered->getState().windows[2].start, 300);
    EXPECT_EQ(recovered->getState().windows[2].end, 400);
    
    // Verify hash maps
    ASSERT_EQ(recovered->getState().hashMaps.size(), 6);
    for (int i = 0; i < 6; ++i) {
        EXPECT_EQ(recovered->getState().hashMaps[i].tupleCount, 10 * (i + 1));
    }
}

TEST_F(JoinRecoveryTest, RecoveryManagerIntegrationWithJoins) {
    // Create a pipeline state with join operators
    PipelineState ps{};
    ps.version = 1;
    ps.queryId = 123;
    ps.pipelineId = 1;
    ps.createdTimestampNs = 1000000;
    
    // Add hash join operator blob
    OperatorStateBlob hjBlob{};
    hjBlob.header.operatorId = 1;
    hjBlob.header.kind = OperatorStateTag::Kind::HashJoin;
    hjBlob.header.version = 1;
    
    // Create and serialize hash join state
    JoinState hjState{};
    hjState.metadata.operatorType = 2;
    hjState.metadata.lastWatermark = 3000;
    hjState.config.keySize = 8;
    hjState.config.valueSize = 8;
    hjState.config.numberOfBuckets = 16;
    
    // Add a window with hash maps
    hjState.windows.push_back({100, 200, 0, 0, 2, 0, 0, 0, 0});
    hjState.hashMaps.push_back({std::vector<uint8_t>(64), {0,0,0,0}, 24, 5, 8, 8, 16, {}, 0});
    hjState.hashMaps.push_back({std::vector<uint8_t>(64), {0,0,0,0}, 24, 7, 8, 8, 16, {}, 0});
    
    auto hjBuffer = Serialization::StateSerializer<JoinState>::serialize(hjState, bufferManager.get());
    hjBlob.bytes.resize(hjBuffer.getBufferSize());
    std::memcpy(hjBlob.bytes.data(), hjBuffer.getBuffer(), hjBuffer.getBufferSize());
    ps.operators.push_back(hjBlob);
    
    // Add nested loop join operator blob
    OperatorStateBlob nljBlob{};
    nljBlob.header.operatorId = 2;
    nljBlob.header.kind = OperatorStateTag::Kind::NestedLoopJoin;
    nljBlob.header.version = 1;
    
    // Create and serialize NLJ state
    JoinState nljState{};
    nljState.metadata.operatorType = 2;
    nljState.metadata.lastWatermark = 4000;
    nljState.config.pageSize = 4096;
    
    // Add a window with vectors
    JoinState::Window window{};
    window.start = 200;
    window.end = 300;
    window.state = 0;
    window.firstLeftVectorIndex = 0;
    window.leftVectorCount = 1;
    window.firstRightVectorIndex = 0;
    window.rightVectorCount = 1;
    nljState.windows.push_back(window);
    
    DataStructures::PagedVectorState leftVec{};
    leftVec.pageSize = 4096;
    leftVec.entrySize = 8;
    leftVec.totalEntries = 50;
    DataStructures::PagedVectorState::PageData leftPage{};
    leftPage.buffer.resize(4096);
    leftPage.bufferSize = 4096;
    leftPage.numberOfEntries = 50;
    leftVec.pages.push_back(leftPage);
    nljState.leftVectors.push_back(leftVec);
    
    DataStructures::PagedVectorState rightVec{};
    rightVec.pageSize = 4096;
    rightVec.entrySize = 8;
    rightVec.totalEntries = 75;
    DataStructures::PagedVectorState::PageData rightPage{};
    rightPage.buffer.resize(4096);
    rightPage.bufferSize = 4096;
    rightPage.numberOfEntries = 75;
    rightVec.pages.push_back(rightPage);
    nljState.rightVectors.push_back(rightVec);
    
    auto nljBuffer = Serialization::StateSerializer<JoinState>::serialize(nljState, bufferManager.get());
    nljBlob.bytes.resize(nljBuffer.getBufferSize());
    std::memcpy(nljBlob.bytes.data(), nljBuffer.getBuffer(), nljBuffer.getBufferSize());
    ps.operators.push_back(nljBlob);
    
    // Write checkpoint
    auto checkpointPath = testDir / "join_test.checkpoint";
    CheckpointCoordinator::writeCheckpoint(ps, checkpointPath);
    
    // Set up recovery args
    RecoveryManager::RecreateArgs args{};
    args.bufferProvider = bufferManager.get();
    
    // Add construction args for both operators
    args.operatorArgsById[1] = {
        .inputOrigins = {OriginId(1), OriginId(2)},
        .outputOriginId = OriginId(3),
        .makeSliceStore = []() { return std::make_unique<DefaultTimeBasedSliceStore>(1000, 1000); }
    };
    
    args.operatorArgsById[2] = {
        .inputOrigins = {OriginId(4), OriginId(5)},
        .outputOriginId = OriginId(6),
        .makeSliceStore = []() { return std::make_unique<DefaultTimeBasedSliceStore>(1000, 1000); }
    };
    
    // Recover pipeline
    auto result = RecoveryManager::recoverPipeline(checkpointPath, args);
    
    // Verify recovery
    ASSERT_EQ(result.handlers.size(), 2);
    EXPECT_EQ(result.pipelineState.operators.size(), 2);
    
    // Verify hash join handler
    auto* hjHandler = dynamic_cast<SerializableHJOperatorHandler*>(result.handlers[0].get());
    ASSERT_NE(hjHandler, nullptr);
    EXPECT_EQ(hjHandler->getState().metadata.lastWatermark, 3000);
    EXPECT_EQ(hjHandler->getState().windows.size(), 1);
    EXPECT_EQ(hjHandler->getState().hashMaps.size(), 2);
    
    // Verify NLJ handler
    auto* nljHandler = dynamic_cast<SerializableNLJOperatorHandler*>(result.handlers[1].get());
    ASSERT_NE(nljHandler, nullptr);
    EXPECT_EQ(nljHandler->getState().metadata.lastWatermark, 4000);
    EXPECT_EQ(nljHandler->getState().windows.size(), 1);
    EXPECT_EQ(nljHandler->getState().leftVectors.size(), 1);
    EXPECT_EQ(nljHandler->getState().rightVectors.size(), 1);
}
