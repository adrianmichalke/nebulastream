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

#include <chrono>
#include <cstdint>
#include <functional>
#include <source_location>
#include <string>
#include <vector>
#include <Identifiers/Identifiers.hpp>
#include <Runtime/Execution/OperatorHandler.hpp>
#include <Engine.hpp>
#include <PipelineExecutionContext.hpp>
#include <val_concepts.hpp>

namespace NES
{

class CompilationContext
{
    const nautilus::engine::NautilusEngine& engine; /// NOLINT(cppcoreguidelines-avoid-const-or-ref-data-members)

public:
    enum class RegistrationPhase : uint8_t
    {
        Unknown,
        Compile,
        Start,
    };

    struct RegistrationEvent
    {
        RegistrationPhase phase;
        PipelineId pipelineId;
        std::string fileName;
        std::string functionName;
        uint_least32_t line;
        std::chrono::nanoseconds duration;
    };

    class ScopedRegistrationPhase
    {
    public:
        ScopedRegistrationPhase(RegistrationPhase phase, PipelineId pipelineId);
        ~ScopedRegistrationPhase();

    private:
        RegistrationPhase previousPhase;
        PipelineId previousPipelineId;
    };

    explicit CompilationContext(const nautilus::engine::NautilusEngine& engine) : engine(engine) { }

    static void resetRegistrationEvents();
    [[nodiscard]] static std::vector<RegistrationEvent> getRegistrationEvents();
    [[nodiscard]] static std::vector<RegistrationEvent> getRegistrationEvents(PipelineId pipelineId);
    [[nodiscard]] static std::chrono::nanoseconds getRegistrationDuration(RegistrationPhase phase);
    [[nodiscard]] static std::chrono::nanoseconds getRegistrationDuration(PipelineId pipelineId, RegistrationPhase phase);
    [[nodiscard]] static uint64_t getRegistrationCount(RegistrationPhase phase);
    [[nodiscard]] static uint64_t getRegistrationCount(PipelineId pipelineId, RegistrationPhase phase);

    template <typename R, typename... FunctionArguments>
    auto registerFunction(
        R (*fnptr)(nautilus::val<FunctionArguments>...), const std::source_location& location = std::source_location::current()) const
    {
        return registerFunctionImpl([&]() { return engine.registerFunction<R, FunctionArguments...>(fnptr); }, location);
    }

    template <typename R, typename... FunctionArguments>
    auto registerFunction(
        std::function<R(nautilus::val<FunctionArguments>...)> func,
        const std::source_location& location = std::source_location::current()) const
    {
        return registerFunctionImpl([&]() { return engine.registerFunction<R, FunctionArguments...>(func); }, location);
    }

private:
    template <typename Registration>
    auto registerFunctionImpl(Registration&& registration, const std::source_location& location) const
    {
        const auto registrationStart = std::chrono::steady_clock::now();
        try
        {
            auto registeredFunction = std::forward<Registration>(registration)();
            const auto registrationEnd = std::chrono::steady_clock::now();
            recordRegistrationEvent(location, std::chrono::duration_cast<std::chrono::nanoseconds>(registrationEnd - registrationStart));
            return registeredFunction;
        }
        catch (...)
        {
            const auto registrationEnd = std::chrono::steady_clock::now();
            recordRegistrationEvent(location, std::chrono::duration_cast<std::chrono::nanoseconds>(registrationEnd - registrationStart));
            throw;
        }
    }

    static void recordRegistrationEvent(const std::source_location& location, std::chrono::nanoseconds duration);
};

class PipelineCompilationContext final : public CompilationContext
{
public:
    PipelineCompilationContext(const nautilus::engine::NautilusEngine& engine, PipelineExecutionContext& pipelineExecutionContext)
        : CompilationContext(engine), pipelineExecutionContext(pipelineExecutionContext)
    {
    }

    [[nodiscard]] OperatorHandler* getOperatorHandler(OperatorHandlerId handlerId) const;

    template <typename OperatorHandlerType>
    [[nodiscard]] OperatorHandlerType* getOperatorHandlerAs(OperatorHandlerId handlerId) const
    {
        return dynamic_cast<OperatorHandlerType*>(getOperatorHandler(handlerId));
    }

private:
    PipelineExecutionContext& pipelineExecutionContext;
};
}
