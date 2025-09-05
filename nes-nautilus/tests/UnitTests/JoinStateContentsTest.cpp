/*
    Licensed under the Apache License, Version 2.0
*/

#include <gtest/gtest.h>

#include <Join/HashJoin/SerializableHJOperatorHandler.hpp>
#include <Join/HashJoin/SerializableHJSlice.hpp>
#include <Runtime/BufferManager.hpp>
#include <SliceStore/DefaultTimeBasedSliceStore.hpp>

using namespace NES;

class JoinStateContentsTest : public ::testing::Test {
protected:
    void SetUp() override { bm = BufferManager::create(64 * 1024, 64); }
    std::shared_ptr<BufferManager> bm;
};

TEST_F(JoinStateContentsTest, HJ_CaptureState_PreservesLeftRightMaps)
{
    std::vector<OriginId> inputs{OriginId(10), OriginId(11)};
    OriginId out{12};
    auto handler = std::make_shared<SerializableHJOperatorHandler>(
        inputs, out, std::make_unique<DefaultTimeBasedSliceStore>(1000, 1000));
    handler->setWorkerThreads(1);
    auto& st = handler->getState();
    st.config.keySize = 8; st.config.valueSize = 8; st.config.numberOfBuckets = 64; st.config.pageSize = 4096;

    const CreateNewHashMapSliceArgs args{{}, st.config.keySize, st.config.valueSize, st.config.pageSize, st.config.numberOfBuckets};
    auto makeFn = handler->getCreateNewSlicesFunction(args);
    auto slices = handler->getSliceAndWindowStore().getSlicesOrCreate(Timestamp(2500), makeFn);
    auto serSlice = std::dynamic_pointer_cast<SerializableHJSlice>(slices[0]);
    ASSERT_TRUE(serSlice != nullptr);

    uint64_t kL=1,vL=2, kR=3,vR=4;
    serSlice->getOffsetHashMapWrapper(WorkerThreadId(0), JoinBuildSideType::Left)->insertOffsetEntry(1,&kL,&vL);
    serSlice->getOffsetHashMapWrapper(WorkerThreadId(0), JoinBuildSideType::Right)->insertOffsetEntry(3,&kR,&vR);

    handler->captureState();
    const auto& s = handler->getState();
    ASSERT_EQ(s.windows.size(), 1u);
    // 2 maps captured (left,right)
    EXPECT_EQ(s.windows[0].hashMapCount, 2u);
    uint64_t sum = 0; for (const auto& hm : s.hashMaps) sum += hm.tupleCount; EXPECT_GE(sum, 1u);

    // Rehydrate and verify
    auto tb = handler->serialize(bm.get());
    auto rec = SerializableHJOperatorHandler::deserialize(tb, inputs, out, std::make_unique<DefaultTimeBasedSliceStore>(1000, 1000));
    auto recSlices = rec->getSliceAndWindowStore().getSlicesOrCreate(Timestamp(s.windows[0].start), rec->getCreateNewSlicesFunction(args));
    auto recSlice = std::dynamic_pointer_cast<SerializableHJSlice>(recSlices[0]);
    ASSERT_TRUE(recSlice != nullptr);
    auto* l = recSlice->getOffsetHashMapWrapper(WorkerThreadId(0), JoinBuildSideType::Left);
    auto* r = recSlice->getOffsetHashMapWrapper(WorkerThreadId(0), JoinBuildSideType::Right);
    EXPECT_GE(l->getNumberOfTuples() + r->getNumberOfTuples(), 1u);
}

