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

#include <Nautilus/Interface/Formatting/BinaryRowEncoder.hpp>

#include <cstring>
#include <functional>

#include <Nautilus/Interface/Record.hpp>
#include <Nautilus/DataTypes/VariableSizedData.hpp>
#include <Nautilus/DataTypes/VarVal.hpp>
#include <Nautilus/Util.hpp>
#include <function.hpp>
#include <val.hpp>
#include <val_ptr.hpp>

namespace NES::Nautilus
{

BinaryRowEncoder::BinaryRowEncoder(const Schema& schema)
{
    fields.reserve(schema.getNumberOfFields());
    allFixed = true;
    fixedRowSize = 0;
    for (const auto& f : schema.getFields())
    {
        uint32_t fsz = 0;
        switch (f.dataType.type)
        {
            case DataType::Type::VARSIZED:
            case DataType::Type::VARSIZED_POINTER_REP:
                fsz = 0; // dynamic
                allFixed = false;
                break;
            default:
                fsz = f.dataType.getSizeInBytes();
                break;
        }
        fields.push_back(FieldInfo{f.name, f.dataType, fsz});
        fixedRowSize += fsz;
    }
}

nautilus::val<uint32_t> BinaryRowEncoder::computeSize(Record& record) const
{
    using namespace nautilus;
    // Fast path: all fields are fixed size
    if (allFixed)
    {
        return val<uint32_t>(fixedRowSize);
    }
    // First compute total size
    val<uint32_t> total = 0u;
    for (const auto& fi : fields)
    {
        if (fi.fixedSize > 0)
        {
            total = total + static_cast<uint32_t>(fi.fixedSize);
        }
        else
        {
            // Variable sized: add size(header+content) if present
            const auto vv = record.read(fi.name);
            auto sizeVar = vv.customVisit([](auto&& underlying) {
                using U = std::remove_cvref_t<decltype(underlying)>;
                if constexpr (std::is_same_v<U, VariableSizedData>)
                {
                    return underlying.getTotalSize();
                }
                else
                {
                    return nautilus::val<uint32_t>(0u);
                }
            });
            auto dynSize = sizeVar.cast<nautilus::val<uint32_t>>();
            total = total + dynSize;
        }
    }

    return total;
}

void BinaryRowEncoder::encodeTo(Record& record, const nautilus::val<int8_t*>& dst) const
{
    using namespace nautilus;
    // Fast path for fixed-width schemas: avoid tracing pointer-walk loops
    if (allFixed)
    {
        // Emit a straight-line write sequence using simple recursion to avoid host loops in the traced region.
        std::function<void(size_t, uint64_t)> emit = [&](size_t idx, uint64_t off) {
            if (idx >= fields.size()) { return; }
            const auto& fi = fields[idx];
            const VarVal& vv = record.read(fi.name);
            vv.writeToMemory(dst + val<uint64_t>(off));
            emit(idx + 1, off + static_cast<uint64_t>(fi.fixedSize));
        };
        emit(0, 0);
        return;
    }

    // General path (may contain var-sized fields)
    auto cur = dst;
    for (const auto& fi : fields)
    {
        const VarVal& vv = record.read(fi.name);
        vv.writeToMemory(cur);
        if (fi.fixedSize > 0)
        {
            cur = cur + static_cast<uint64_t>(fi.fixedSize);
        }
        else
        {
            auto contentLen = invoke(+[](int8_t* p) {
                uint32_t n = 0;
                std::memcpy(&n, p, sizeof(uint32_t));
                return n;
            }, cur);
            auto dynSize = contentLen + nautilus::val<uint32_t>(static_cast<uint32_t>(sizeof(uint32_t)));
            nautilus::val<uint64_t> dynSize64 = dynSize;
            cur = cur + dynSize64;
        }
    }
}

}
