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

#include <StatementHandler.hpp>

#include <algorithm>
#include <expected>
#include <memory>
#include <mutex>
#include <ranges>
#include <unordered_map>
#include <utility>
#include <vector>
#include <SQLQueryParser/StatementBinder.hpp>
#include <Sinks/SinkCatalog.hpp>
#include <ErrorHandling.hpp>

#include <Listeners/QueryLog.hpp>
#include <QueryManager/QueryManager.hpp>
#include <cpptrace/from_current.hpp>
#include <LegacyOptimizer.hpp>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/statvfs.h>
#include <unistd.h>
#include <cstring>
#include <string>
#include <vector>
#include <Nautilus/Interface/Formatting/BinaryRowEncoder.hpp>
#include <Nautilus/Interface/Record.hpp>
#include <Nautilus/DataTypes/VarVal.hpp>
#include <Nautilus/DataTypes/VariableSizedData.hpp>
#include <DataTypes/DataTypeProvider.hpp>

namespace NES
{

SourceStatementHandler::SourceStatementHandler(const std::shared_ptr<SourceCatalog>& sourceCatalog) : sourceCatalog(sourceCatalog)
{
}

std::expected<CreateLogicalSourceStatementResult, Exception>
SourceStatementHandler::operator()(const CreateLogicalSourceStatement& statement)
{
    if (const auto created = sourceCatalog->addLogicalSource(statement.name, statement.schema))
    {
        return CreateLogicalSourceStatementResult{created.value()};
    }
    return std::unexpected{SourceAlreadyExists(statement.name)};
}

std::expected<CreatePhysicalSourceStatementResult, Exception>
SourceStatementHandler::operator()(const CreatePhysicalSourceStatement& statement)
{
    if (const auto created
        = sourceCatalog->addPhysicalSource(statement.attachedTo, statement.sourceType, statement.sourceConfig, statement.parserConfig))
    {
        return CreatePhysicalSourceStatementResult{created.value()};
    }
    return std::unexpected{InvalidConfigParameter("Invalid configuration: {}", statement)};
}

std::expected<ShowLogicalSourcesStatementResult, Exception>
SourceStatementHandler::operator()(const ShowLogicalSourcesStatement& statement) const
{
    if (statement.name)
    {
        if (const auto foundSource = sourceCatalog->getLogicalSource(*statement.name))
        {
            return ShowLogicalSourcesStatementResult{std::vector{*foundSource}};
        }
        return ShowLogicalSourcesStatementResult{{}};
    }
    return ShowLogicalSourcesStatementResult{sourceCatalog->getAllLogicalSources() | std::ranges::to<std::vector>()};
}

std::expected<ShowPhysicalSourcesStatementResult, Exception>
SourceStatementHandler::operator()(const ShowPhysicalSourcesStatement& statement) const
{
    if (statement.id and not statement.logicalSource)
    {
        if (const auto foundSource = sourceCatalog->getPhysicalSource(PhysicalSourceId{statement.id.value()}))
        {
            return ShowPhysicalSourcesStatementResult{std::vector{*foundSource}};
        }
        return ShowPhysicalSourcesStatementResult{{}};
    }
    if (not statement.id and statement.logicalSource)
    {
        if (const auto foundSources = sourceCatalog->getPhysicalSources(*statement.logicalSource))
        {
            return ShowPhysicalSourcesStatementResult{*foundSources | std::ranges::to<std::vector>()};
        }
        return ShowPhysicalSourcesStatementResult{{}};
    }
    if (statement.logicalSource and statement.id)
    {
        if (const auto foundSources = sourceCatalog->getPhysicalSources(*statement.logicalSource))
        {
            return ShowPhysicalSourcesStatementResult{
                foundSources.value()
                | std::views::filter([statement](const auto& source)
                                     { return source.getPhysicalSourceId() == PhysicalSourceId{statement.id.value()}; })
                | std::ranges::to<std::vector>()};
        }
        return ShowPhysicalSourcesStatementResult{{}};
    }
    return ShowPhysicalSourcesStatementResult{
        sourceCatalog->getLogicalToPhysicalSourceMapping() | std::views::transform([](auto& pair) { return pair.second; })
        | std::views::join | std::ranges::to<std::vector>()};
}

std::expected<DropLogicalSourceStatementResult, Exception> SourceStatementHandler::operator()(const DropLogicalSourceStatement& statement)
{
    if (sourceCatalog->removeLogicalSource(statement.source))
    {
        return DropLogicalSourceStatementResult{statement.source};
    }
    return std::unexpected{UnknownSourceName(statement.source.getLogicalSourceName())};
}

std::expected<DropPhysicalSourceStatementResult, Exception> SourceStatementHandler::operator()(const DropPhysicalSourceStatement& statement)
{
    if (sourceCatalog->removePhysicalSource(statement.descriptor))
    {
        return DropPhysicalSourceStatementResult{statement.descriptor};
    }
    return std::unexpected{UnknownSourceName("Unknown physical source: {}", statement.descriptor)};
}

SinkStatementHandler::SinkStatementHandler(const std::shared_ptr<SinkCatalog>& sinkCatalog) : sinkCatalog(sinkCatalog)
{
}

std::expected<CreateSinkStatementResult, Exception> SinkStatementHandler::operator()(const CreateSinkStatement& statement)
{
    if (const auto created = sinkCatalog->addSinkDescriptor(statement.name, statement.schema, statement.sinkType, statement.sinkConfig))
    {
        return CreateSinkStatementResult{created.value()};
    }
    return std::unexpected{SinkAlreadyExists(statement.name)};
}

std::expected<ShowSinksStatementResult, Exception> SinkStatementHandler::operator()(const ShowSinksStatement& statement) const
{
    if (statement.name)
    {
        if (const auto foundSink = sinkCatalog->getSinkDescriptor(*statement.name))
        {
            return ShowSinksStatementResult{std::vector{*foundSink}};
        }
        return ShowSinksStatementResult{{}};
    }
    return ShowSinksStatementResult{sinkCatalog->getAllSinkDescriptors()};
}

std::expected<DropSinkStatementResult, Exception> SinkStatementHandler::operator()(const DropSinkStatement& statement)
{
    if (sinkCatalog->removeSinkDescriptor(statement.descriptor))
    {
        return DropSinkStatementResult{statement.descriptor};
    }
    return std::unexpected{UnknownSinkName(statement.descriptor.getSinkName())};
}

QueryStatementHandler::QueryStatementHandler(
    const std::shared_ptr<QueryManager>& queryManager, const std::shared_ptr<LegacyOptimizer>& optimizer)
    : queryManager(queryManager), optimizer(optimizer)
{
}

std::expected<DropQueryStatementResult, Exception> QueryStatementHandler::operator()(const DropQueryStatement& statement)
{
    const std::unique_lock lock(mutex);
    std::erase(runningQueries, statement.id);
    return queryManager->stop(statement.id)
        .and_then([&statement, this] { return queryManager->unregister(statement.id); })
        .transform([&statement] { return DropQueryStatementResult{statement.id}; });
}

std::expected<QueryStatementResult, Exception> QueryStatementHandler::operator()(const QueryStatement& statement)
{
    const std::unique_lock lock(mutex);
    CPPTRACE_TRY
    {
        const auto optimizedPlan = optimizer->optimize(statement);
        const auto id = queryManager->registerQuery(optimizedPlan);
        return id.and_then([this](const auto& queryId) { return queryManager->start(queryId); })
            .transform(
                [&id, this]
                {
                    runningQueries.push_back(id.value());
                    return QueryStatementResult{id.value()};
                });
    }
    CPPTRACE_CATCH(...)
    {
        return std::unexpected{wrapExternalException()};
    }
    std::unreachable();
}

std::expected<ShowQueriesStatementResult, Exception> QueryStatementHandler::operator()(const ShowQueriesStatement& statement)
{
    const std::unique_lock lock(mutex);
    if (not statement.id.has_value())
    {
        auto allQueryState = runningQueries | std::views::transform([this](auto queryId) { return queryManager->status(queryId); })
            | std::views::filter([](const std::expected<QuerySummary, Exception>& statusResult) { return statusResult.has_value(); })
            | std::views::transform([](const auto& statusResult) { return statusResult.value(); }) | std::ranges::to<std::vector>();
        auto newRunningQueries = allQueryState | std::views::transform([](const auto& querySummary) { return querySummary.queryId; })
            | std::ranges::to<std::vector>();
        runningQueries = newRunningQueries;
        return ShowQueriesStatementResult{
            allQueryState
            | std::views::transform([](const auto& querySummary) { return std::make_pair(querySummary.queryId, querySummary); })
            | std::ranges::to<std::unordered_map<QueryId, QuerySummary>>()};
    }
    if (const auto statusOpt = queryManager->status(statement.id.value()); statusOpt.has_value())
    {
        return ShowQueriesStatementResult{std::unordered_map<QueryId, QuerySummary>{{statement.id.value(), statusOpt.value()}}};
    }
    return ShowQueriesStatementResult{.queries = {}};
}

std::vector<QueryId> QueryStatementHandler::getRunningQueries() const
{
    return runningQueries;
}

}

