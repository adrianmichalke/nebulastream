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
#include <filesystem>
#include <fstream>
#include <chrono>
#include <thread>
#include <future>

#include <Nautilus/Recovery/CheckpointManager.hpp>
#include <Nautilus/State/Reflection/PipelineState.hpp>
#include <Nautilus/State/Reflection/StateDefinitions.hpp>

using namespace NES;
using namespace NES::Recovery;
using namespace NES::State;

class CheckpointManagerTest : public ::testing::Test {
protected:
    void SetUp() override {
        testDir = std::filesystem::temp_directory_path() / ("checkpoint_test_" + std::to_string(getpid()));
        std::filesystem::create_directories(testDir);
    }
    
    void TearDown() override {
        std::filesystem::remove_all(testDir);
    }
    
    std::filesystem::path testDir;
    
    PipelineState createTestPipelineState() {
        PipelineState state;
        state.version = 42;
        state.queryId = 12345;
        state.pipelineId = 67890;
        state.createdTimestampNs = std::chrono::system_clock::now().time_since_epoch().count();
        
        // Add some operator blobs
        OperatorStateBlob blob1;
        blob1.header.kind = OperatorStateTag::Kind::Aggregation;
        blob1.header.operatorId = 1;
        blob1.header.version = 1;
        blob1.bytes = {0x01, 0x02, 0x03, 0x04, 0x05};
        state.operators.push_back(blob1);
        
        OperatorStateBlob blob2;
        blob2.header.kind = OperatorStateTag::Kind::HashJoin;
        blob2.header.operatorId = 2;
        blob2.header.version = 2;
        blob2.bytes = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};
        state.operators.push_back(blob2);
        
        // Add progress metadata
        state.progress.version = 1;
        state.progress.lastWatermark = 9999;
        
        ProgressMetadata::OriginProgress origin1;
        origin1.originId = 100;
        origin1.processedRecords = 5000;
        origin1.lastWatermark = 8888;
        state.progress.origins.push_back(origin1);
        
        ProgressMetadata::OriginProgress origin2;
        origin2.originId = 200;
        origin2.processedRecords = 7500;
        origin2.lastWatermark = 9999;
        state.progress.origins.push_back(origin2);
        
        return state;
    }
    
    bool areStatesEqual(const PipelineState& a, const PipelineState& b) {
        if (a.version != b.version || 
            a.queryId != b.queryId ||
            a.pipelineId != b.pipelineId ||
            a.createdTimestampNs != b.createdTimestampNs) {
            return false;
        }
        
        if (a.operators.size() != b.operators.size()) {
            return false;
        }
        
        for (size_t i = 0; i < a.operators.size(); ++i) {
            const auto& opA = a.operators[i];
            const auto& opB = b.operators[i];
            if (opA.header.kind != opB.header.kind ||
                opA.header.operatorId != opB.header.operatorId ||
                opA.header.version != opB.header.version ||
                opA.bytes != opB.bytes) {
                return false;
            }
        }
        
        if (a.progress.version != b.progress.version ||
            a.progress.lastWatermark != b.progress.lastWatermark ||
            a.progress.origins.size() != b.progress.origins.size()) {
            return false;
        }
        
        for (size_t i = 0; i < a.progress.origins.size(); ++i) {
            const auto& oA = a.progress.origins[i];
            const auto& oB = b.progress.origins[i];
            if (oA.originId != oB.originId ||
                oA.processedRecords != oB.processedRecords ||
                oA.lastWatermark != oB.lastWatermark) {
                return false;
            }
        }
        
        return true;
    }
};

TEST_F(CheckpointManagerTest, BasicRoundTripSerialization) {
    CheckpointManager manager;
    auto originalState = createTestPipelineState();
    std::string checkpointPath = (testDir / "test.checkpoint").string();
    
    // Write checkpoint
    manager.checkpoint(originalState, checkpointPath);
    
    // Verify file exists
    ASSERT_TRUE(std::filesystem::exists(checkpointPath));
    
    // Read it back
    auto recoveredState = manager.recover(checkpointPath);
    
    // Verify all fields match
    EXPECT_TRUE(areStatesEqual(originalState, recoveredState));
}

TEST_F(CheckpointManagerTest, AtomicWriteVerification) {
    CheckpointManager manager;
    auto state = createTestPipelineState();
    std::string checkpointPath = (testDir / "atomic.checkpoint").string();
    std::string tmpPath = checkpointPath + ".inprogress";
    
    // Start async checkpoint
    auto future = manager.checkpointAsync(state, checkpointPath);
    
    // Brief sleep to let async operation start
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    
    // Check that temporary file might exist during write (race condition, so we don't assert)
    bool tmpExisted = std::filesystem::exists(tmpPath);
    
    // Wait for completion
    future.get();
    
    // Verify final file exists and temp file is gone
    ASSERT_TRUE(std::filesystem::exists(checkpointPath));
    ASSERT_FALSE(std::filesystem::exists(tmpPath));
    
    // Verify content is correct
    auto recoveredState = manager.recover(checkpointPath);
    EXPECT_TRUE(areStatesEqual(state, recoveredState));
}

