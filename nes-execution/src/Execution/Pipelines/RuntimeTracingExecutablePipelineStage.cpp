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

#include <Execution/Pipelines/RuntimeTracingExecutablePipelineStage.hpp>
#include <Execution/Pipelines/PhysicalOperatorPipeline.hpp>
#include <Execution/Operators/ExecutionContext.hpp>
#include <Nautilus/Interface/RecordBuffer.hpp>
#include <nautilus/tracing/runtime/RuntimeTracingInterface.hpp>
#include <Util/Logger/Logger.hpp>
#include <Util/Timer.hpp>
#include <chrono>
#include <filesystem>
#include <iomanip>
#include <sstream>

namespace NES::Runtime::Execution
{

RuntimeTracingExecutablePipelineStage::RuntimeTracingExecutablePipelineStage(
    const std::shared_ptr<PhysicalOperatorPipeline>& physicalOperatorPipeline, 
    nautilus::engine::Options options)
    : CompiledExecutablePipelineStage(physicalOperatorPipeline, options), options(options)
{
}

uint32_t RuntimeTracingExecutablePipelineStage::stop(PipelineExecutionContext& pipelineExecutionContext)
{
    // Save runtime trace before stopping the pipeline
    try {
        if (nautilus::tracing::runtime::RuntimeTracingInterface::isActive() &&
            nautilus::tracing::runtime::RuntimeTracingInterface::isRecording()) {
            
            auto traceFilename = generateTraceFilename(pipelineExecutionContext);
            saveRuntimeTrace(traceFilename);
            NES_INFO("Runtime trace saved to: {}", traceFilename);
        }
    } catch (const std::exception& e) {
        NES_WARNING("Failed to save runtime trace: {}", e.what());
        // Continue with normal pipeline termination even if trace saving fails
    }

    // Call parent stop method to properly terminate the pipeline
    return CompiledExecutablePipelineStage::stop(pipelineExecutionContext);
}

std::string RuntimeTracingExecutablePipelineStage::generateTraceFilename(
    PipelineExecutionContext& /* pipelineExecutionContext */) const
{
    // Get trace output directory from options, default to /tmp/nes-traces/
    std::string traceDir = options.getOptionOrDefault("trace.OutputDirectory", std::string("/tmp/nes-traces/"));
    
    // Ensure directory ends with separator
    if (!traceDir.empty() && traceDir.back() != '/') {
        traceDir += '/';
    }
    
    // Create directory if it doesn't exist
    std::error_code ec;
    std::filesystem::create_directories(traceDir, ec);
    if (ec) {
        NES_WARNING("Failed to create trace directory {}: {}", traceDir, ec.message());
        traceDir = "/tmp/";  // Fall back to /tmp
    }

    // Generate timestamp
    auto now = std::chrono::system_clock::now();
    auto time_t = std::chrono::system_clock::to_time_t(now);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;
    
    std::stringstream ss;
    ss << std::put_time(std::localtime(&time_t), "%Y%m%d_%H%M%S");
    ss << "_" << std::setfill('0') << std::setw(3) << ms.count();

    // Get trace format from options (json or binary)
    std::string traceFormat = options.getOptionOrDefault("trace.Format", std::string("json"));
    std::string extension = (traceFormat == "binary") ? ".btrace" : ".jtrace";
    
    // Get pipeline information - using timestamp for uniqueness since extracting IDs is complex
    // TODO: Add proper query and pipeline ID extraction once strong type conversion is resolved
    
    // Construct filename: nes_trace_TIMESTAMP.extension
    std::string filename = traceDir + "nes_trace_" + ss.str() + extension;
    
    return filename;
}

nautilus::engine::CallableFunction<void, WorkerContext*, PipelineExecutionContext*, Memory::TupleBuffer*>
RuntimeTracingExecutablePipelineStage::compilePipeline() const
{
    Timer timer("compiler");
    timer.start();

    /// We must capture the physicalOperatorPipeline by value to ensure it is not destroyed before the function is called
    /// Additionally, we can NOT use const or const references for the parameters of the lambda function
    const std::function compiledFunction = [=, this](nautilus::val<WorkerContext*> workerContext,
                                                     nautilus::val<PipelineExecutionContext*> pipelineExecutionContext,
                                                     nautilus::val<Memory::TupleBuffer*> recordBufferRef)
    {
        auto ctx = ExecutionContext(workerContext, pipelineExecutionContext);
        RecordBuffer recordBuffer(recordBufferRef);
        physicalOperatorPipeline->getRootOperator()->open(ctx, recordBuffer);
        physicalOperatorPipeline->getRootOperator()->close(ctx, recordBuffer);
        
        // Save runtime trace if tracing is active - this happens BEFORE the wrapper terminates the context
        try {
            if (nautilus::tracing::runtime::RuntimeTracingInterface::isActive() &&
                nautilus::tracing::runtime::RuntimeTracingInterface::isRecording()) {
                
                // Generate trace filename with timestamp
                auto now = std::chrono::system_clock::now();
                auto time_t = std::chrono::system_clock::to_time_t(now);
                auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;
                
                std::stringstream ss;
                ss << std::put_time(std::localtime(&time_t), "%Y%m%d_%H%M%S");
                ss << "_" << std::setfill('0') << std::setw(3) << ms.count();
                
                // Get trace output directory and format from options
                std::string traceDir = options.getOptionOrDefault("trace.OutputDirectory", std::string("/tmp/nes-traces/"));
                if (!traceDir.empty() && traceDir.back() != '/') {
                    traceDir += '/';
                }
                
                // Create directory if it doesn't exist
                std::error_code ec;
                std::filesystem::create_directories(traceDir, ec);
                
                std::string traceFormat = options.getOptionOrDefault("trace.Format", std::string("json"));
                std::string extension = (traceFormat == "binary") ? ".btrace" : ".jtrace";
                
                std::string filename = traceDir + "nes_trace_" + ss.str() + extension;
                
                // Save the trace
                nautilus::tracing::runtime::RuntimeTracingInterface::saveTrace(filename);
                NES_INFO("Runtime trace saved to: {}", filename);
            }
        } catch (const std::exception& e) {
            NES_WARNING("Failed to save runtime trace: {}", e.what());
            // Continue with normal execution even if trace saving fails
        }
    };

    const nautilus::engine::NautilusEngine engine(options);
    auto executable = engine.registerFunction(compiledFunction);
    timer.snapshot("Compiled");
    timer.pause();
    NES_INFO("Timer: {}", fmt::streamed(timer));
    return executable;
}

void RuntimeTracingExecutablePipelineStage::saveRuntimeTrace(const std::string& filename) const
{
    try {
        nautilus::tracing::runtime::RuntimeTracingInterface::saveTrace(filename);
        NES_DEBUG("Successfully saved runtime trace with {} operations", 
                  "trace data");  // Note: We don't have direct access to operation count here
    } catch (const std::exception& e) {
        throw std::runtime_error("Failed to save runtime trace to " + filename + ": " + e.what());
    }
}

}