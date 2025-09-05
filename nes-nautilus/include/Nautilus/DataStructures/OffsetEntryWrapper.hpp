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
#include <Nautilus/DataStructures/OffsetBasedHashMap.hpp>
#include <Nautilus/Interface/HashMap/HashMap.hpp>

namespace NES::DataStructures {

class OffsetEntryWrapper : public Nautilus::Interface::AbstractHashMapEntry {
private:
    OffsetEntry* entry_;
    
public:
    explicit OffsetEntryWrapper(OffsetEntry* entry) : entry_(entry) {}
    
    ~OffsetEntryWrapper() override = default;
    
    OffsetEntry* getEntry() { return entry_; }
    const OffsetEntry* getEntry() const { return entry_; }
    
    uint64_t getHash() const { return entry_->hash; }
    uint32_t getNextOffset() const { return entry_->nextOffset; }
    void setNextOffset(uint32_t offset) { entry_->nextOffset = offset; }
    
    void* getKeyData() { return entry_->data; }
    const void* getKeyData() const { return entry_->data; }
    
    void* getValueData(size_t keySize) { return entry_->data + keySize; }
    const void* getValueData(size_t keySize) const { return entry_->data + keySize; }
};

}