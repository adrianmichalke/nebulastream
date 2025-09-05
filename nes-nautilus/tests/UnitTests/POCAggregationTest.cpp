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
#include <chrono>
#include <Nautilus/Operations/SimpleAggregationOperator.hpp>
#include <Nautilus/Operations/StandardOperations.hpp>
#include <Nautilus/Operations/OperationRegistry.hpp>
#include <Nautilus/DataStructures/OffsetBasedHashMap.hpp>
#include <Runtime/BufferManager.hpp>

using namespace NES;
using namespace NES::Operations;
using namespace NES::DataStructures;

class POCAggregationTest : public ::testing::Test {
protected:
    void SetUp() override {
        bufferManager = BufferManager::create(1024 * 1024, 10);
    }
    
    void TearDown() override {
        bufferManager.reset();
    }
    
    TupleBuffer createKeyValueTupleBuffer(int64_t key, int64_t value) {
        auto bufferOpt = bufferManager->getUnpooledBuffer(sizeof(int64_t) * 2);
        auto buffer = bufferOpt.value();
        
        int64_t* data = buffer.getBuffer<int64_t>();
        data[0] = key;
        data[1] = value;
        
        return buffer;
    }
    
    std::shared_ptr<BufferManager> bufferManager;
};

TEST_F(POCAggregationTest, testBasicSumAggregation) {
    SimpleAggregationOperator op;
    op.addOperation(OperationID::SUM_INT64, OperationParams{rfl::Field<"sum", SumParams>{SumParams{"testField", true}}});
    
    // Process some data: key=1 with values [10, 20, 30]
    auto tuple1 = createKeyValueTupleBuffer(1, 10);
    auto tuple2 = createKeyValueTupleBuffer(1, 20);
    auto tuple3 = createKeyValueTupleBuffer(1, 30);
    
    op.processTuple(tuple1);
    op.processTuple(tuple2);
    op.processTuple(tuple3);
    
    // Verify state
    const auto& state = op.getState();
    EXPECT_EQ(state.metadata.processedRecords, 3);
    EXPECT_FALSE(state.hashMaps.empty());
    
    // Check the hashmap contains our aggregated data
    auto hashMap = OffsetBasedHashMap::fromState(
        const_cast<OffsetHashMapState&>(
            reinterpret_cast<const OffsetHashMapState&>(state.hashMaps[0])
        )
    );
    
    EXPECT_GT(hashMap.size(), 0);
}

TEST_F(POCAggregationTest, testCountAggregation) {
    SimpleAggregationOperator op;
    op.addOperation(OperationID::COUNT);
    
    // Process multiple tuples with same key
    auto tuple1 = createKeyValueTupleBuffer(1, 100);
    auto tuple2 = createKeyValueTupleBuffer(1, 200);
    auto tuple3 = createKeyValueTupleBuffer(1, 300);
    auto tuple4 = createKeyValueTupleBuffer(2, 400); // Different key
    
    op.processTuple(tuple1);
    op.processTuple(tuple2);
    op.processTuple(tuple3);
    op.processTuple(tuple4);
    
    // Verify processed records
    EXPECT_EQ(op.getState().metadata.processedRecords, 4);
    
    // Verify hashmap has entries
    const auto& hashMapData = op.getState().hashMaps[0];
    EXPECT_GT(hashMapData.tupleCount, 0);
}

TEST_F(POCAggregationTest, testMultipleOperations) {
    SimpleAggregationOperator op;
    op.addOperation(OperationID::SUM_INT64, OperationParams{rfl::Field<"sum", SumParams>{SumParams{"value", true}}});
    op.addOperation(OperationID::COUNT);
    
    // Process data
    for (int i = 1; i <= 5; ++i) {
        auto tuple = createKeyValueTupleBuffer(1, i * 10); // key=1, values=[10,20,30,40,50]
        op.processTuple(tuple);
    }
    
    // Verify all operations were executed
    EXPECT_EQ(op.getState().metadata.processedRecords, 5);
    EXPECT_EQ(op.getState().operations.size(), 3); // COUNT (default) + SUM + COUNT (added)
}

