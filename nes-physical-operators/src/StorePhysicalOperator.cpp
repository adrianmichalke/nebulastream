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


StorePhysicalOperator::StorePhysicalOperator(OperatorHandlerId handlerId, const Schema& inputSchema)
    : handlerId(handlerId), inputSchema(inputSchema), encoder(this->inputSchema)
{
}

void StorePhysicalOperator::setup(ExecutionContext& executionCtx) const
{
    // Only propagate setup to the child. Avoid invoking handler lifecycle here to keep setup free of DSL side effects
    // in runtime-compiled initialization. Handler initialization happens in open/close/execute.
    if (child.has_value())
    {
        setupChild(executionCtx);
    }
}

void StorePhysicalOperator::open(ExecutionContext& executionCtx, Nautilus::RecordBuffer& recordBuffer) const
{
    // Keep child lifecycle consistent
    if (child.has_value())
    {
        openChild(executionCtx, recordBuffer);
    }
    // Start handler and ensure header at open-time per design.
    auto handler = executionCtx.getGlobalOperatorHandler(handlerId);
    nautilus::invoke(
        +[](OperatorHandler* h, PipelineExecutionContext* pctx) {
            if (!h || !pctx) { return; }
            h->start(*pctx, 0);
        },
        handler,
        executionCtx.pipelineContext);
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
    using namespace nautilus;
    // Compute row size dynamically (supports fixed & var-sized fields)
    auto rowSize = encoder.computeSize(record);

    // Reserve destination bytes in the handler's shard for the current worker
    auto handlerPtr = executionCtx.getGlobalOperatorHandler(handlerId);
    auto dst = nautilus::invoke(
        +[](OperatorHandler* h, PipelineExecutionContext* pctx, uint32_t len) -> int8_t* {
            if (auto* store = dynamic_cast<StoreOperatorHandler*>(h))
            {
                const uint32_t wid = pctx->getId().getRawValue();
                return reinterpret_cast<int8_t*>(store->reserve(wid, len));
            }
            return static_cast<int8_t*>(nullptr);
        },
        handlerPtr,
        executionCtx.pipelineContext,
        rowSize);

    // Encode directly into reserved memory
    encoder.encodeTo(record, dst);

    // Commit bytes
    nautilus::invoke(
        +[](OperatorHandler* h, PipelineExecutionContext* pctx, uint32_t len) {
            if (auto* store = dynamic_cast<StoreOperatorHandler*>(h))
            {
                const uint32_t wid = pctx->getId().getRawValue();
                store->commit(wid, len);
            }
        },
        handlerPtr,
        executionCtx.pipelineContext,
        rowSize);
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

    // Commit and optionally flush WAL on buffer close if enabled
    auto handler = executionCtx.getGlobalOperatorHandler(handlerId);
    nautilus::invoke(
        +[](OperatorHandler* h, PipelineExecutionContext* pctx) {
            if (auto* store = dynamic_cast<StoreOperatorHandler*>(h))
            {
                const uint32_t wid = pctx->getId().getRawValue();
                store->commitBuffer(wid);
            }
        },
        handler,
        executionCtx.pipelineContext);
}

void StorePhysicalOperator::terminate(ExecutionContext& executionCtx) const
{
    // Stop handler and then children
    nautilus::invoke(
        +[](OperatorHandler* h, PipelineExecutionContext* pctx) {
            if (!h || !pctx) { return; }
            h->stop(QueryTerminationType::Graceful, *pctx);
        },
        executionCtx.getGlobalOperatorHandler(handlerId),
        executionCtx.pipelineContext);
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
