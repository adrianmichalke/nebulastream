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

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <memory>
#include <optional>
#include <ranges>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>
#include <Configuration/WorkerConfiguration.hpp>
#include <Identifiers/Identifiers.hpp>
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

    std::vector<std::weak_ptr<ExecutablePipeline>> executableSuccessorPipelines;

    for (const auto& successor : pipeline->getSuccessors())
    {
        if (auto executableSuccessor = processSuccessor(sourceOperator.id, successor))
        {
            executableSuccessorPipelines.emplace_back(*executableSuccessor);
        }
    }
    sources.emplace_back(
        sourceOperator.getOriginId(), sourceOperator.id, sourceOperator.getDescriptor(), std::move(executableSuccessorPipelines));
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

uint64_t LowerToCompiledQueryPlanPhase::getStablePipelineCacheOrdinal(const std::shared_ptr<Pipeline>& pipeline)
{
    const Pipeline* pipelinePtr = pipeline.get();
    if (const auto existing = pipelineToStableCacheOrdinalMap.find(pipelinePtr); existing != pipelineToStableCacheOrdinalMap.end())
    {
        return existing->second;
    }
    const auto ordinal = nextStablePipelineCacheOrdinal++;
    pipelineToStableCacheOrdinalMap.emplace(pipelinePtr, ordinal);
    return ordinal;
}

