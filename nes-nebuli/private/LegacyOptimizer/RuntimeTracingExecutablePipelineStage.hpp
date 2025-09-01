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
#pragma once

#include <Execution/Pipelines/CompiledExecutablePipelineStage.hpp>
#include <nautilus/options.hpp>

namespace NES::Runtime::Execution
{

/**
 * @brief A runtime tracing executable pipeline stage that extends CompiledExecutablePipelineStage
 * with automatic trace saving capabilities when pipeline execution completes.
 */
class RuntimeTracingExecutablePipelineStage final : public CompiledExecutablePipelineStage
{
public:
    RuntimeTracingExecutablePipelineStage(
        const std::shared_ptr<PhysicalOperatorPipeline>& physicalOperatorPipeline, 
        nautilus::engine::Options options);
    
    /**
     * @brief Overridden stop method that saves runtime traces before terminating the pipeline
     * @param pipelineExecutionContext
     * @return 0 if no error occurred
     */
    uint32_t stop(PipelineExecutionContext& pipelineExecutionContext) override;

protected:
    /**
     * @brief Override compilePipeline to add trace saving logic within the compiled function
     * @return Compiled pipeline function with integrated trace saving
     */
    [[nodiscard]] nautilus::engine::CallableFunction<void, WorkerContext*, PipelineExecutionContext*, Memory::TupleBuffer*>
    compilePipeline() const override;

private:
    /**
     * @brief Generate a unique trace filename with timestamp and pipeline information
     * @param pipelineExecutionContext
     * @return Generated filename for the trace file
     */
    std::string generateTraceFilename(PipelineExecutionContext& pipelineExecutionContext) const;
    
    /**
     * @brief Save the runtime trace to file if tracing is active
     * @param filename Path where to save the trace file
     */
    void saveRuntimeTrace(const std::string& filename) const;

    nautilus::engine::Options options;
};

}