TEST_F(CheckpointManagerTest, AsyncCheckpointCompletion) {
    CheckpointManager manager;
    auto state = createTestPipelineState();
    std::string checkpointPath = (testDir / "async.checkpoint").string();
    
    // Launch async checkpoint
    auto future = manager.checkpointAsync(state, checkpointPath);
    
    // File might not exist immediately
    // Wait for completion
    future.get();
    
    // Now file must exist
    ASSERT_TRUE(std::filesystem::exists(checkpointPath));
    
    // Verify content
    auto recoveredState = manager.recover(checkpointPath);
    EXPECT_TRUE(areStatesEqual(state, recoveredState));
}

TEST_F(CheckpointManagerTest, EmptyPipelineState) {
    CheckpointManager manager;
    PipelineState emptyState;
    emptyState.version = 1;
    emptyState.queryId = 0;
    emptyState.pipelineId = 0;
    emptyState.createdTimestampNs = 0;
    emptyState.progress.version = 0;
    emptyState.progress.lastWatermark = 0;
    
    std::string checkpointPath = (testDir / "empty.checkpoint").string();
    
    // Write and recover
    manager.checkpoint(emptyState, checkpointPath);
    auto recoveredState = manager.recover(checkpointPath);
    
    // Verify
    EXPECT_TRUE(areStatesEqual(emptyState, recoveredState));
    EXPECT_EQ(recoveredState.operators.size(), 0);
    EXPECT_EQ(recoveredState.progress.origins.size(), 0);
}

TEST_F(CheckpointManagerTest, LargePipelineState) {
    CheckpointManager manager;
    PipelineState largeState;
    largeState.version = 1;
    largeState.queryId = 999;
    largeState.pipelineId = 888;
    largeState.createdTimestampNs = 777;
    
    // Add many operators with large blobs
    for (int i = 0; i < 100; ++i) {
        OperatorStateBlob blob;
        blob.header.kind = static_cast<OperatorStateTag::Kind>(i % 4);
        blob.header.operatorId = i;
        blob.header.version = 1;
        // Create a large blob (10KB each)
        blob.bytes.resize(10240);
        for (size_t j = 0; j < blob.bytes.size(); ++j) {
            blob.bytes[j] = static_cast<uint8_t>((i + j) % 256);
        }
        largeState.operators.push_back(blob);
    }
    
    // Add many origins
    for (int i = 0; i < 50; ++i) {
        ProgressMetadata::OriginProgress origin;
        origin.originId = 1000 + i;
        origin.processedRecords = i * 1000;
        origin.lastWatermark = i * 100;
        largeState.progress.origins.push_back(origin);
    }
    
    largeState.progress.version = 2;
    largeState.progress.lastWatermark = 123456;
    
    std::string checkpointPath = (testDir / "large.checkpoint").string();
    
    // Write and recover
    manager.checkpoint(largeState, checkpointPath);
    auto recoveredState = manager.recover(checkpointPath);
    
    // Verify
    EXPECT_TRUE(areStatesEqual(largeState, recoveredState));
    EXPECT_EQ(recoveredState.operators.size(), 100);
    EXPECT_EQ(recoveredState.progress.origins.size(), 50);
}

TEST_F(CheckpointManagerTest, CorruptedFileHandling) {
    CheckpointManager manager;
    std::string checkpointPath = (testDir / "corrupted.checkpoint").string();
    
    // Write a valid checkpoint first
    auto state = createTestPipelineState();
    manager.checkpoint(state, checkpointPath);
    
    // Corrupt the file by appending garbage
    {
        std::ofstream file(checkpointPath, std::ios::binary | std::ios::app);
        file << "GARBAGE_DATA";
    }
    
    // Recovery should throw due to trailing bytes check
    EXPECT_THROW({
        manager.recover(checkpointPath);
    }, std::runtime_error);
}

TEST_F(CheckpointManagerTest, InvalidMagicHeader) {
    CheckpointManager manager;
    std::string checkpointPath = (testDir / "invalid_magic.checkpoint").string();
    
    // Write a file with invalid magic
    {
        std::ofstream file(checkpointPath, std::ios::binary);
        file << "BADD";  // Wrong magic (should be "NESP")
        // Add some padding to avoid "too small" error
        for (int i = 0; i < 100; ++i) {
            file.put(0);
        }
    }
    
    // Recovery should throw
    EXPECT_THROW({
        manager.recover(checkpointPath);
    }, std::runtime_error);
}

TEST_F(CheckpointManagerTest, TruncatedFile) {
    CheckpointManager manager;
    auto state = createTestPipelineState();
    std::string checkpointPath = (testDir / "truncated.checkpoint").string();
    
    // Write a valid checkpoint
    manager.checkpoint(state, checkpointPath);
    
    // Truncate the file
    {
        auto fileSize = std::filesystem::file_size(checkpointPath);
        std::filesystem::resize_file(checkpointPath, fileSize / 2);
    }
    
    // Recovery should throw
    EXPECT_THROW({
        manager.recover(checkpointPath);
    }, std::runtime_error);
}

