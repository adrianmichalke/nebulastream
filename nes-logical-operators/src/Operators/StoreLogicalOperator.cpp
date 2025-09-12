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

#include <Operators/StoreLogicalOperator.hpp>

#include <algorithm>
#include <optional>
#include <ranges>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <Configurations/Descriptor.hpp>
#include <DataTypes/Schema.hpp>
#include <Identifiers/Identifiers.hpp>
#include <Operators/LogicalOperator.hpp>
#include <Traits/Trait.hpp>
#include <Util/PlanRenderer.hpp>
#include <fmt/format.h>
#include <ErrorHandling.hpp>
#include <SerializableOperator.pb.h>
#include <LogicalOperatorRegistry.hpp>
#include <Serialization/SchemaSerializationUtil.hpp>

namespace NES
{

std::string StoreLogicalOperator::explain(ExplainVerbosity verbosity) const
{
    if (verbosity == ExplainVerbosity::Debug)
    {
        std::stringstream cfg;
        Descriptor tmp{DescriptorConfig::Config(config)};
        cfg << &tmp;
        return fmt::format("STORE(opId: {}, config: {}, schema: {})", id, cfg.str(), getOutputSchema());
    }
    return std::string("STORE");
}

std::string_view StoreLogicalOperator::getName() const noexcept
{
    return NAME;
}

LogicalOperator StoreLogicalOperator::withTraitSet(TraitSet ts) const
{
    auto copy = *this;
    copy.traitSet = std::move(ts);
    return copy;
}

TraitSet StoreLogicalOperator::getTraitSet() const
{
    return traitSet;
}

LogicalOperator StoreLogicalOperator::withChildren(std::vector<LogicalOperator> newChildren) const
{
    auto copy = *this;
    copy.children = std::move(newChildren);
    return copy;
}

std::vector<LogicalOperator> StoreLogicalOperator::getChildren() const
{
    return children;
}

std::vector<Schema> StoreLogicalOperator::getInputSchemas() const
{
    INVARIANT(!children.empty(), "Store operator requires exactly one child");
    return children | std::ranges::views::transform([](const LogicalOperator& child) { return child.getOutputSchema(); })
        | std::ranges::to<std::vector>();
}

Schema StoreLogicalOperator::getOutputSchema() const
{
    INVARIANT(!children.empty(), "Store operator requires exactly one child");
    return children.front().getOutputSchema();
}

std::vector<std::vector<OriginId>> StoreLogicalOperator::getInputOriginIds() const
{
    return {inputOriginIds};
}

std::vector<OriginId> StoreLogicalOperator::getOutputOriginIds() const
{
    return outputOriginIds;
}

LogicalOperator StoreLogicalOperator::withInputOriginIds(std::vector<std::vector<OriginId>> ids) const
{
    auto copy = *this;
    copy.inputOriginIds = ids.at(0);
    return copy;
}

LogicalOperator StoreLogicalOperator::withOutputOriginIds(std::vector<OriginId> ids) const
{
    auto copy = *this;
    copy.outputOriginIds = std::move(ids);
    return copy;
}

LogicalOperator StoreLogicalOperator::withInferredSchema(std::vector<Schema> inputSchemas) const
{
    auto copy = *this;
    INVARIANT(!inputSchemas.empty(), "Store should have at least one input");
    const auto& first = inputSchemas.front();
    for (const auto& s : inputSchemas)
    {
        if (s != first)
        {
            throw CannotInferSchema("All input schemas must be equal for Store operator");
        }
    }
    return copy;
}

bool StoreLogicalOperator::operator==(const LogicalOperatorConcept& rhs) const
{
    if (const auto* other = dynamic_cast<const StoreLogicalOperator*>(&rhs))
    {
        return getOutputSchema() == other->getOutputSchema() && getInputSchemas() == other->getInputSchemas()
            && getInputOriginIds() == other->getInputOriginIds() && getOutputOriginIds() == other->getOutputOriginIds()
            && getTraitSet() == other->getTraitSet() && config == other->config;
    }
    return false;
}

void StoreLogicalOperator::serialize(SerializableOperator& serializableOperator) const
{
    // No dedicated Store message yet; store operator config in generic config map
    auto* cfgMap = serializableOperator.mutable_config();
    for (const auto& [k, v] : config)
    {
        (*cfgMap)[k] = descriptorConfigTypeToProto(v);
    }

    // Populate generic operator_ oneof with type, origins, and schemas
    auto* genOp = serializableOperator.mutable_operator_();
    genOp->set_operator_type(std::string(NAME));

    // Origins
    for (const auto& originList : getInputOriginIds())
    {
        auto* olist = genOp->add_input_origin_lists();
        for (auto originId : originList)
        {
            olist->add_origin_ids(originId.getRawValue());
        }
    }
    for (auto outId : getOutputOriginIds())
    {
        genOp->add_output_origin_ids(outId.getRawValue());
    }

    // Schemas
    for (const auto& inSchema : getInputSchemas())
    {
        SchemaSerializationUtil::serializeSchema(inSchema, genOp->add_input_schemas());
    }
    SchemaSerializationUtil::serializeSchema(getOutputSchema(), genOp->mutable_output_schema());

    serializableOperator.set_operator_id(id.getRawValue());
    for (auto& child : getChildren())
    {
        serializableOperator.add_children_ids(child.getId().getRawValue());
    }
    // Done: we use existing generic operator_ oneof; no Store-specific proto needed.
}

StoreLogicalOperator StoreLogicalOperator::withConfig(DescriptorConfig::Config validatedConfig) const
{
    auto copy = *this;
    copy.config = std::move(validatedConfig);
    return copy;
}

DescriptorConfig::Config StoreLogicalOperator::validateAndFormatConfig(std::unordered_map<std::string, std::string> configPairs)
{
    // Basic validation via DescriptorConfig
    auto cfg = DescriptorConfig::validateAndFormat<ConfigParameters>(std::move(configPairs), std::string(NAME));

    // Additional constraints from Phase 1 outcome
    const bool directIO = std::get<bool>(cfg.at(ConfigParameters::DIRECT_IO));
    if (directIO)
    {
        const auto chunkMin = std::get<uint32_t>(cfg.at(ConfigParameters::CHUNK_MIN_BYTES));
        if (chunkMin % 4096 != 0)
        {
            throw InvalidConfigParameter(
                fmt::format("For direct_io=true, chunk_min_bytes must be 4096-byte aligned, got {}", chunkMin));
        }
    }
    // Normalize async_backend to match enum names (accept case-insensitive user strings)
    if (auto it = cfg.find(std::string(ConfigParameters::ASYNC_BACKEND)); it != cfg.end())
    {
        if (std::holds_alternative<EnumWrapper>(it->second))
        {
            auto ew = std::get<EnumWrapper>(it->second);
            std::string v = ew.getValue();
            // uppercase and convert dashes to underscores for safety
            std::transform(v.begin(), v.end(), v.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
            for (auto& c : v)
            {
                if (c == '-')
                {
                    c = '_';
                }
            }
            // known aliases
            if (v == "POSIX" || v == "IO_URING")
            {
                it->second = EnumWrapper(v);
            }
            else
            {
                // fallback to POSIX
                it->second = EnumWrapper(std::string("POSIX"));
            }
        }
    }
    // async_backend validation beyond normalization is deferred to runtime build flags
    return cfg;
}

}

// Registrar for Store logical operator in the generated registry
namespace NES
{

LogicalOperatorRegistryReturnType
LogicalOperatorGeneratedRegistrar::RegisterStoreLogicalOperator(LogicalOperatorRegistryArguments arguments)
{
    // Construct with provided config (already typed)
    auto logicalOp = StoreLogicalOperator(arguments.config);
    if (auto& id = arguments.id)
    {
        logicalOp.id = *id;
    }
    return logicalOp.withInferredSchema(arguments.inputSchemas)
        .withInputOriginIds(arguments.inputOriginIds)
        .withOutputOriginIds(arguments.outputOriginIds);
}

}
