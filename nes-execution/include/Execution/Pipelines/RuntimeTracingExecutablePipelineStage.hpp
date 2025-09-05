/*
 Licensed under the Apache License, Version 2.0
*/
#pragma once
#include <memory>
#include <nautilus/Engine.hpp>
#include <Execution/Pipelines/CompiledExecutablePipelineStage.hpp>

namespace NES::Runtime::Execution {

class RuntimeTracingExecutablePipelineStage : public CompiledExecutablePipelineStage {
public:
    RuntimeTracingExecutablePipelineStage(const std::shared_ptr<PhysicalOperatorPipeline>& physicalOperatorPipeline,
                                          nautilus::engine::Options options);
    uint32_t stop(PipelineExecutionContext& pipelineExecutionContext) override;
protected:
    nautilus::engine::CallableFunction<void, WorkerContext*, PipelineExecutionContext*, Memory::TupleBuffer*>
    compilePipeline() const override;

private:
    std::string generateTraceFilename(PipelineExecutionContext&) const;
    void saveRuntimeTrace(const std::string& filename) const;

    nautilus::engine::Options options;
};

} // namespace NES::Runtime::Execution
