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

#include <Sources/SourceProvider.hpp>

#include <memory>
#include <string>
#include <utility>
#include <Identifiers/Identifiers.hpp>
#include <Runtime/AbstractBufferProvider.hpp>
#include <Sources/SourceDescriptor.hpp>
#include <Sources/SourceHandle.hpp>
#include <Sources/BinaryStoreSource.hpp>
#include <ErrorHandling.hpp>
#include <SourceRegistry.hpp>

namespace NES
{

std::unique_ptr<SourceHandle> SourceProvider::lower(
    OriginId originId,
    const SourceDescriptor& sourceDescriptor,
    std::shared_ptr<AbstractPoolProvider> bufferPool,
    const int defaultNumberOfBuffersInLocalPool)
{
    /// Todo #241: Get the new source identfier from the source descriptor and pass it to SourceHandle.
    auto sourceArguments = SourceRegistryArguments(sourceDescriptor);
    if (auto source = SourceRegistry::instance().create(sourceDescriptor.getSourceType(), sourceArguments))
    {
        /// The source-specific configuration of numberOfBuffersInLocalPool takes priority.
        /// If not specified (-1), we take the NodeEngine-wide configuration.
        auto numberOfBuffersInLocalPool = (sourceDescriptor.getFromConfig(SourceDescriptor::NUMBER_OF_BUFFERS_IN_LOCAL_POOL) > 0)
            ? sourceDescriptor.getFromConfig(SourceDescriptor::NUMBER_OF_BUFFERS_IN_LOCAL_POOL)
            : defaultNumberOfBuffersInLocalPool;
        NES_DEBUG(
            "SourceProvider: lowering source type={} originId={} parserType={}",
            sourceDescriptor.getSourceType(),
            originId.getRawValue(),
            sourceDescriptor.getParserConfig().parserType);
        // Extra debug: verify instantiated source type for BinaryStore
        if (sourceDescriptor.getSourceType() == "BinaryStore")
        {
            auto* bs = dynamic_cast<BinaryStoreSource*>(source.value().get());
            NES_DEBUG("SourceProvider: created BinaryStoreSource instance? {}", bs ? "yes" : "no");
            if (!bs)
            {
                throw UnknownSourceType(
                    "Expected BinaryStoreSource instance for BinaryStore type, but got a different implementation");
            }
        }
        return std::make_unique<SourceHandle>(
            std::move(originId), std::move(bufferPool), numberOfBuffersInLocalPool, std::move(source.value()));
    }
    throw UnknownSourceType("unknown source descriptor type: {}", sourceDescriptor.getSourceType());
}

bool SourceProvider::contains(const std::string& sourceType)
{
    return SourceRegistry::instance().contains(sourceType);
}

}
