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

#include <Execution/Pipelines/RuntimeTracingPipelineProvider.hpp>
#include <Execution/Pipelines/RuntimeTracingExecutablePipelineStage.hpp>
#include <nautilus/options.hpp>

namespace NES::Runtime::Execution
{

std::unique_ptr<ExecutablePipelineProvider> RegisterRuntimeTracingPipelineProvider()
{
    return std::make_unique<RuntimeTracingPipelineProvider>();
}

std::unique_ptr<ExecutablePipelineStage>
RuntimeTracingPipelineProvider::create(std::shared_ptr<PhysicalOperatorPipeline> pipeline, nautilus::engine::Options& options)
{
    /// Enable runtime tracing mode - disable compilation and enable runtime tracing
    options.setOption("engine.Compilation", false);
    options.setOption("engine.RuntimeTrace", true);
    
    /// Set default trace configuration options
    options.setOption("trace.OutputDirectory", std::string("/tmp/nes-traces/"));
    options.setOption("trace.Format", std::string("json"));
    
    return std::make_unique<RuntimeTracingExecutablePipelineStage>(pipeline, options);
}

}