/*
 Licensed under the Apache License, Version 2.0
*/
#pragma once
#include <memory>
#include <Execution/Pipelines/ExecutablePipelineProvider.hpp>

namespace NES::Runtime::Execution {

class RuntimeTracingPipelineProvider final : public ExecutablePipelineProvider {
public:
    std::unique_ptr<ExecutablePipelineStage> create(std::shared_ptr<PhysicalOperatorPipeline> pipeline,
                                                    nautilus::engine::Options& options) override;
};

} // namespace NES::Runtime::Execution
