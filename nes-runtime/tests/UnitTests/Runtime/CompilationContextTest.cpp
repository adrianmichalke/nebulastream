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

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>

#include <gtest/gtest.h>

#include <Identifiers/Identifiers.hpp>
#include <Identifiers/NESStrongType.hpp>
#include <Runtime/AbstractBufferProvider.hpp>
#include <Runtime/Execution/OperatorHandler.hpp>
#include <Runtime/QueryTerminationType.hpp>
#include <Runtime/TupleBuffer.hpp>
#include <CompilationContext.hpp>
#include <Engine.hpp>
#include <PhysicalOperator.hpp>
#include <options.hpp>

namespace NES
{
namespace
{
class MockPipelineExecutionContext final : public PipelineExecutionContext
{
public:
    explicit MockPipelineExecutionContext(const PipelineId pipelineId) : pipelineId(pipelineId) { }

    bool emitBuffer(const TupleBuffer&, ContinuationPolicy) override { throw std::logic_error("unused"); }

    void repeatTask(const TupleBuffer&, std::chrono::milliseconds) override { throw std::logic_error("unused"); }

    TupleBuffer allocateTupleBuffer() override { throw std::logic_error("unused"); }

    TupleBuffer& pinBuffer(TupleBuffer&&) override { throw std::logic_error("unused"); }

    [[nodiscard]] WorkerThreadId getId() const override { return INITIAL<WorkerThreadId>; }

    [[nodiscard]] uint64_t getNumberOfWorkerThreads() const override { return 1; }

    [[nodiscard]] std::shared_ptr<AbstractBufferProvider> getBufferManager() const override { return nullptr; }

    [[nodiscard]] PipelineId getPipelineId() const override { return pipelineId; }

    std::unordered_map<OperatorHandlerId, std::shared_ptr<OperatorHandler>>& getOperatorHandlers() override { return operatorHandlers; }

    void setOperatorHandlers(std::unordered_map<OperatorHandlerId, std::shared_ptr<OperatorHandler>>& operatorHandlers) override
    {
        this->operatorHandlers = operatorHandlers;
    }

private:
    PipelineId pipelineId;
    std::unordered_map<OperatorHandlerId, std::shared_ptr<OperatorHandler>> operatorHandlers;
};

class TestOperatorHandler final : public OperatorHandler
{
public:
    void start(PipelineExecutionContext&, uint32_t) override { }

    void stop(QueryTerminationType, PipelineExecutionContext&) override { }

    bool wasCompiled = false;
};

struct CountingLeafOperator final : PhysicalOperatorConcept
{
    CountingLeafOperator(int& compileCount, int& setupCount, OperatorHandlerId handlerId)
        : compileCount(&compileCount), setupCount(&setupCount), handlerId(handlerId)
    {
    }

    [[nodiscard]] std::optional<PhysicalOperator> getChild() const override { return std::nullopt; }

    void setChild(PhysicalOperator) override { throw std::logic_error("unused"); }

    void compile(CompilationContext&) const override { ++(*compileCount); }

    void setup(ExecutionContext&) const override { ++(*setupCount); }

    int* compileCount;
    int* setupCount;
    OperatorHandlerId handlerId;
};

struct ParentOperator final : PhysicalOperatorConcept
{
    explicit ParentOperator(PhysicalOperator child) : child(std::move(child)) { }

    [[nodiscard]] std::optional<PhysicalOperator> getChild() const override { return child; }

    void setChild(PhysicalOperator child) override { this->child = std::move(child); }

    std::optional<PhysicalOperator> child;
};

struct CompileStopOperator final : PhysicalOperatorConcept
{
    explicit CompileStopOperator(PhysicalOperator child) : child(std::move(child)) { }

    void compile(CompilationContext&) const override { }

    [[nodiscard]] std::optional<PhysicalOperator> getChild() const override { return child; }

    void setChild(PhysicalOperator child) override { this->child = std::move(child); }