namespace NES
{

static std::pair<Schema, std::string> parseSchemaFromOptions(const std::unordered_map<std::string, std::string>& opts)
{
    Schema s;
    auto it = opts.find("SCHEMA");
    if (it == opts.end())
    {
        throw InvalidConfigParameter("INSERT INTO STORE requires SCHEMA option specifying pairs of TYPE NAME");
    }
    std::stringstream ss(it->second);
    std::vector<std::string> toks;
    std::string tok;
    while (ss >> tok) toks.push_back(tok);
    if (toks.size() % 2 != 0)
    {
        throw InvalidConfigParameter("SCHEMA must be specified as pairs of TYPE NAME, got: {}", it->second);
    }
    for (size_t i = 0; i < toks.size(); i += 2)
    {
        auto typeStr = toks[i];
        auto nameStr = toks[i + 1];
        auto type = DataTypeProvider::tryProvideDataType(typeStr);
        if (!type.has_value())
        {
            throw InvalidConfigParameter("Unknown data type '{}' in SCHEMA", typeStr);
        }
        if (type->type == DataType::Type::VARSIZED || type->type == DataType::Type::VARSIZED_POINTER_REP)
        {
            throw InvalidConfigParameter("INSERT currently does not support VARSIZED fields in SCHEMA");
        }
        s.addField(nameStr, *type);
    }
    std::stringstream schemaText;
    schemaText << s;
    return {s, schemaText.str()};
}

static size_t detectIoAlign(const std::string& filePath)
{
    size_t align = 4096;
    long ps = ::sysconf(_SC_PAGESIZE);
    if (ps > 0) align = static_cast<size_t>(ps);
    struct statvfs vfs
    {
    };
    if (::statvfs(filePath.c_str(), &vfs) == 0 && vfs.f_bsize > 0)
    {
        align = std::max(align, static_cast<size_t>(vfs.f_bsize));
    }
    return align;
}

std::expected<InsertIntoStoreStatementResult, Exception>
InsertStatementHandler::operator()(const InsertIntoStoreStatement& stmt)
{
    try
    {
        // Required options
        auto findOpt = [&](std::string key) -> std::string
        {
            // options keys are stored upper-cased by the binder
            auto it = stmt.options.find(Util::toUpperCase(key));
            if (it == stmt.options.end())
            {
                throw InvalidConfigParameter("Missing required option '{}' for INSERT INTO STORE", key);
            }
            return it->second;
        };

        const std::string filePath = findOpt("FILE_PATH");
        const bool append = [&] {
            auto it = stmt.options.find("APPEND");
            return it != stmt.options.end() && (it->second == "true" || it->second == "TRUE" || it->second == "1");
        }();
        const bool header = [&] {
            auto it = stmt.options.find("HEADER");
            return it == stmt.options.end() || it->second == "true" || it->second == "TRUE" || it->second == "1";
        }();
        const bool directIO = [&] {
            auto it = stmt.options.find("DIRECT_IO");
            return it != stmt.options.end() && (it->second == "true" || it->second == "TRUE" || it->second == "1");
        }();
        const uint32_t fdatasyncInterval = [&] {
            auto it = stmt.options.find("FDATASYNC_INTERVAL");
            return it != stmt.options.end() ? static_cast<uint32_t>(std::stoul(it->second)) : 0u;
        }();
        const size_t chunkMinBytes = [&] {
            auto it = stmt.options.find("CHUNK_MIN_BYTES");
            return it != stmt.options.end() ? static_cast<size_t>(std::stoul(it->second)) : static_cast<size_t>(65536u);
        }();

        if (stmt.rowsAsText.empty())
        {
            return InsertIntoStoreStatementResult{.filePath = filePath, .rowsInserted = 0};
        }

        auto [schema, schemaText] = parseSchemaFromOptions(stmt.options);
        // Validate row width
        const size_t numFields = schema.getNumberOfFields();
        // Precompute fixed row size from schema (we disallow VARSIZED)
        uint32_t fixedRowSize = 0;
        for (const auto& f : schema.getFields()) fixedRowSize += static_cast<uint32_t>(f.dataType.getSizeInBytes());

        for (const auto& row : stmt.rowsAsText)
        {
            if (row.size() != numFields)
            {
                throw InvalidQuerySyntax("Row has {} values but schema has {} fields", row.size(), numFields);
            }
        }

        // Prepare encoder and build a single contiguous buffer for all rows
        Nautilus::BinaryRowEncoder encoder(schema);
        std::vector<uint8_t> payload;
        payload.reserve(std::max(chunkMinBytes, static_cast<size_t>(numFields * 16 * stmt.rowsAsText.size())));
        Nautilus::Record rec;
        // Build name order list
        std::vector<std::string> fieldNames;
        fieldNames.reserve(numFields);
        for (const auto& f : schema.getFields()) fieldNames.push_back(f.name);

        for (const auto& row : stmt.rowsAsText)
        {
            // Build record
            std::unordered_map<std::string, Nautilus::VarVal> fields;
            fields.reserve(numFields);
            for (size_t i = 0; i < numFields; ++i)
            {
                const auto& f = schema.getFields().at(i);
                const auto& vtxt = row[i];
                // Convert text to type and wrap in VarVal
                switch (f.dataType.type)
                {
                    case DataType::Type::BOOLEAN:
                        fields.emplace(f.name, Nautilus::VarVal(vtxt == "true" || vtxt == "TRUE" || vtxt == "1"));
                        break;
                    case DataType::Type::INT8:
                        fields.emplace(f.name, Nautilus::VarVal(static_cast<int8_t>(std::stoi(vtxt))));
                        break;
                    case DataType::Type::INT16:
                        fields.emplace(f.name, Nautilus::VarVal(static_cast<int16_t>(std::stoi(vtxt))));
                        break;
                    case DataType::Type::INT32:
                        fields.emplace(f.name, Nautilus::VarVal(static_cast<int32_t>(std::stol(vtxt))));
                        break;
                    case DataType::Type::INT64:
                        fields.emplace(f.name, Nautilus::VarVal(static_cast<int64_t>(std::stoll(vtxt))));
                        break;
                    case DataType::Type::UINT8:
                        fields.emplace(f.name, Nautilus::VarVal(static_cast<uint8_t>(std::stoul(vtxt))));
                        break;
                    case DataType::Type::UINT16:
                        fields.emplace(f.name, Nautilus::VarVal(static_cast<uint16_t>(std::stoul(vtxt))));
                        break;
                    case DataType::Type::UINT32:
                        fields.emplace(f.name, Nautilus::VarVal(static_cast<uint32_t>(std::stoul(vtxt))));
                        break;
                    case DataType::Type::UINT64:
                        fields.emplace(f.name, Nautilus::VarVal(static_cast<uint64_t>(std::stoull(vtxt))));
                        break;
                    case DataType::Type::FLOAT32:
                        fields.emplace(f.name, Nautilus::VarVal(static_cast<float>(std::stof(vtxt))));
                        break;
                    case DataType::Type::FLOAT64:
                        fields.emplace(f.name, Nautilus::VarVal(static_cast<double>(std::stod(vtxt))));
                        break;
                    default:
                        throw InvalidConfigParameter("Unsupported data type in INSERT row for field '{}'", f.name);
                }
            }
            rec = Nautilus::Record(std::move(fields));
            // Fixed-size row
            size_t oldSize = payload.size();
            payload.resize(oldSize + fixedRowSize);
            auto* base = reinterpret_cast<int8_t*>(payload.data() + oldSize);
            encoder.encodeTo(rec, base);
        }

        // Open file and write header if needed
        int flags = O_CREAT | O_WRONLY;
        if (!append) flags |= O_TRUNC;
#ifdef O_DIRECT
        if (directIO) flags |= O_DIRECT;
#endif
        int fd = ::open(filePath.c_str(), flags, 0644);
        if (fd < 0) return std::unexpected{CannotOpenSink("Could not open output file: {} (errno={}, msg={})", filePath, errno, std::strerror(errno))};
        struct stat st
        {
        };
        if (::fstat(fd, &st) != 0)
        {
            const int err = errno;
            ::close(fd);
            return std::unexpected{CannotOpenSink("fstat failed for {}: errno={}, msg={}", filePath, err, std::strerror(err))};
        }
        uint64_t tail = static_cast<uint64_t>(st.st_size);
        const size_t ioAlign = detectIoAlign(filePath);

        if (header && tail == 0)
        {
            // Build header: MAGIC + VERSION + ENDIANNESS + flags + fingerprint + schemaLen + schemaText
            static constexpr const char MAGIC[8] = {'N', 'E', 'S', 'S', 'T', 'O', 'R', 'E'};
            constexpr uint32_t VERSION = 1;
            constexpr uint8_t ENDIANNESS_LE = 1;
            auto fnv1a64 = [](const char* data, size_t len)
            {
                uint64_t hash = 1469598103934665603ull;
                for (size_t i = 0; i < len; ++i)
                {
                    hash ^= static_cast<uint8_t>(data[i]);
                    hash *= 1099511628211ull;
                }
                return hash;
            };
            const uint64_t fingerprint = fnv1a64(schemaText.c_str(), schemaText.size());
            const uint32_t schemaLen = static_cast<uint32_t>(schemaText.size());
            size_t headerSize = sizeof(MAGIC) + sizeof(uint32_t) + sizeof(uint8_t) + sizeof(uint32_t) + sizeof(uint64_t) + sizeof(uint32_t)
                + schemaLen;
            size_t paddedHeader = headerSize;
            if (directIO)
            {
                size_t rem = paddedHeader % ioAlign;
                if (rem != 0) paddedHeader += ioAlign - rem;
            }
            std::vector<uint8_t> hdr(paddedHeader, 0);
            size_t off = 0;
            std::memcpy(hdr.data() + off, MAGIC, sizeof(MAGIC)); off += sizeof(MAGIC);
            std::memcpy(hdr.data() + off, &VERSION, sizeof(uint32_t)); off += sizeof(uint32_t);
            std::memcpy(hdr.data() + off, &ENDIANNESS_LE, sizeof(uint8_t)); off += sizeof(uint8_t);
            uint32_t flags32 = 0; // reserved
            std::memcpy(hdr.data() + off, &flags32, sizeof(uint32_t)); off += sizeof(uint32_t);
            std::memcpy(hdr.data() + off, &fingerprint, sizeof(uint64_t)); off += sizeof(uint64_t);
            std::memcpy(hdr.data() + off, &schemaLen, sizeof(uint32_t)); off += sizeof(uint32_t);
            std::memcpy(hdr.data() + off, schemaText.data(), schemaLen); off += schemaLen;
            ssize_t written = ::pwrite(fd, hdr.data(), hdr.size(), 0);
            if (written < 0 || static_cast<size_t>(written) != hdr.size())
            {
                ::close(fd);
                return std::unexpected{CannotOpenSink("Writing store header failed: errno={} {}", errno, std::strerror(errno))};
            }
            tail += hdr.size();
        }

        // Align payload if directIO
        std::vector<uint8_t> alignedOut = payload;
        if (directIO)
        {
            size_t rem = alignedOut.size() % ioAlign;
            if (rem != 0) alignedOut.resize(alignedOut.size() + (ioAlign - rem), 0);
        }
        ssize_t written = ::pwrite(fd, alignedOut.data(), alignedOut.size(), static_cast<off_t>(tail));
        if (written < 0 || static_cast<size_t>(written) != alignedOut.size())
        {
            const int err = errno;
            ::close(fd);
            return std::unexpected{CannotOpenSink("Writing rows failed: errno={} {}", err, std::strerror(err))};
        }
        if (fdatasyncInterval > 0)
        {
            ::fdatasync(fd);
        }
        ::close(fd);
        return InsertIntoStoreStatementResult{.filePath = filePath, .rowsInserted = static_cast<uint64_t>(stmt.rowsAsText.size())};
    }
    catch (Exception& e)
    {
        return std::unexpected{e};
    }
    catch (const std::exception& e)
    {
        return std::unexpected{InvalidStatement(e.what())};
    }
}

}
