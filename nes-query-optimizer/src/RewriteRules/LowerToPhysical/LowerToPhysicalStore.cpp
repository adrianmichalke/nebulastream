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

#include <memory>
#include <sstream>
#include <Operators/LogicalOperator.hpp>
#include <Operators/StoreLogicalOperator.hpp>
#include <RewriteRules/AbstractRewriteRule.hpp>
#include <RewriteRules/LowerToPhysical/LowerToPhysicalStore.hpp>
#include <ErrorHandling.hpp>
#include <PhysicalOperator.hpp>
#include <RewriteRuleRegistry.hpp>
#include <StorePhysicalOperator.hpp>
#include <StoreOperatorHandler.hpp>

namespace NES
{

RewriteRuleResultSubgraph LowerToPhysicalStore::apply(LogicalOperator logicalOperator)
{
    PRECONDITION(logicalOperator.tryGet<StoreLogicalOperator>(), "Expected a StoreLogicalOperator");
    auto store = logicalOperator.get<StoreLogicalOperator>();

    // Prepare handler config from logical operator config
    auto cfgCopy = DescriptorConfig::Config(store.getConfig());
    Descriptor logicalCfg(std::move(cfgCopy));
    const auto filePath = logicalCfg.getFromConfig(StoreLogicalOperator::ConfigParameters::FILE_PATH);
    const auto append = logicalCfg.getFromConfig(StoreLogicalOperator::ConfigParameters::APPEND);
    const auto header = logicalCfg.getFromConfig(StoreLogicalOperator::ConfigParameters::HEADER);
    const auto directIO = logicalCfg.getFromConfig(StoreLogicalOperator::ConfigParameters::DIRECT_IO);
    const auto fdatasyncInterval = logicalCfg.getFromConfig(StoreLogicalOperator::ConfigParameters::FDATASYNC_INTERVAL);
    const auto flushOnClose = logicalCfg.getFromConfig(StoreLogicalOperator::ConfigParameters::FLUSH_ON_CLOSE);
    const auto chunkMinBytes = logicalCfg.getFromConfig(StoreLogicalOperator::ConfigParameters::CHUNK_MIN_BYTES);
    const auto asyncBackendLogical = logicalCfg.getFromConfig(StoreLogicalOperator::ConfigParameters::ASYNC_BACKEND);

    std::stringstream schemaStream;
    schemaStream << logicalOperator.getOutputSchema();

    // Map logical backend enum to handler backend enum (kept local to avoid coupling layers)
    StoreOperatorHandler::AsyncBackend handlerBackend
        = (asyncBackendLogical == StoreAsyncBackend::IO_URING) ? StoreOperatorHandler::AsyncBackend::IO_URING
                                                               : StoreOperatorHandler::AsyncBackend::POSIX;

    StoreOperatorHandler::Config handlerCfg{
        .filePath = filePath,
        .append = append,
        .header = header,
        .directIO = directIO,
        .fdatasyncInterval = fdatasyncInterval,
        .schemaText = schemaStream.str(),
        .chunkMinBytes = chunkMinBytes,
        .asyncBackend = handlerBackend,
        .flushOnClose = flushOnClose,
    };

    auto handlerId = getNextOperatorHandlerId();
    auto handler = std::make_shared<StoreOperatorHandler>(handlerCfg);

    // Build physical operator and wrapper
    const auto inputSchema = logicalOperator.getInputSchemas()[0];
    const auto outputSchema = logicalOperator.getOutputSchema();
    auto physicalOperator = StorePhysicalOperator(handlerId, inputSchema);
    auto wrapper = std::make_shared<PhysicalOperatorWrapper>(
        physicalOperator, inputSchema, outputSchema, handlerId, handler, PhysicalOperatorWrapper::PipelineLocation::INTERMEDIATE);

    // Leaf where the lowered child(ren) will be attached: for unary store this is the store itself
    const std::vector leafs{wrapper};
    return {.root = wrapper, .leafs = leafs};
}

std::unique_ptr<AbstractRewriteRule>
RewriteRuleGeneratedRegistrar::RegisterStoreRewriteRule(RewriteRuleRegistryArguments argument) /// NOLINT
{
    return std::make_unique<LowerToPhysicalStore>(argument.conf);
}

}