    std::optional<PhysicalOperator> child;
};
}

TEST(CompilationContextTest, TracksRegistrationPhaseAndCallsite)
{
    CompilationContext::resetRegistrationEvents();
    const nautilus::engine::Options options;
    const nautilus::engine::NautilusEngine engine(options);
    const CompilationContext compilationContext(engine);
    const auto pipelineId = PipelineId(7);

    {
        const CompilationContext::ScopedRegistrationPhase scopedRegistrationPhase(
            CompilationContext::RegistrationPhase::Compile, pipelineId);
        compilationContext.registerFunction(std::function<void()>([]() { }));
    }

    {
        const CompilationContext::ScopedRegistrationPhase scopedRegistrationPhase(CompilationContext::RegistrationPhase::Start, pipelineId);
        compilationContext.registerFunction(std::function<void()>([]() { }));
    }

    const auto events = CompilationContext::getRegistrationEvents();
    ASSERT_EQ(events.size(), 2);
    EXPECT_EQ(events[0].phase, CompilationContext::RegistrationPhase::Compile);
    EXPECT_EQ(events[1].phase, CompilationContext::RegistrationPhase::Start);
    EXPECT_EQ(events[0].pipelineId, pipelineId);
    EXPECT_EQ(events[1].pipelineId, pipelineId);
    EXPECT_NE(events[0].fileName.find("CompilationContextTest.cpp"), std::string::npos);
    EXPECT_NE(events[1].fileName.find("CompilationContextTest.cpp"), std::string::npos);
    EXPECT_GE(events[0].duration, std::chrono::nanoseconds::zero());
    EXPECT_GE(events[1].duration, std::chrono::nanoseconds::zero());
    EXPECT_EQ(CompilationContext::getRegistrationEvents(pipelineId).size(), 2);
    EXPECT_EQ(CompilationContext::getRegistrationCount(CompilationContext::RegistrationPhase::Compile), 1);
    EXPECT_EQ(CompilationContext::getRegistrationCount(CompilationContext::RegistrationPhase::Start), 1);
    EXPECT_EQ(CompilationContext::getRegistrationCount(pipelineId, CompilationContext::RegistrationPhase::Compile), 1);
    EXPECT_EQ(CompilationContext::getRegistrationCount(pipelineId, CompilationContext::RegistrationPhase::Start), 1);
    EXPECT_GE(
        CompilationContext::getRegistrationDuration(pipelineId, CompilationContext::RegistrationPhase::Compile),
        std::chrono::nanoseconds::zero());
}

TEST(CompilationContextTest, CompilationHookRecursesWithoutRunningSetup)
{
    CompilationContext::resetRegistrationEvents();
    const nautilus::engine::Options options;
    const nautilus::engine::NautilusEngine engine(options);
    const auto operatorHandlerId = OperatorHandlerId(3);

    int compileCount = 0;
    int setupCount = 0;
    const PhysicalOperator root = ParentOperator(PhysicalOperator(CountingLeafOperator(compileCount, setupCount, operatorHandlerId)));

    CompilationContext compilationContext(engine);
    root.compile(compilationContext);

    EXPECT_EQ(compileCount, 1);
    EXPECT_EQ(setupCount, 0);
}

TEST(CompilationContextTest, CompilationOverrideCanStopRecursion)
{
    CompilationContext::resetRegistrationEvents();
    const nautilus::engine::Options options;
    const nautilus::engine::NautilusEngine engine(options);
    const auto operatorHandlerId = OperatorHandlerId(4);

    int compileCount = 0;
    int setupCount = 0;
    const PhysicalOperator root = CompileStopOperator(PhysicalOperator(CountingLeafOperator(compileCount, setupCount, operatorHandlerId)));

    CompilationContext compilationContext(engine);
    root.compile(compilationContext);

    EXPECT_EQ(compileCount, 0);
    EXPECT_EQ(setupCount, 0);
}
}
