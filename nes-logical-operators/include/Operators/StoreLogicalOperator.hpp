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

#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <Configurations/Descriptor.hpp>
#include <DataTypes/Schema.hpp>
#include <Identifiers/Identifiers.hpp>
#include <Operators/LogicalOperator.hpp>
#include <Traits/Trait.hpp>
#include <Util/PlanRenderer.hpp>

namespace NES
{

// Async backend selection for store operator
enum class StoreAsyncBackend : uint8_t
{
    POSIX = 0,
    IO_URING = 1
};

// Logical operator that persists rows to a binary file while passing them downstream unchanged.
struct StoreLogicalOperator final : LogicalOperatorConcept
{
    static constexpr std::string_view NAME = "Store";

    StoreLogicalOperator() = default;

    // Construct with already validated config
    explicit StoreLogicalOperator(DescriptorConfig::Config validatedConfig) : config(std::move(validatedConfig)) {}

    // Human readable
    [[nodiscard]] std::string explain(ExplainVerbosity verbosity) const override;
    [[nodiscard]] std::string_view getName() const noexcept override;

    // Children and traits
    [[nodiscard]] std::vector<LogicalOperator> getChildren() const override;
    [[nodiscard]] LogicalOperator withChildren(std::vector<LogicalOperator> children) const override;
    [[nodiscard]] LogicalOperator withTraitSet(TraitSet traitSet) const override;
    [[nodiscard]] TraitSet getTraitSet() const override;
    [[nodiscard]] bool operator==(const LogicalOperatorConcept& rhs) const override;

    // Schemas and origins
    [[nodiscard]] std::vector<Schema> getInputSchemas() const override;
    [[nodiscard]] Schema getOutputSchema() const override;
    [[nodiscard]] std::vector<std::vector<OriginId>> getInputOriginIds() const override;
    [[nodiscard]] std::vector<OriginId> getOutputOriginIds() const override;
    [[nodiscard]] LogicalOperator withInputOriginIds(std::vector<std::vector<OriginId>> ids) const override;
    [[nodiscard]] LogicalOperator withOutputOriginIds(std::vector<OriginId> ids) const override;
    [[nodiscard]] LogicalOperator withInferredSchema(std::vector<Schema> inputSchemas) const override;

    // Serialization
    void serialize(SerializableOperator& serializableOperator) const override;

    // Config access
    [[nodiscard]] const DescriptorConfig::Config& getConfig() const { return config; }
    [[nodiscard]] StoreLogicalOperator withConfig(DescriptorConfig::Config validatedConfig) const;

    // Configuration keys and validation
    struct ConfigParameters
    {
        // file path
        static inline const DescriptorConfig::ConfigParameter<std::string> FILE_PATH{
            "file_path",
            std::nullopt,
            [](const std::unordered_map<std::string, std::string>& cfg) { return DescriptorConfig::tryGet(FILE_PATH, cfg); }};

        static inline const DescriptorConfig::ConfigParameter<bool> APPEND{
            "append",
            true,
            [](const std::unordered_map<std::string, std::string>& cfg) { return DescriptorConfig::tryGet(APPEND, cfg); }};

        static inline const DescriptorConfig::ConfigParameter<bool> HEADER{
            "header",
            true,
            [](const std::unordered_map<std::string, std::string>& cfg) { return DescriptorConfig::tryGet(HEADER, cfg); }};

        static inline const DescriptorConfig::ConfigParameter<uint32_t> CHUNK_MIN_BYTES{
            "chunk_min_bytes",
            65536u,
            [](const std::unordered_map<std::string, std::string>& cfg) { return DescriptorConfig::tryGet(CHUNK_MIN_BYTES, cfg); }};

        static inline const DescriptorConfig::ConfigParameter<EnumWrapper, StoreAsyncBackend> ASYNC_BACKEND{
            "async_backend",
            EnumWrapper(std::string("posix")),
            [](const std::unordered_map<std::string, std::string>& cfg) { return DescriptorConfig::tryGet(ASYNC_BACKEND, cfg); }};

        static inline const DescriptorConfig::ConfigParameter<bool> DIRECT_IO{
            "direct_io",
            false,
            [](const std::unordered_map<std::string, std::string>& cfg) { return DescriptorConfig::tryGet(DIRECT_IO, cfg); }};

        static inline const DescriptorConfig::ConfigParameter<uint32_t> FDATASYNC_INTERVAL{
            "fdatasync_interval",
            0u,
            [](const std::unordered_map<std::string, std::string>& cfg) { return DescriptorConfig::tryGet(FDATASYNC_INTERVAL, cfg); }};

        static inline std::unordered_map<std::string, DescriptorConfig::ConfigParameterContainer> parameterMap
            = DescriptorConfig::createConfigParameterContainerMap(
                FILE_PATH, APPEND, HEADER, CHUNK_MIN_BYTES, ASYNC_BACKEND, DIRECT_IO, FDATASYNC_INTERVAL);
    };

    static DescriptorConfig::Config validateAndFormatConfig(std::unordered_map<std::string, std::string> configPairs);

private:
    std::vector<LogicalOperator> children;
    TraitSet traitSet;
    std::vector<OriginId> inputOriginIds;
    std::vector<OriginId> outputOriginIds;

    DescriptorConfig::Config config{};
};
}
