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

#include <CompilationContext.hpp>

#include <algorithm>
#include <iterator>
#include <mutex>
#include <utility>
#include <ErrorHandling.hpp>

namespace NES
{
namespace
{
struct RegistrationScope
{
    CompilationContext::RegistrationPhase phase = CompilationContext::RegistrationPhase::Unknown;
    PipelineId pipelineId = INVALID_PIPELINE_ID;
};

thread_local RegistrationScope activeRegistrationScope;
std::mutex registrationEventsMutex;
std::vector<CompilationContext::RegistrationEvent> registrationEvents;
}

CompilationContext::ScopedRegistrationPhase::ScopedRegistrationPhase(const RegistrationPhase phase, const PipelineId pipelineId)
    : previousPhase(activeRegistrationScope.phase), previousPipelineId(activeRegistrationScope.pipelineId)
{
    activeRegistrationScope.phase = phase;
    activeRegistrationScope.pipelineId = pipelineId;
}

CompilationContext::ScopedRegistrationPhase::~ScopedRegistrationPhase()
{
    activeRegistrationScope.phase = previousPhase;
    activeRegistrationScope.pipelineId = previousPipelineId;
}

void CompilationContext::resetRegistrationEvents()
{
    std::lock_guard lock(registrationEventsMutex);
    registrationEvents.clear();
}

std::vector<CompilationContext::RegistrationEvent> CompilationContext::getRegistrationEvents()
{
    std::lock_guard lock(registrationEventsMutex);
    return registrationEvents;
}

std::vector<CompilationContext::RegistrationEvent> CompilationContext::getRegistrationEvents(const PipelineId pipelineId)
{
    std::lock_guard lock(registrationEventsMutex);
    std::vector<RegistrationEvent> filteredEvents;
    std::ranges::copy_if(registrationEvents, std::back_inserter(filteredEvents), [pipelineId](const auto& event)
    { return event.pipelineId == pipelineId; });
    return filteredEvents;
}

std::chrono::nanoseconds CompilationContext::getRegistrationDuration(const RegistrationPhase phase)
{
    std::lock_guard lock(registrationEventsMutex);
    auto totalDuration = std::chrono::nanoseconds::zero();
    for (const auto& event : registrationEvents)
    {
        if (event.phase == phase)
        {
            totalDuration += event.duration;
        }
    }
    return totalDuration;
}

std::chrono::nanoseconds CompilationContext::getRegistrationDuration(const PipelineId pipelineId, const RegistrationPhase phase)
{
    std::lock_guard lock(registrationEventsMutex);
    auto totalDuration = std::chrono::nanoseconds::zero();
    for (const auto& event : registrationEvents)
    {
        if (event.pipelineId == pipelineId and event.phase == phase)
        {
            totalDuration += event.duration;
        }
    }
    return totalDuration;
}

uint64_t CompilationContext::getRegistrationCount(const RegistrationPhase phase)
{
    std::lock_guard lock(registrationEventsMutex);
    return std::ranges::count_if(registrationEvents, [phase](const auto& event) { return event.phase == phase; });
}

uint64_t CompilationContext::getRegistrationCount(const PipelineId pipelineId, const RegistrationPhase phase)
{
    std::lock_guard lock(registrationEventsMutex);
    return std::ranges::count_if(registrationEvents, [pipelineId, phase](const auto& event)
    { return event.pipelineId == pipelineId and event.phase == phase; });
}

void CompilationContext::recordRegistrationEvent(const std::source_location& location, const std::chrono::nanoseconds duration)
{
    std::lock_guard lock(registrationEventsMutex);
    registrationEvents.emplace_back(
        activeRegistrationScope.phase,
        activeRegistrationScope.pipelineId,
        location.file_name(),
        location.function_name(),
        location.line(),
        duration);
}

OperatorHandler* PipelineCompilationContext::getOperatorHandler(const OperatorHandlerId handlerId) const
{
    auto& operatorHandlers = pipelineExecutionContext.getOperatorHandlers();
    const auto operatorHandlerIterator = operatorHandlers.find(handlerId);
    PRECONDITION(operatorHandlerIterator != operatorHandlers.end(), "Could not find operator handler {} during pipeline compilation", handlerId);
    PRECONDITION(operatorHandlerIterator->second != nullptr, "Operator handler {} must not be null during pipeline compilation", handlerId);
    return operatorHandlerIterator->second.get();
}
}