TEST_F(CheckpointManagerTest, NonExistentFile) {
    CheckpointManager manager;
    std::string checkpointPath = (testDir / "nonexistent.checkpoint").string();
    
    // Recovery should throw for non-existent file
    EXPECT_THROW({
        manager.recover(checkpointPath);
    }, std::exception);
}

TEST_F(CheckpointManagerTest, EmptyFile) {
    CheckpointManager manager;
    std::string checkpointPath = (testDir / "empty_file.checkpoint").string();
    
    // Create an empty file
    {
        std::ofstream file(checkpointPath);
    }
    
    // Recovery should throw (file too small)
    EXPECT_THROW({
        manager.recover(checkpointPath);
    }, std::runtime_error);
}

TEST_F(CheckpointManagerTest, ConcurrentAsyncCheckpoints) {
    CheckpointManager manager;
    
    // Create multiple different states
    std::vector<PipelineState> states;
    std::vector<std::string> paths;
    std::vector<std::future<void>> futures;
    
    for (int i = 0; i < 10; ++i) {
        auto state = createTestPipelineState();
        state.queryId = 1000 + i;  // Make each state unique
        states.push_back(state);
        
        std::string path = (testDir / ("concurrent_" + std::to_string(i) + ".checkpoint")).string();
        paths.push_back(path);
        
        // Launch async checkpoint
        futures.push_back(manager.checkpointAsync(state, path));
    }
    
    // Wait for all to complete
    for (auto& future : futures) {
        future.get();
    }
    
    // Verify all files exist and contain correct data
    for (size_t i = 0; i < states.size(); ++i) {
        ASSERT_TRUE(std::filesystem::exists(paths[i]));
        auto recovered = manager.recover(paths[i]);
        EXPECT_TRUE(areStatesEqual(states[i], recovered));
    }
}

TEST_F(CheckpointManagerTest, OverwriteExistingCheckpoint) {
    CheckpointManager manager;
    std::string checkpointPath = (testDir / "overwrite.checkpoint").string();
    
    // Write first checkpoint
    auto state1 = createTestPipelineState();
    state1.queryId = 111;
    manager.checkpoint(state1, checkpointPath);
    
    // Verify first checkpoint
    auto recovered1 = manager.recover(checkpointPath);
    EXPECT_EQ(recovered1.queryId, 111);
    
    // Overwrite with different state
    auto state2 = createTestPipelineState();
    state2.queryId = 222;
    manager.checkpoint(state2, checkpointPath);
    
    // Verify second checkpoint replaced the first
    auto recovered2 = manager.recover(checkpointPath);
    EXPECT_EQ(recovered2.queryId, 222);
}

TEST_F(CheckpointManagerTest, SpecialCharactersInPath) {
    CheckpointManager manager;
    auto state = createTestPipelineState();
    
    // Create a subdirectory with special characters
    auto specialDir = testDir / "test-dir_with.special+chars=123";
    std::filesystem::create_directories(specialDir);
    std::string checkpointPath = (specialDir / "checkpoint@file#1.dat").string();
    
    // Write and recover
    manager.checkpoint(state, checkpointPath);
    auto recovered = manager.recover(checkpointPath);
    
    // Verify
    EXPECT_TRUE(areStatesEqual(state, recovered));
}

TEST_F(CheckpointManagerTest, MaxUint64Values) {
    CheckpointManager manager;
    PipelineState state;
    state.version = UINT32_MAX;
    state.queryId = UINT64_MAX;
    state.pipelineId = UINT64_MAX;
    state.createdTimestampNs = UINT64_MAX;
    
    OperatorStateBlob blob;
    blob.header.kind = OperatorStateTag::Kind::NestedLoopJoin;
    blob.header.operatorId = UINT64_MAX;
    blob.header.version = UINT32_MAX;
    blob.bytes = {0xFF, 0xFF, 0xFF, 0xFF};
    state.operators.push_back(blob);
    
    state.progress.version = UINT32_MAX;
    state.progress.lastWatermark = UINT64_MAX;
    
    ProgressMetadata::OriginProgress origin;
    origin.originId = UINT64_MAX;
    origin.processedRecords = UINT64_MAX;
    origin.lastWatermark = UINT64_MAX;
    state.progress.origins.push_back(origin);
    
    std::string checkpointPath = (testDir / "maxvals.checkpoint").string();
    
    // Write and recover
    manager.checkpoint(state, checkpointPath);
    auto recovered = manager.recover(checkpointPath);
    
    // Verify
    EXPECT_TRUE(areStatesEqual(state, recovered));
    EXPECT_EQ(recovered.queryId, UINT64_MAX);
    EXPECT_EQ(recovered.progress.lastWatermark, UINT64_MAX);
}

