/*
    Licensed under the Apache License, Version 2.0 (the "License");
*/

#include <Sources/BinaryStoreSource.hpp>

#include <cerrno>
#include <cstring>
#include <fstream>
#include <stop_token>
#include <string>
#include <unordered_map>

#include <Configurations/Descriptor.hpp>
#include <DataTypes/Schema.hpp>
#include <MemoryLayout/RowLayout.hpp>
#include <Runtime/TupleBuffer.hpp>
#include <Sources/SourceDescriptor.hpp>
#include <ErrorHandling.hpp>
#include <SourceRegistry.hpp>
#include <SourceValidationRegistry.hpp>
#include <MemoryLayout/RowLayout.hpp>
#include <Util/Logger/Logger.hpp>

namespace NES
{

namespace
{
// Parse Store header and return data start offset; ignore schema
uint64_t parseHeader(std::ifstream& ifs)
{
    char magic[8];
    ifs.read(magic, 8);
    if (!ifs)
    {
        throw CannotOpenSink("Failed to read magic from file");
    }
    NES_DEBUG("BinaryStoreSource: magic read ok: {}{}{}{}{}{}{}{}",
              magic[0], magic[1], magic[2], magic[3], magic[4], magic[5], magic[6], magic[7]);
    // Read fixed header fields
    uint32_t version = 0;
    uint8_t endianness = 0;
    uint32_t flags = 0;
    uint64_t fingerprint = 0;
    uint32_t schemaLen = 0;
    ifs.read(reinterpret_cast<char*>(&version), sizeof(version));
    ifs.read(reinterpret_cast<char*>(&endianness), sizeof(endianness));
    ifs.read(reinterpret_cast<char*>(&flags), sizeof(flags));
    ifs.read(reinterpret_cast<char*>(&fingerprint), sizeof(fingerprint));
    ifs.read(reinterpret_cast<char*>(&schemaLen), sizeof(schemaLen));
    if (!ifs)
    {
        throw CannotOpenSink("Failed to read header fields");
    }
    // Skip schema bytes
    ifs.seekg(schemaLen, std::ios::cur);
    if (!ifs)
    {
        throw CannotOpenSink("Failed to skip schema bytes");
    }
    return static_cast<uint64_t>(8 + 4 + 1 + 4 + 8 + 4 + schemaLen);
}
}

BinaryStoreSource::BinaryStoreSource(const SourceDescriptor& sourceDescriptor)
    : filePath(sourceDescriptor.getConfig().contains("file_path")
                   ? std::get<std::string>(sourceDescriptor.getConfig().at("file_path"))
                   : std::string()),
      schema(*sourceDescriptor.getLogicalSource().getSchema())
{
}

void BinaryStoreSource::open()
{
    NES_DEBUG("BinaryStoreSource: opening {}", filePath);
    inputFile = std::ifstream(filePath, std::ios::binary);
    if (!inputFile)
    {
        throw CannotOpenSink("Could not open input file: {}: {}", filePath, std::strerror(errno));
    }
    // Parse header and set data start offset
    dataStartOffset = parseHeader(inputFile);
    NES_DEBUG("BinaryStoreSource: dataStartOffset={} currentPos={}", dataStartOffset, static_cast<long long>(inputFile.tellg()));
}

void BinaryStoreSource::close()
{
    inputFile.close();
}

size_t BinaryStoreSource::fillTupleBuffer(TupleBuffer& tupleBuffer, const std::stop_token&)
{
    const auto startPos = inputFile.tellg();
    NES_DEBUG("BinaryStoreSource: fill called at pos {}", static_cast<long long>(startPos));
    if (!inputFile) { return 0; }

    // Build RowLayout using current buffer size and the logical source schema
    RowLayout rowLayout(tupleBuffer.getBufferSize(), schema);
    const auto capacity = rowLayout.getCapacity();
    uint64_t tuplesWritten = 0;
    // Compute fixed row width once
    uint32_t rowWidthBytes = 0;
    for (size_t fieldIdx = 0; fieldIdx < schema.getNumberOfFields(); ++fieldIdx)
    {
        rowWidthBytes += static_cast<uint32_t>(rowLayout.getFieldSize(fieldIdx));
    }

    for (; tuplesWritten < capacity; ++tuplesWritten)
    {
        bool rowOk = true;
        for (size_t fieldIdx = 0; fieldIdx < schema.getNumberOfFields(); ++fieldIdx)
        {
            const auto physicalType = rowLayout.getPhysicalType(fieldIdx);
            const auto fieldSize = rowLayout.getFieldSize(fieldIdx);
            const auto offset = rowLayout.getFieldOffset(tuplesWritten, fieldIdx);
            char* dest = tupleBuffer.getBuffer<char>() + offset;

            if (physicalType.isType(DataType::Type::VARSIZED) || physicalType.isType(DataType::Type::VARSIZED_POINTER_REP))
            {
                // Expect [u32 length][bytes]; skip content, write zero child index into row
                uint32_t len = 0;
                inputFile.read(reinterpret_cast<char*>(&len), sizeof(uint32_t));
                if (!inputFile)
                {
                    rowOk = false;
                    break;
                }
                // Skip content bytes
                inputFile.seekg(len, std::ios::cur);
                if (!inputFile)
                {
                    rowOk = false;
                    break;
                }
                // Write zero as child buffer index/pointer
                std::memset(dest, 0, fieldSize);
            }
            else
            {
                inputFile.read(dest, static_cast<std::streamsize>(fieldSize));
                if (!inputFile)
                {
                    rowOk = false;
                    break;
                }
            }
        }
        if (!rowOk)
        {
            // Revert partial row writes by not counting this row
            break;
        }
    }

    // Calculate bytes read based on successfully written tuples to avoid tellg() failure after EOF
    const size_t bytesRead = static_cast<size_t>(tuplesWritten) * static_cast<size_t>(rowWidthBytes);
    tupleBuffer.setNumberOfTuples(tuplesWritten);

    // If we hit EOF trying to read further, clear failbit and seek to end of the successfully read region
    if (!inputFile)
    {
        const bool hitEof = inputFile.eof();
        inputFile.clear();
        if (startPos != -1)
        {
            inputFile.seekg(static_cast<std::streamoff>(startPos) + static_cast<std::streamoff>(bytesRead), std::ios::beg);
        }
        if (hitEof)
        {
            tupleBuffer.setLastChunk(true);
        }
    }
    else
    {
        // If stream is still good but we consumed all data available for now, mark last chunk when at physical EOF
        if (inputFile.peek() == std::char_traits<char>::eof())
        {
            tupleBuffer.setLastChunk(true);
        }
    }

    NES_DEBUG(
        "BinaryStoreSource: read {} bytes ({} tuples), eof={} fail={}", bytesRead, tuplesWritten, inputFile.eof(), inputFile.fail());
    totalNumBytesRead += bytesRead;
    return bytesRead;
}

uint32_t BinaryStoreSource::getRowWidthBytes() const
{
    uint32_t rowWidth = 0;
    RowLayout rl(/*bufferSize*/ 4096, schema);
    for (size_t i = 0; i < schema.getNumberOfFields(); ++i)
    {
        rowWidth += static_cast<uint32_t>(rl.getFieldSize(i));
    }
    return rowWidth;
}

DescriptorConfig::Config BinaryStoreSource::validateAndFormat(std::unordered_map<std::string, std::string> config)
{
    // Reuse SourceDescriptor::parameterMap and add FILE_PATH key convention
    // For now, require lowercase file_path
    if (config.find("file_path") == config.end())
    {
        throw InvalidConfigParameter("BinaryStoreSource requires 'file_path'");
    }
    DescriptorConfig::Config validated;
    validated.emplace("file_path", DescriptorConfig::ConfigType(config.at("file_path")));
    // Ensure common source parameter is present to avoid downstream lookup failures
    validated.emplace("number_of_buffers_in_local_pool", DescriptorConfig::ConfigType(static_cast<int64_t>(-1)));
    return validated;
}

std::ostream& BinaryStoreSource::toString(std::ostream& str) const
{
    str << fmt::format("BinaryStoreSource(filePath: {}, bytesRead: {})", filePath, totalNumBytesRead.load());
    return str;
}

SourceValidationRegistryReturnType RegisterBinaryStoreSourceValidation(SourceValidationRegistryArguments args)
{
    return BinaryStoreSource::validateAndFormat(std::move(args.config));
}

SourceRegistryReturnType SourceGeneratedRegistrar::RegisterBinaryStoreSource(SourceRegistryArguments args)
{
    return std::make_unique<BinaryStoreSource>(args.sourceDescriptor);
}

}
