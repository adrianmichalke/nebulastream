/*
    Licensed under the Apache License, Version 2.0
*/

#include <gtest/gtest.h>

#include <Aggregation/SerializableAggregationOperatorHandler.hpp>
#include <Aggregation/SerializableAggregationSlice.hpp>
#include <Nautilus/Recovery/RecoveryManager.hpp>
#include <Runtime/BufferManager.hpp>
#include <SliceStore/DefaultTimeBasedSliceStore.hpp>

using namespace NES;

class AggregationStateContentsTest : public ::testing::Test {
protected:
    void SetUp() override { bm = BufferManager::create(64 * 1024, 64); }
    std::shared_ptr<BufferManager> bm;
};

TEST_F(AggregationStateContentsTest, CapturePopulatesStateAndRehydrateRestores)
{
    std::vector<OriginId> inputs{OriginId(1)};
    OriginId out{2};
    auto handler = std::make_shared<SerializableAggregationOperatorHandler>(
        inputs, out, std::make_unique<DefaultTimeBasedSliceStore>(1000, 1000));
    handler->setWorkerThreads(2);

    // Create a slice for window [1000,2000) and insert entries across two workers
    const CreateNewHashMapSliceArgs args{{}, /*key*/8, /*val*/8, /*page*/4096, /*buckets*/64};
    auto makeFn = handler->getCreateNewSlicesFunction(args);
    auto slices = handler->getSliceAndWindowStore().getSlicesOrCreate(Timestamp(1500), makeFn);
    ASSERT_EQ(slices.size(), 1u);
    auto serSlice = std::dynamic_pointer_cast<SerializableAggregationSlice>(slices[0]);
    ASSERT_TRUE(serSlice != nullptr);

    uint64_t k = 111, v = 222;
    auto* w0 = serSlice->getOffsetHashMapWrapper(WorkerThreadId(0));
    auto* w1 = serSlice->getOffsetHashMapWrapper(WorkerThreadId(1));
    ASSERT_NE(w0, nullptr);
    ASSERT_NE(w1, nullptr);
    (void)w0->insertOffsetEntry(/*hash*/11, &k, &v);
    (void)w1->insertOffsetEntry(/*hash*/12, &k, &v);
    EXPECT_GE(w0->getNumberOfTuples() + w1->getNumberOfTuples(), 1u);

    // Capture and verify state contents
    handler->captureState();
    const auto& st = handler->getState();
    ASSERT_EQ(st.windows.size(), 1u);
    EXPECT_EQ(st.hashMaps.size(), 2u);
    uint64_t sumTuples = 0; for (const auto& hm : st.hashMaps) sumTuples += hm.tupleCount; EXPECT_GE(sumTuples, 1u);

    // Serialize/deserialize and rehydrate
    auto tb = handler->serialize(bm.get());
    auto rec = SerializableAggregationOperatorHandler::deserialize(tb, inputs, out, std::make_unique<DefaultTimeBasedSliceStore>(1000, 1000));
    // Access recovered slice and verify tuples
    auto recSlices = rec->getSliceAndWindowStore().getSlicesOrCreate(Timestamp(st.windows[0].start), rec->getCreateNewSlicesFunction(args));
    ASSERT_EQ(recSlices.size(), 1u);
    auto recSlice = std::dynamic_pointer_cast<SerializableAggregationSlice>(recSlices[0]);
    ASSERT_TRUE(recSlice != nullptr);
    auto* rw0 = recSlice->getOffsetHashMapWrapper(WorkerThreadId(0));
    auto* rw1 = recSlice->getOffsetHashMapWrapper(WorkerThreadId(1));
    ASSERT_NE(rw0, nullptr);
    ASSERT_NE(rw1, nullptr);
    EXPECT_GE(rw0->getNumberOfTuples() + rw1->getNumberOfTuples(), 1u);
}

TEST_F(AggregationStateContentsTest, PartialWindowRecovery)
{
    std::vector<OriginId> inputs{OriginId(3)};
    OriginId out{4};
    auto handler = std::make_shared<SerializableAggregationOperatorHandler>(
        inputs, out, std::make_unique<DefaultTimeBasedSliceStore>(1000, 1000));
    handler->setWorkerThreads(1);

    const CreateNewHashMapSliceArgs args{{}, 8, 8, 4096, 64};
    auto makeFn = handler->getCreateNewSlicesFunction(args);

    // Window A [0,1000): insert one entry
    auto sA = handler->getSliceAndWindowStore().getSlicesOrCreate(Timestamp(500), makeFn);
    auto sliceA = std::dynamic_pointer_cast<SerializableAggregationSlice>(sA[0]);
    uint64_t k=1,v=2; sliceA->getOffsetHashMapWrapper(WorkerThreadId(0))->insertOffsetEntry(1,&k,&v);
    // Window B [1000,2000): leave empty
    (void)handler->getSliceAndWindowStore().getSlicesOrCreate(Timestamp(1500), makeFn);

    handler->captureState();
    const auto& st = handler->getState();
    ASSERT_EQ(st.windows.size(), 2u);
    // One hash map captured for window A, zero for B
    EXPECT_EQ(st.windows[0].hashMapCount, 1u);
    EXPECT_EQ(st.windows[1].hashMapCount, 0u);

    auto tb = handler->serialize(bm.get());
    auto rec = SerializableAggregationOperatorHandler::deserialize(tb, inputs, out, std::make_unique<DefaultTimeBasedSliceStore>(1000, 1000));

    // Verify recovered A has tuples
    auto sArec = rec->getSliceAndWindowStore().getSlicesOrCreate(Timestamp(st.windows[0].start), rec->getCreateNewSlicesFunction(args));
    auto sliceArec = std::dynamic_pointer_cast<SerializableAggregationSlice>(sArec[0]);
    EXPECT_GE(sliceArec->getOffsetHashMapWrapper(WorkerThreadId(0))->getNumberOfTuples(), 1u);
    // Verify recovered B remains empty
    auto sBrec = rec->getSliceAndWindowStore().getSlicesOrCreate(Timestamp(st.windows[1].start), rec->getCreateNewSlicesFunction(args));
    auto sliceBrec = std::dynamic_pointer_cast<SerializableAggregationSlice>(sBrec[0]);
    EXPECT_EQ(sliceBrec->getOffsetHashMapWrapper(WorkerThreadId(0))->getNumberOfTuples(), 0u);
}

