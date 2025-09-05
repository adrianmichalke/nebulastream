/*
    Licensed under the Apache License, Version 2.0
*/

#include <gtest/gtest.h>

#include <Join/HashJoin/SerializableHJOperatorHandler.hpp>
#include <Join/HashJoin/SerializableHJSlice.hpp>
#include <Nautilus/Recovery/CheckpointCoordinator.hpp>
#include <Nautilus/Recovery/RecoveryManager.hpp>
#include <Runtime/BufferManager.hpp>
#include <SliceStore/DefaultTimeBasedSliceStore.hpp>

using namespace NES;

static std::string mkTmpJoinNonEmpty() {
    char tmpl[] = "/tmp/nes_join_nonempty_ckpt_XXXXXX";
    int fd = mkstemp(tmpl);
    if (fd >= 0) close(fd);
    std::remove(tmpl);
    return std::string(tmpl);
}

TEST(JoinSerializableNonEmptyRecoveryTest, HJ_NonEmpty_Capture_Bytes_NotEmpty_And_RoundTrip)
{
    auto bufferManager = BufferManager::create(64 * 1024, 64);
    std::vector<OriginId> inputs{OriginId(10), OriginId(11)};
    OriginId out{12};

    auto handler = std::make_shared<SerializableHJOperatorHandler>(
        inputs, out, std::make_unique<DefaultTimeBasedSliceStore>(1000, 1000));
    handler->setWorkerThreads(2);
    // Configure sizes for simple key/value (both 8 bytes)
    auto& st = handler->getState();
    st.config.keySize = 8;
    st.config.valueSize = 8;
    st.config.numberOfBuckets = 64;
    st.config.pageSize = 4096;

    // Create a serializable slice and insert a couple of entries on both sides
    const CreateNewHashMapSliceArgs args{{}, st.config.keySize, st.config.valueSize, st.config.pageSize, st.config.numberOfBuckets};
    auto makeFn = handler->getCreateNewSlicesFunction(args);
    auto slices = handler->getSliceAndWindowStore().getSlicesOrCreate(Timestamp(5000), makeFn);
    ASSERT_EQ(slices.size(), 1u);
    auto serSlice = std::dynamic_pointer_cast<SerializableHJSlice>(slices[0]);
    ASSERT_TRUE(serSlice != nullptr);

    uint64_t keyL = 0xAAAABBBBCCCCDDDDULL, valL = 0x1111222233334444ULL;
    uint64_t keyR = 0xEEEFFFF011223344ULL, valR = 0x5555666677778888ULL;
    auto* left0 = serSlice->getOffsetHashMapWrapper(WorkerThreadId(0), JoinBuildSideType::Left);
    auto* right0 = serSlice->getOffsetHashMapWrapper(WorkerThreadId(0), JoinBuildSideType::Right);
    ASSERT_NE(left0, nullptr);
    ASSERT_NE(right0, nullptr);
    (void)left0->insertOffsetEntry(/*hash*/42, &keyL, &valL);
    (void)right0->insertOffsetEntry(/*hash*/43, &keyR, &valR);
    EXPECT_GE(left0->getNumberOfTuples() + right0->getNumberOfTuples(), 1u);

    // Capture and build PipelineState
    handler->captureState();
    Recovery::OperatorDescriptor d{.operatorId = 201, .kind = State::OperatorStateTag::Kind::HashJoin};
    State::ProgressMetadata pm{}; pm.version = 1; pm.lastWatermark = 6000;
    auto ps = Recovery::CheckpointCoordinator::buildPipelineState(9, 9, 123123ULL,
        std::vector<std::shared_ptr<OperatorHandler>>{handler}, std::vector<Recovery::OperatorDescriptor>{d}, pm, bufferManager.get());

    ASSERT_EQ(ps.operators.size(), 1u);
    EXPECT_GT(ps.operators[0].bytes.size(), 0u) << "HJ blob must not be empty";

    // Persist and recover
    const auto path = mkTmpJoinNonEmpty();
    Recovery::CheckpointCoordinator::writeCheckpoint(ps, path);
    Recovery::RecoveryManager::RecreateArgs rargs{};
    rargs.bufferProvider = bufferManager.get();
    rargs.operatorArgsById.emplace(d.operatorId, Recovery::OperatorConstructionArgs{
        .inputOrigins = inputs,
        .outputOriginId = out,
        .makeSliceStore = [](){ return std::make_unique<DefaultTimeBasedSliceStore>(1000, 1000); },
    });
    auto recovered = Recovery::RecoveryManager::recoverPipeline(path, rargs);
    ASSERT_EQ(recovered.handlers.size(), 1u);
    auto* rec = dynamic_cast<SerializableHJOperatorHandler*>(recovered.handlers[0].get());
    ASSERT_NE(rec, nullptr);

    // Serialized bytes equality check indicates correct restore of state
    auto tb1 = handler->serialize(bufferManager.get());
    auto tb2 = rec->serialize(bufferManager.get());
    ASSERT_EQ(tb1.getBufferSize(), tb2.getBufferSize());
    EXPECT_EQ(std::memcmp(tb1.getBuffer(), tb2.getBuffer(), tb1.getBufferSize()), 0);

    std::remove(path.c_str());
}