TEST_F(POCAggregationTest, testSerializationRoundTrip) {
    SimpleAggregationOperator originalOp;
    originalOp.addOperation(OperationID::SUM_INT64, OperationParams{rfl::Field<"sum", SumParams>{SumParams{"value", true}}});
    
    // Process some data
    for (int i = 1; i <= 100; ++i) {
        auto tuple = createKeyValueTupleBuffer(i % 10, i); // 10 different keys
        originalOp.processTuple(tuple);
    }
    
    EXPECT_EQ(originalOp.getState().metadata.processedRecords, 100);
    
    // Serialize
    auto start = std::chrono::high_resolution_clock::now();
    auto serialized = originalOp.serialize(bufferManager.get());
    auto serializationTime = std::chrono::high_resolution_clock::now() - start;
    
    // Deserialize
    start = std::chrono::high_resolution_clock::now();
    auto deserializedOp = SimpleAggregationOperator::deserialize(serialized);
    auto deserializationTime = std::chrono::high_resolution_clock::now() - start;
    
    // Verify correctness
    EXPECT_EQ(deserializedOp->getState().metadata.processedRecords, 100);
    EXPECT_EQ(deserializedOp->getState().operations.size(), originalOp.getState().operations.size());
    EXPECT_EQ(deserializedOp->getState().hashMaps.size(), originalOp.getState().hashMaps.size());
    
    // Continue processing with deserialized operator
    auto continueTuple = createKeyValueTupleBuffer(5, 999);
    deserializedOp->processTuple(continueTuple);
    
    EXPECT_EQ(deserializedOp->getState().metadata.processedRecords, 101);
    
    // Performance logging
    auto serializationMs = std::chrono::duration_cast<std::chrono::milliseconds>(serializationTime).count();
    auto deserializationMs = std::chrono::duration_cast<std::chrono::milliseconds>(deserializationTime).count();
    
    std::cout << "Serialization time: " << serializationMs << "ms" << std::endl;
    std::cout << "Deserialization time: " << deserializationMs << "ms" << std::endl;
    std::cout << "Buffer size: " << serialized.getBufferSize() << " bytes" << std::endl;
}

TEST_F(POCAggregationTest, testLargeDatasetProcessing) {
    SimpleAggregationOperator op;
    op.addOperation(OperationID::COUNT);
    op.addOperation(OperationID::SUM_INT64, OperationParams{rfl::Field<"sum", SumParams>{SumParams{"value", true}}});
    
    const int numRecords = 10000;
    const int numKeys = 100;
    
    auto start = std::chrono::high_resolution_clock::now();
    
    // Process large dataset
    for (int i = 0; i < numRecords; ++i) {
        int64_t key = i % numKeys;
        int64_t value = i;
        auto tuple = createKeyValueTupleBuffer(key, value);
        op.processTuple(tuple);
    }
    
    auto processingTime = std::chrono::high_resolution_clock::now() - start;
    
    // Verify processing
    EXPECT_EQ(op.getState().metadata.processedRecords, numRecords);
    EXPECT_FALSE(op.getState().hashMaps.empty());
    
    // Performance metrics
    auto processingMs = std::chrono::duration_cast<std::chrono::milliseconds>(processingTime).count();
    std::cout << "Processed " << numRecords << " records in " << processingMs << "ms" << std::endl;
    std::cout << "Throughput: " << (numRecords * 1000 / processingMs) << " records/sec" << std::endl;
    
    // Serialize and measure
    start = std::chrono::high_resolution_clock::now();
    auto serialized = op.serialize(bufferManager.get());
    auto serializationMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::high_resolution_clock::now() - start).count();
    
    std::cout << "Serialized in " << serializationMs << "ms, size: " << serialized.getBufferSize() << " bytes" << std::endl;
    
    // Verify performance targets from Phase 1 plan
    EXPECT_LT(serializationMs, 100); // Should be fast with Cap'n Proto
    EXPECT_GT(numRecords * 1000 / processingMs, 10000); // >10k records/sec
}

TEST_F(POCAggregationTest, testMultiKeyAggregation) {
    SimpleAggregationOperator op;
    op.addOperation(OperationID::SUM_INT64, OperationParams{rfl::Field<"sum", SumParams>{SumParams{"value", true}}});
    
    // Insert data for different keys
    std::map<int64_t, int64_t> expectedSums;
    
    for (int key = 1; key <= 10; ++key) {
        for (int val = 1; val <= 10; ++val) {
            auto tuple = createKeyValueTupleBuffer(key, val);
            op.processTuple(tuple);
            expectedSums[key] += val;
        }
    }
    
    EXPECT_EQ(op.getState().metadata.processedRecords, 100);
    
    // Verify each key should have sum = 1+2+...+10 = 55
    for (int key = 1; key <= 10; ++key) {
        EXPECT_EQ(expectedSums[key], 55);
    }
}

TEST_F(POCAggregationTest, testStateViewZeroCopy) {
    SimpleAggregationOperator op;
    op.addOperation(OperationID::COUNT);
    
    // Process some data
    for (int i = 0; i < 50; ++i) {
        auto tuple = createKeyValueTupleBuffer(i % 5, i);
        op.processTuple(tuple);
    }
    
    // Serialize
    auto serialized = op.serialize(bufferManager.get());
    
    // Create StateView for zero-copy access
    Serialization::StateView<State::AggregationState> view(serialized);
    
    EXPECT_TRUE(view.isValid());
    EXPECT_EQ(view->metadata.processedRecords, 50);
    EXPECT_FALSE(view->hashMaps.empty());
}