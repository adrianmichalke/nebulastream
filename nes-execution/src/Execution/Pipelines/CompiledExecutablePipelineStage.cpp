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
#include <cstdint>
#include <functional>
#include <utility>
#include <ExecutionContext.hpp>
#include <Execution/Pipelines/CompiledExecutablePipelineStage.hpp>
#include <Nautilus/Interface/RecordBuffer.hpp>
#include <Util/DumpHelper.hpp>
#include <Util/Timer.hpp>
#include <nautilus/val.hpp>
#include <nautilus/val_ptr.hpp>
#include <ErrorHandling.hpp>

namespace NES::Runtime::Execution
{

CompiledExecutablePipelineStage::CompiledExecutablePipelineStage(
    const std::shared_ptr<PhysicalOperatorPipeline>& physicalOperatorPipeline, nautilus::engine::Options options)
    : options(std::move(options)), physicalOperatorPipeline(physicalOperatorPipeline), pipelineFunctionCompiled(nullptr)
{
}

void CompiledExecutablePipelineStage::execute(
    const TupleBuffer& inputTupleBuffer, PipelineExecutionContext& pipelineExecutionContext)
{
    /// we call the compiled pipeline function with an input buffer and the execution context
    pipelineFunctionCompiled(&pipelineExecutionContext, std::addressof(inputTupleBuffer), nullptr);
}

nautilus::engine::CallableFunction<void, PipelineExecutionContext*, const TupleBuffer*, const Arena*>
CompiledExecutablePipelineStage::compilePipeline() const
{
    Timer timer("compiler");
    timer.start();

    /// We must capture the physicalOperatorPipeline by value to ensure it is not destroyed before the function is called
    /// Additionally, we can NOT use const or const references for the parameters of the lambda function
    const std::function compiledFunction = [&](nautilus::val<PipelineExecutionContext*> pipelineExecutionContext,
                                               nautilus::val<const TupleBuffer*> recordBufferRef,
                                               nautilus::val<const Arena*>)
    {
        auto ctx = ExecutionContext(pipelineExecutionContext, nautilus::val<const Arena*>{nullptr});
        RecordBuffer recordBuffer(recordBufferRef);
        physicalOperatorPipeline->getRootOperator().open(ctx, recordBuffer);
        physicalOperatorPipeline->getRootOperator().close(ctx, recordBuffer);
    };

    const nautilus::engine::NautilusEngine engine(options);
    auto executable = engine.registerFunction(compiledFunction);
    timer.snapshot("Compiled");
    timer.pause();
    NES_INFO("Timer: {}", fmt::streamed(timer));
    return executable;
}

void CompiledExecutablePipelineStage::stop(PipelineExecutionContext& pipelineExecutionContext)
{
    const auto pipelineExecutionContextRef = nautilus::val<PipelineExecutionContext*>(&pipelineExecutionContext);
    auto ctx = ExecutionContext(pipelineExecutionContextRef, nautilus::val<const Arena*>{nullptr});
    physicalOperatorPipeline->getRootOperator().terminate(ctx);
}

void CompiledExecutablePipelineStage::start(PipelineExecutionContext& pipelineExecutionContext)
{
    pipelineFunctionCompiled = this->compilePipeline();
}

}
