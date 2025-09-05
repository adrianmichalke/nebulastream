/*
    Licensed under the Apache License, Version 2.0
*/

#include <gtest/gtest.h>
#include <Nautilus/Recovery/CheckpointCoordinator.hpp>
#include <Nautilus/Recovery/RecoveryManager.hpp>
#include <Runtime/BufferManager.hpp>

#include <Join/HashJoin/SerializableHJOperatorHandler.hpp>
#include <Join/NestedLoopJoin/SerializableNLJOperatorHandler.hpp>
#include <SliceStore/DefaultTimeBasedSliceStore.hpp>

using namespace NES;

static std::string mkTmpJoinSer() {
    char tmpl[] = "/tmp/nes_join_ser_ckpt_XXXXXX";
    int fd = mkstemp(tmpl);
    if (fd >= 0) close(fd);
    std::remove(tmpl);
    return std::string(tmpl);
}

TEST(JoinSerializableRecoveryTest, HJ_RoundTrip) {
    auto bufferManager = BufferManager::create(8 * 1024, 64);
    std::vector<OriginId> inputs{OriginId(1), OriginId(2)};
    OriginId out{3};
    auto hj = std::make_shared<SerializableHJOperatorHandler>(inputs, out, std::make_unique<DefaultTimeBasedSliceStore>(1000, 1000));
    hj->setWorkerThreads(2);
    hj->captureState();

    Recovery::OperatorDescriptor d{.operatorId = 99, .kind = State::OperatorStateTag::Kind::HashJoin};
    State::ProgressMetadata pm{}; pm.version = 1; pm.lastWatermark = 1000;
    auto ps = Recovery::CheckpointCoordinator::buildPipelineState(1, 1, 123456ULL,
        std::vector<std::shared_ptr<OperatorHandler>>{hj}, std::vector<Recovery::OperatorDescriptor>{d}, pm, bufferManager.get());

    const auto path = mkTmpJoinSer();
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

    // Bytes equality
    auto tb1 = hj->serialize(bufferManager.get());
    auto tb2 = rec->serialize(bufferManager.get());
    ASSERT_EQ(tb1.getBufferSize(), tb2.getBufferSize());
    EXPECT_EQ(std::memcmp(tb1.getBuffer(), tb2.getBuffer(), tb1.getBufferSize()), 0);
    std::remove(path.c_str());
}

TEST(JoinSerializableRecoveryTest, NLJ_RoundTrip) {
    auto bufferManager = BufferManager::create(8 * 1024, 64);
    std::vector<OriginId> inputs{OriginId(10), OriginId(11)};
    OriginId out{12};
    auto nlj = std::make_shared<SerializableNLJOperatorHandler>(inputs, out, std::make_unique<DefaultTimeBasedSliceStore>(1000, 1000));
    nlj->setWorkerThreads(2);
    nlj->captureState();

    Recovery::OperatorDescriptor d{.operatorId = 199, .kind = State::OperatorStateTag::Kind::NestedLoopJoin};
    State::ProgressMetadata pm{}; pm.version = 1; pm.lastWatermark = 2000;
    auto ps = Recovery::CheckpointCoordinator::buildPipelineState(2, 2, 456789ULL,
        std::vector<std::shared_ptr<OperatorHandler>>{nlj}, std::vector<Recovery::OperatorDescriptor>{d}, pm, bufferManager.get());

    const auto path = mkTmpJoinSer();
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
    auto* rec = dynamic_cast<SerializableNLJOperatorHandler*>(recovered.handlers[0].get());
    ASSERT_NE(rec, nullptr);

    auto tb1 = nlj->serialize(bufferManager.get());
    auto tb2 = rec->serialize(bufferManager.get());
    ASSERT_EQ(tb1.getBufferSize(), tb2.getBufferSize());
    EXPECT_EQ(std::memcmp(tb1.getBuffer(), tb2.getBuffer(), tb1.getBufferSize()), 0);
    std::remove(path.c_str());
}