std::unique_ptr<ExecutablePipelineStage> LowerToCompiledQueryPlanPhase::getStage(const std::shared_ptr<Pipeline>& pipeline)
{
    nautilus::engine::Options options;
    std::string explicitCacheKeyForDebug;
    /// We disable multithreading in MLIR by default to not interfere with NebulaStream's thread model
    options.setOption("mlir.enableMultithreading", false);
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
    switch (dumpQueryCompilationIR.getDumpOption())
    {
        case DumpMode::Options::NONE:
            options.setOption("dump.all", false);
            options.setOption("dump.console", false);
            options.setOption("dump.file", false);
            break;
        case DumpMode::Options::CONSOLE:
            options.setOption("dump.all", true);
            options.setOption("dump.console", true);
            options.setOption("dump.file", false);
            break;
        case DumpMode::Options::FILE:
            options.setOption("dump.all", true);
            options.setOption("dump.console", false);
            options.setOption("dump.file", true);
            break;
        case DumpMode::Options::FILE_AND_CONSOLE:
            options.setOption("dump.all", true);
            options.setOption("dump.console", true);
            options.setOption("dump.file", true);
            break;
    }
    options.setOption("dump.graph", dumpQueryCompilationIR.isDumpGraphEnabled());

    if (const char* cacheDir = std::getenv("NES_COMPILATION_CACHE_DIR"); cacheDir && *cacheDir)
    {
        options.setOption("engine.Blob.CacheDir", std::string(cacheDir));
    }
    if (const char* cacheKeyMode = std::getenv("NES_COMPILATION_CACHE_KEY_MODE"); cacheKeyMode && *cacheKeyMode)
    {
        options.setOption("engine.Blob.CacheKeyMode", std::string(cacheKeyMode));
    }
    if (const char* cacheKey = std::getenv("NES_COMPILATION_CACHE_KEY"); cacheKey && *cacheKey)
    {
        options.setOption("engine.Blob.CacheKey", std::string(cacheKey));
    }
    else if (const char* cacheKeyPrefix = std::getenv("NES_COMPILATION_CACHE_KEY_PREFIX"); cacheKeyPrefix && *cacheKeyPrefix)
    {
        std::ostringstream pipelineSignature;
        for (auto currentOperator = std::optional<PhysicalOperator>(pipeline->getRootOperator()); currentOperator;
             currentOperator = currentOperator->getChild())
        {
            pipelineSignature << ":op" << currentOperator->toString();
        }
        std::ostringstream keyBuilder;
        keyBuilder << cacheKeyPrefix;
        keyBuilder << ":q" << pipelineQueryPlan->getQueryId().getRawValue();
        if (const auto& cacheKeySeed = pipelineQueryPlan->getCacheKeySeed(); not cacheKeySeed.empty())
        {
            keyBuilder << ":s" << cacheKeySeed;
        }
        std::vector<uint64_t> handlerIds;
        handlerIds.reserve(pipeline->getOperatorHandlers().size());
        for (const auto& [handlerId, _] : pipeline->getOperatorHandlers())
        {
            handlerIds.emplace_back(handlerId.getRawValue());
        }
        std::ranges::sort(handlerIds);

        keyBuilder << ":o" << getStablePipelineCacheOrdinal(pipeline);
        keyBuilder << ":pid" << pipeline->getPipelineId().getRawValue();
        keyBuilder << ":h[";
        for (const auto handlerId : handlerIds)
        {
            keyBuilder << handlerId << ",";
        }
        keyBuilder << "]" << pipelineSignature.str();
        explicitCacheKeyForDebug = keyBuilder.str();
        options.setOption("engine.Blob.CacheKey", explicitCacheKeyForDebug);
    }
    if (const char* skipTraceOnHit = std::getenv("NES_COMPILATION_CACHE_SKIP_TRACE_ON_HIT"); skipTraceOnHit && *skipTraceOnHit)
    {
        const std::string skipTraceOnHitValue(skipTraceOnHit);
        const bool shouldSkipTraceOnHit = !(skipTraceOnHitValue == "0" || skipTraceOnHitValue == "false" || skipTraceOnHitValue == "off");
        options.setOption("engine.Blob.SkipTraceOnHit", shouldSkipTraceOnHit);
    }
    if (const char* cacheDebug = std::getenv("NES_COMPILATION_CACHE_DEBUG"); cacheDebug && *cacheDebug)
    {
        std::ostringstream debugStream;
        debugStream << "cache-debug pipelineId=" << pipeline->getPipelineId().getRawValue();
        debugStream << " stageOrdinal=" << getStablePipelineCacheOrdinal(pipeline);
        if (!explicitCacheKeyForDebug.empty())
        {
            debugStream << " key=" << explicitCacheKeyForDebug;
        }
        debugStream << " handlers=[";
        std::vector<std::pair<uint64_t, uintptr_t>> handlerDebugEntries;
        handlerDebugEntries.reserve(pipeline->getOperatorHandlers().size());
        for (const auto& [handlerId, handler] : pipeline->getOperatorHandlers())
        {
            handlerDebugEntries.emplace_back(handlerId.getRawValue(), reinterpret_cast<uintptr_t>(handler.get()));
        }
        std::ranges::sort(
            handlerDebugEntries, [](const auto& lhs, const auto& rhs) { return lhs.first < rhs.first; });
        for (const auto& [handlerId, handlerPtr] : handlerDebugEntries)
        {
            debugStream << "(" << handlerId << ",0x" << std::hex << handlerPtr << std::dec << ")";
        }
        debugStream << "] root=" << pipeline->getRootOperator().toString();
        std::cerr << debugStream.str() << '\n';
    }
    return std::make_unique<CompiledExecutablePipelineStage>(pipeline, pipeline->getOperatorHandlers(), options);
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
    pipelineToStableCacheOrdinalMap.clear();
    nextStablePipelineCacheOrdinal = 0;

    /// Process all pipelines recursively.
    for (auto sourcePipelines = pipelineQueryPlan->getSourcePipelines(); const auto& pipeline : sourcePipelines)
    {
        processSource(pipeline);
    }

    auto pipelines = std::move(pipelineToExecutableMap) | std::views::values | std::ranges::to<std::vector>();

    return CompiledQueryPlan::create(pipelineQueryPlan->getQueryId(), std::move(pipelines), std::move(sinks), std::move(sources));
}

}
