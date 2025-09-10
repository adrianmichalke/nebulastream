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

#include <Phases/LowerToCompiledQueryPlanPhase.hpp>
#include <RecoveredOperatorHandlersRegistry.hpp>

#include <algorithm>
#include <memory>
#include <optional>
#include <ranges>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>
#include <Configuration/WorkerConfiguration.hpp>
#include <Identifiers/Identifiers.hpp>
#include <InputFormatters/InputFormatterProvider.hpp>
#include <Pipelines/CompiledExecutablePipelineStage.hpp>
#include <Sources/SourceDescriptor.hpp>
#include <Util/DumpMode.hpp>
#include <Util/ExecutionMode.hpp>
#include <CompiledQueryPlan.hpp>
#include <ErrorHandling.hpp>
#include <ExecutablePipelineStage.hpp>
#include <Pipeline.hpp>
#include <PipelinedQueryPlan.hpp>
#include <SinkPhysicalOperator.hpp>
#include <SourcePhysicalOperator.hpp>
#include <options.hpp>

namespace NES
{

LowerToCompiledQueryPlanPhase::Successor
LowerToCompiledQueryPlanPhase::processSuccessor(const Predecessor& predecessor, const std::shared_ptr<Pipeline>& pipeline)
{
    PRECONDITION(pipeline->isSinkPipeline() || pipeline->isOperatorPipeline(), "expected a Sink or OperatorPipeline");

    if (pipeline->isSinkPipeline())
    {
        processSink(predecessor, pipeline);
        return {};
    }
    return processOperatorPipeline(pipeline);
}

void LowerToCompiledQueryPlanPhase::processSource(const std::shared_ptr<Pipeline>& pipeline)
{
    PRECONDITION(pipeline->isSourcePipeline(), "expected a SourcePipeline {}", *pipeline);

    /// Convert logical source descriptor to actual source descriptor
    const auto sourceOperator = pipeline->getRootOperator().get<SourcePhysicalOperator>();

    auto desc = sourceOperator.getDescriptor();
    auto parserCfg = desc.getParserConfig();
    if ((desc.getSourceType() == "BinaryStore") && (parserCfg.parserType.empty()))
    {
        parserCfg.parserType = "Native";
    }
    // Do not bypass the InputFormatter for now; the Native formatter will schedule with NEVER to avoid duplicates

    const std::vector<std::shared_ptr<ExecutablePipeline>> executableSuccessorPipelines;
    NES_DEBUG(
        "LowerToCompiledQueryPlanPhase: Source originId={} type={} parserType={}",
        desc.getPhysicalSourceId().getRawValue(),
        desc.getSourceType(),
        parserCfg.parserType);
    // Stable bypass: for BinaryStore in INTERPRETER mode, skip InputFormatter and attach source directly to operator pipelines.
    const bool isBinaryStore = (desc.getSourceType() == std::string("BinaryStore"));
    const bool isInterpreter = (pipelineQueryPlan->getExecutionMode() == ExecutionMode::INTERPRETER);
    if (isBinaryStore && isInterpreter)
    {
        std::vector<std::weak_ptr<ExecutablePipeline>> firstStages;
        const Predecessor predecessor = sourceOperator.getOriginId();
        for (const auto& successor : pipeline->getSuccessors())
        {
            if (successor->isSinkPipeline())
            {
                // Register sink with the source as predecessor (OriginId)
                processSuccessor(predecessor, successor);
            }
            else
            {
                // Build operator pipeline starting after source and collect as a first-stage successor
                if (auto exec = processOperatorPipeline(successor))
                {
                    firstStages.emplace_back(exec);
                }
            }
        }

        // Remove the logical source pipeline from the plan (we directly connect source to its successors)
        pipelineQueryPlan->removePipeline(*pipeline);

        // Register the source with its first executable operator stages; sinks registered above will be linked at instantiation
        sources.emplace_back(sourceOperator.getOriginId(), desc, std::move(firstStages));
        return;
    }

    auto inputFormatterTaskPipeline
        = provideInputFormatterTask(sourceOperator.getOriginId(), *desc.getLogicalSource().getSchema(), parserCfg);

    auto executableInputFormatterPipeline
        = ExecutablePipeline::create(pipeline->getPipelineId(), std::move(inputFormatterTaskPipeline), executableSuccessorPipelines);

    for (const auto& successor : pipeline->getSuccessors())
    {
        if (auto executableSuccessor = processSuccessor(executableInputFormatterPipeline, successor))
        {
            executableInputFormatterPipeline->successors.emplace_back(*executableSuccessor);
        }
    }

    /// Insert the executable pipeline into the pipelineQueryPlan at position 1 (after the source)
    pipelineQueryPlan->removePipeline(*pipeline);

    std::vector<std::weak_ptr<ExecutablePipeline>> inputFormatterTasks;

    pipelineToExecutableMap.emplace(getNextPipelineId(), executableInputFormatterPipeline);
    inputFormatterTasks.emplace_back(executableInputFormatterPipeline);

    sources.emplace_back(sourceOperator.getOriginId(), desc, std::move(inputFormatterTasks));
}

void LowerToCompiledQueryPlanPhase::processSink(const Predecessor& predecessor, const std::shared_ptr<Pipeline>& pipeline)
{
    const auto sinkOperator = pipeline->getRootOperator().get<SinkPhysicalOperator>().getDescriptor();
    auto it = std::ranges::find(sinks, pipeline->getPipelineId(), &CompiledQueryPlan::Sink::id);
    if (it == sinks.end())
    {
        sinks.emplace_back(pipeline->getPipelineId(), sinkOperator, std::vector<Predecessor>{});
        it = sinks.end() - 1;
    }
    it->predecessor.emplace_back(predecessor);
}

std::unique_ptr<ExecutablePipelineStage> LowerToCompiledQueryPlanPhase::getStage(const std::shared_ptr<Pipeline>& pipeline)
{
    nautilus::engine::Options options;
    switch (pipelineQueryPlan->getExecutionMode())
    {
        case ExecutionMode::COMPILER: {
            options.setOption("engine.Compilation", true);
            break;
        }
        case ExecutionMode::INTERPRETER: {
            options.setOption("engine.Compilation", false);
            break;
        }
        default: {
            INVARIANT(false, "Invalid backend");
        }
    }
    /// See: https://github.com/nebulastream/nautilus/blob/main/docs/options.md
    switch (dumpQueryCompilationIntermediateRepresentations)
    {
        case DumpMode::NONE:
            options.setOption("dump.all", false);
            options.setOption("dump.console", false);
            options.setOption("dump.file", false);
            break;
        case DumpMode::CONSOLE:
            options.setOption("dump.all", true);
            options.setOption("dump.console", true);
            options.setOption("dump.file", false);
            break;
        case DumpMode::FILE:
            options.setOption("dump.all", true);
            options.setOption("dump.console", false);
            options.setOption("dump.file", true);
            break;
        case DumpMode::FILE_AND_CONSOLE:
            options.setOption("dump.all", true);
            options.setOption("dump.console", true);
            options.setOption("dump.file", true);
            break;
    }
    {
        auto merged = pipeline->getOperatorHandlers();
        auto recovered = RecoveredOperatorHandlersRegistry::getForQuery(pipelineQueryPlan->getQueryId());
        for (auto& kv : recovered) {
            merged[kv.first] = kv.second;
        }
        return std::make_unique<CompiledExecutablePipelineStage>(pipeline, merged, options);
    }
}

std::shared_ptr<ExecutablePipeline> LowerToCompiledQueryPlanPhase::processOperatorPipeline(const std::shared_ptr<Pipeline>& pipeline)
{
    /// check if the particular pipeline already exist in the pipeline map.
    if (const auto executable = pipelineToExecutableMap.find(pipeline->getPipelineId()); executable != pipelineToExecutableMap.end())
    {
        return executable->second;
    }
    auto executablePipeline = ExecutablePipeline::create(PipelineId(pipeline->getPipelineId()), getStage(pipeline), {});

    for (const auto& successor : pipeline->getSuccessors())
    {
        if (auto executableSuccessor = processSuccessor(executablePipeline, successor))
        {
            executablePipeline->successors.emplace_back(*executableSuccessor);
        }
    }

    pipelineToExecutableMap.emplace(pipeline->getPipelineId(), executablePipeline);
    return executablePipeline;
}

std::unique_ptr<CompiledQueryPlan> LowerToCompiledQueryPlanPhase::apply(const std::shared_ptr<PipelinedQueryPlan>& pipelineQueryPlan)
{
    this->pipelineQueryPlan = pipelineQueryPlan;

    ///Process all pipelines recursively.
    auto sourcePipelines = pipelineQueryPlan->getSourcePipelines();
    for (const auto& pipeline : sourcePipelines)
    {
        processSource(pipeline);
    }

    auto pipelines = std::move(pipelineToExecutableMap) | std::views::values | std::ranges::to<std::vector>();

    return CompiledQueryPlan::create(pipelineQueryPlan->getQueryId(), std::move(pipelines), std::move(sinks), std::move(sources));
}

}
