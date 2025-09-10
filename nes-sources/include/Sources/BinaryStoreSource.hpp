/*
    Licensed under the Apache License, Version 2.0 (the "License");
*/
#pragma once

#include <fstream>
#include <string>
#include <Sources/Source.hpp>
#include <DataTypes/Schema.hpp>
#include <SourceRegistry.hpp>
#include <SourceValidationRegistry.hpp>

namespace NES
{

// Reads rows produced by Store from a binary file and fills TupleBuffers in row layout.
class BinaryStoreSource final : public Source
{
public:
    static constexpr std::string_view NAME = "BinaryStore";
    explicit BinaryStoreSource(const SourceDescriptor& sourceDescriptor);
    ~BinaryStoreSource() override = default;

    void open() override;
    void close() override;
    size_t fillTupleBuffer(TupleBuffer& tupleBuffer, const std::stop_token& stopToken) override;
    // Row width in bytes for fixed-sized schema
    [[nodiscard]] uint32_t getRowWidthBytes() const;

    // Validation
    static DescriptorConfig::Config validateAndFormat(std::unordered_map<std::string, std::string> config);

protected:
    std::ostream& toString(std::ostream& str) const override;

private:
    std::string filePath;
    std::ifstream inputFile;
    uint64_t dataStartOffset{0};
    std::atomic<uint64_t> totalNumBytesRead{0};
    Schema schema;
};

// Registry hooks
SourceValidationRegistryReturnType RegisterBinaryStoreSourceValidation(SourceValidationRegistryArguments args);

}
