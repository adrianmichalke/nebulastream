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

#include <optional>
#include <utility>

#include <ExecutionContext.hpp>
#include <PipelineExecutionContext.hpp>
#include <Nautilus/Interface/Record.hpp>
#include <Nautilus/Interface/RecordBuffer.hpp>
#include <PhysicalOperator.hpp>
#include <StorePhysicalOperator.hpp>
#include <Runtime/Execution/OperatorHandler.hpp>
#include <StoreOperatorHandler.hpp>
#include <Runtime/QueryTerminationType.hpp>
#include <ErrorHandling.hpp>
#include <fmt/format.h>

namespace NES
{

namespace
{
// Proxy helpers similar to patterns in WindowProbePhysicalOperator
void setupHandlerProxy(OperatorHandler* handler, PipelineExecutionContext* pipelineCtx)
{
    if (!handler || !pipelineCtx)
    {
        return;
    }
    handler->start(*pipelineCtx, 0);
}

void stopHandlerProxy(OperatorHandler* handler, PipelineExecutionContext* pipelineCtx)
{
    PRECONDITION(handler != nullptr, "OperatorHandler must not be null");
    PRECONDITION(pipelineCtx != nullptr, "PipelineExecutionContext must not be null");
    handler->stop(QueryTerminationType::Graceful, *pipelineCtx);
}
}

StorePhysicalOperator::StorePhysicalOperator(OperatorHandlerId handlerId, const Schema& inputSchema)
    : handlerId(handlerId), inputSchema(inputSchema)
{
    fieldNames.reserve(this->inputSchema.getNumberOfFields());
    fieldTypes.reserve(this->inputSchema.getNumberOfFields());
    fieldSizes.reserve(this->inputSchema.getNumberOfFields());
    fieldOffsets.reserve(this->inputSchema.getNumberOfFields());
    uint32_t offset = 0;
    for (const auto& f : this->inputSchema.getFields())
    {
        fieldNames.emplace_back(f.name);
        fieldTypes.emplace_back(f.dataType);
        uint32_t sz = 0;
        switch (f.dataType.type)
        {
            case DataType::Type::VARSIZED:
            case DataType::Type::VARSIZED_POINTER_REP:
                sz = 0; // unsupported in phase 1
                break;
            default:
                sz = f.dataType.getSizeInBytes();
        }
        fieldSizes.emplace_back(sz);
        fieldOffsets.emplace_back(offset);
        offset += sz;
    }
    rowWidth = offset;
}

void StorePhysicalOperator::setup(ExecutionContext& executionCtx) const
{
    // Start the handler once per pipeline lifetime
    if (child.has_value())
    {
        setupChild(executionCtx);
    }
    nautilus::invoke(setupHandlerProxy, executionCtx.getGlobalOperatorHandler(handlerId), executionCtx.pipelineContext);
}

void StorePhysicalOperator::open(ExecutionContext& executionCtx, Nautilus::RecordBuffer& recordBuffer) const
{
    // Keep child lifecycle consistent
    if (child.has_value())
    {
        openChild(executionCtx, recordBuffer);
    }
    // Ensure header creation at open-time per design.
    auto handler = executionCtx.getGlobalOperatorHandler(handlerId);
    nautilus::invoke(
        +[](OperatorHandler* h, PipelineExecutionContext* pctx) {
            if (!h || !pctx)
            {
                return;
            }
            if (auto* store = dynamic_cast<StoreOperatorHandler*>(h))
            {
                store->ensureHeader(*pctx);
            }
        },
        handler,
        executionCtx.pipelineContext);
}

void StorePhysicalOperator::encodeAndAppend(Nautilus::Record& record, ExecutionContext& executionCtx) const
{
    // Encode fixed-width fields into a host buffer to avoid nautilus arena lifetime issues
    // Note: variable-sized fields are not supported in phase 1
    std::vector<uint8_t> row;
    row.resize(rowWidth);
    for (size_t i = 0; i < fieldNames.size(); ++i)
    {
        const auto sz = fieldSizes[i];
        if (sz == 0)
        {
            // Skip var-sized fields in phase 1
            continue;
        }
        const auto name = fieldNames[i];
        const auto dstPtr = reinterpret_cast<int8_t*>(row.data() + fieldOffsets[i]);
        auto dstVal = nautilus::val<int8_t*>(dstPtr);
        const auto vv = record.read(name);
        vv.writeToMemory(dstVal);
    }

    // Append via handler
    auto handler = executionCtx.getGlobalOperatorHandler(handlerId);
    auto dataPtr = nautilus::val<int8_t*>(reinterpret_cast<int8_t*>(row.data()));
    nautilus::invoke(
        +[](OperatorHandler* h, int8_t* data, uint32_t len) {
            if (auto* store = dynamic_cast<StoreOperatorHandler*>(h))
            {
                store->append(reinterpret_cast<const uint8_t*>(data), static_cast<size_t>(len));
            }
        },
        handler,
        dataPtr,
        nautilus::val<uint32_t>(rowWidth));
}

void StorePhysicalOperator::execute(ExecutionContext& executionCtx, Nautilus::Record& record) const
{
    // Encode and append each record once per execute call, then forward the record.
    encodeAndAppend(record, executionCtx);
    if (child.has_value())
    {
        executeChild(executionCtx, record);
    }
}

void StorePhysicalOperator::close(ExecutionContext& executionCtx, Nautilus::RecordBuffer& recordBuffer) const
{
    // No buffered data in Phase 3; close child as usual
    if (child.has_value())
    {
        closeChild(executionCtx, recordBuffer);
    }
}

void StorePhysicalOperator::terminate(ExecutionContext& executionCtx) const
{
    // Stop handler and then children
    nautilus::invoke(stopHandlerProxy, executionCtx.getGlobalOperatorHandler(handlerId), executionCtx.pipelineContext);
    if (child.has_value())
    {
        terminateChild(executionCtx);
    }
}

std::optional<PhysicalOperator> StorePhysicalOperator::getChild() const
{
    return child;
}

void StorePhysicalOperator::setChild(PhysicalOperator c)
{
    child = std::move(c);
}

}
