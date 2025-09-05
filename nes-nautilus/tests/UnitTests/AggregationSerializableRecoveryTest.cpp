/*
    Licensed under the Apache License, Version 2.0
*/

#include <gtest/gtest.h>
#include <Aggregation/SerializableAggregationOperatorHandler.hpp>
#include <Aggregation/SerializableAggregationSlice.hpp>
#include <Nautilus/State/Conversion/AggregationStateConverters.hpp>
#include <Nautilus/Recovery/CheckpointCoordinator.hpp>
#include <Nautilus/Recovery/RecoveryManager.hpp>
#include <Runtime/BufferManager.hpp>
#include <SliceStore/DefaultTimeBasedSliceStore.hpp>

using namespace NES;

static std::string mkTmpAggSer() {
    char tmpl[] = "/tmp/nes_agg_ser_ckpt_XXXXXX";
    int fd = mkstemp(tmpl);
    if (fd >= 0) close(fd);
    std::remove(tmpl);
    return std::string(tmpl);
}

TEST(AggregationSerializableRecoveryTest, NonEmpty_Capture_Bytes_NotEmpty_And_RoundTrip)
{
    auto bufferManager = BufferManager::create(64 * 1024, 64);
    std::vector<OriginId> inputs{OriginId(1)};
    OriginId out{2};

    auto handler = std::make_shared<SerializableAggregationOperatorHandler>(
        inputs, out, std::make_unique<DefaultTimeBasedSliceStore>(1000, 1000));
    handler->setWorkerThreads(2);

    // Create a slice and insert entries via offset wrappers
    const CreateNewHashMapSliceArgs args{{}, /*key*/8, /*val*/8, /*page*/4096, /*buckets*/64};
    auto makeFn = handler->getCreateNewSlicesFunction(args);
    auto slices = handler->getSliceAndWindowStore().getSlicesOrCreate(Timestamp(1000), makeFn);
    ASSERT_EQ(slices.size(), 1u);
    auto serSlice = std::dynamic_pointer_cast<SerializableAggregationSlice>(slices[0]);
    ASSERT_TRUE(serSlice != nullptr);

    auto* w0 = serSlice->getOffsetHashMapWrapper(WorkerThreadId(0));
    auto* w1 = serSlice->getOffsetHashMapWrapper(WorkerThreadId(1));
    uint64_t key = 42, val = 7;
    ASSERT_NE(w0, nullptr);
    ASSERT_NE(w1, nullptr);
    (void)w0->insertOffsetEntry(/*hash*/42, &key, &val);
    (void)w1->insertOffsetEntry(/*hash*/43, &key, &val);
    EXPECT_GE(w0->getNumberOfTuples() + w1->getNumberOfTuples(), 1u);

    // Build pipeline state and ensure non-empty bytes
    handler->captureState();
    Recovery::OperatorDescriptor d{.operatorId = 77, .kind = State::OperatorStateTag::Kind::Aggregation};
    State::ProgressMetadata pm{}; pm.version = 1; pm.lastWatermark = 1000;
    auto ps = Recovery::CheckpointCoordinator::buildPipelineState(1, 1, 999ULL,
        std::vector<std::shared_ptr<OperatorHandler>>{handler}, std::vector<Recovery::OperatorDescriptor>{d}, pm, bufferManager.get());

    ASSERT_EQ(ps.operators.size(), 1u);
    EXPECT_GT(ps.operators[0].bytes.size(), 0u);

    // Write and recover
    auto path = mkTmpAggSer();
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
    auto* rec = dynamic_cast<SerializableAggregationOperatorHandler*>(recovered.handlers[0].get());
    ASSERT_NE(rec, nullptr);

    // Bytes equality post-recovery
    auto tb1 = handler->serialize(bufferManager.get());
    auto tb2 = rec->serialize(bufferManager.get());
    ASSERT_EQ(tb1.getBufferSize(), tb2.getBufferSize());
    EXPECT_EQ(std::memcmp(tb1.getBuffer(), tb2.getBuffer(), tb1.getBufferSize()), 0);
    std::remove(path.c_str());
}

