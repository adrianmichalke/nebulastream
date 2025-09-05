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

#include <gtest/gtest.h>
#include <Nautilus/DataStructures/OffsetBasedHashMap.hpp>
#include <cstring>

using namespace NES::DataStructures;

class OffsetHashMapTest : public ::testing::Test {
protected:
    void SetUp() override {
        keySize = sizeof(int64_t);
        valueSize = sizeof(double);
        bucketCount = 16;
    }
    
    size_t keySize;
    size_t valueSize;
    size_t bucketCount;
};

TEST_F(OffsetHashMapTest, testBasicInsertAndFind) {
    OffsetBasedHashMap hashMap(keySize, valueSize, bucketCount);
    
    int64_t key = 42;
    double value = 3.14;
    uint64_t hash = std::hash<int64_t>{}(key);
    
    uint32_t offset = hashMap.insert(hash, &key, &value);
    EXPECT_GT(offset, 0);
    EXPECT_EQ(hashMap.size(), 1);
    
    auto* entry = hashMap.find(hash);
    ASSERT_NE(entry, nullptr);
    EXPECT_EQ(entry->hash, hash);
    
    int64_t* foundKey = reinterpret_cast<int64_t*>(hashMap.getKey(entry));
    double* foundValue = reinterpret_cast<double*>(hashMap.getValue(entry));
    
    EXPECT_EQ(*foundKey, key);
    EXPECT_DOUBLE_EQ(*foundValue, value);
}

TEST_F(OffsetHashMapTest, testMultipleInsertions) {
    OffsetBasedHashMap hashMap(keySize, valueSize, bucketCount);
    
    const int numEntries = 100;
    std::vector<int64_t> keys;
    std::vector<double> values;
    std::vector<uint64_t> hashes;
    
    for (int i = 0; i < numEntries; ++i) {
        keys.push_back(i * 10);
        values.push_back(i * 2.5);
        hashes.push_back(std::hash<int64_t>{}(keys.back()));
        
        hashMap.insert(hashes.back(), &keys.back(), &values.back());
    }
    
    EXPECT_EQ(hashMap.size(), numEntries);
    
    for (int i = 0; i < numEntries; ++i) {
        auto* entry = hashMap.find(hashes[i]);
        ASSERT_NE(entry, nullptr);
        
        int64_t* foundKey = reinterpret_cast<int64_t*>(hashMap.getKey(entry));
        double* foundValue = reinterpret_cast<double*>(hashMap.getValue(entry));
        
        EXPECT_EQ(*foundKey, keys[i]);
        EXPECT_DOUBLE_EQ(*foundValue, values[i]);
    }
}

TEST_F(OffsetHashMapTest, testCollisionHandling) {
    OffsetBasedHashMap hashMap(keySize, valueSize, bucketCount);
    
    uint64_t sameHash = 123456;
    
    int64_t key1 = 100, key2 = 200, key3 = 300;
    double value1 = 1.0, value2 = 2.0, value3 = 3.0;
    
    hashMap.insert(sameHash, &key1, &value1);
    hashMap.insert(sameHash, &key2, &value2);
    hashMap.insert(sameHash, &key3, &value3);
    
    EXPECT_EQ(hashMap.size(), 3);
    
    auto* entry = hashMap.find(sameHash);
    ASSERT_NE(entry, nullptr);
    
    std::set<int64_t> foundKeys;
    std::set<double> foundValues;
    
    while (entry != nullptr) {
        if (entry->hash == sameHash) {
            int64_t* key = reinterpret_cast<int64_t*>(hashMap.getKey(entry));
            double* value = reinterpret_cast<double*>(hashMap.getValue(entry));
            
            foundKeys.insert(*key);
            foundValues.insert(*value);
        }
        
        entry = (entry->nextOffset != 0) ? 
            reinterpret_cast<OffsetEntry*>(hashMap.extractState().memory.data() + entry->nextOffset) : 
            nullptr;
    }
    
    EXPECT_EQ(foundKeys.size(), 3);
    EXPECT_TRUE(foundKeys.count(key1));
    EXPECT_TRUE(foundKeys.count(key2));
    EXPECT_TRUE(foundKeys.count(key3));
}

TEST_F(OffsetHashMapTest, testStateExtraction) {
    OffsetBasedHashMap hashMap(keySize, valueSize, bucketCount);
    
    int64_t key = 999;
    double value = 99.9;
    uint64_t hash = std::hash<int64_t>{}(key);
    
    hashMap.insert(hash, &key, &value);
    
    const auto& state = hashMap.extractState();
    
    EXPECT_EQ(state.keySize, keySize);
    EXPECT_EQ(state.valueSize, valueSize);
    EXPECT_EQ(state.bucketCount, bucketCount);
    EXPECT_EQ(state.tupleCount, 1);
    EXPECT_GT(state.memory.size(), 0);
    EXPECT_EQ(state.chains.size(), bucketCount);
}

TEST_F(OffsetHashMapTest, testStateRestoration) {
    OffsetHashMapState savedState;
    uint64_t originalHash;
    
    {
        OffsetBasedHashMap originalHashMap(keySize, valueSize, bucketCount);
        
        int64_t key = 12345;
        double value = 67.89;
        originalHash = std::hash<int64_t>{}(key);
        
        originalHashMap.insert(originalHash, &key, &value);
        savedState = std::move(originalHashMap.extractState());
    }
    
    OffsetBasedHashMap restoredHashMap = OffsetBasedHashMap::fromState(savedState);
    
    EXPECT_EQ(restoredHashMap.size(), 1);
    
    auto* entry = restoredHashMap.find(originalHash);
    ASSERT_NE(entry, nullptr);
    
    int64_t* foundKey = reinterpret_cast<int64_t*>(restoredHashMap.getKey(entry));
    double* foundValue = reinterpret_cast<double*>(restoredHashMap.getValue(entry));
    
    EXPECT_EQ(*foundKey, 12345);
    EXPECT_DOUBLE_EQ(*foundValue, 67.89);
}

TEST_F(OffsetHashMapTest, testZeroCopyRestoration) {
    OffsetHashMapState state;
    state.keySize = keySize;
    state.valueSize = valueSize;
    state.bucketCount = bucketCount;
    state.entrySize = sizeof(OffsetEntry) + keySize + valueSize;
    state.tupleCount = 0;
    state.chains.resize(bucketCount, 0);
    
    OffsetBasedHashMap hashMap1(state);
    OffsetBasedHashMap hashMap2(state);
    
    int64_t key = 777;
    double value = 88.8;
    uint64_t hash = std::hash<int64_t>{}(key);
    
    hashMap1.insert(hash, &key, &value);
    
    EXPECT_EQ(hashMap1.size(), 1);
    EXPECT_EQ(hashMap2.size(), 1);
    
    auto* entry = hashMap2.find(hash);
    ASSERT_NE(entry, nullptr);
}

TEST_F(OffsetHashMapTest, testLargeDataset) {
    OffsetBasedHashMap hashMap(keySize, valueSize, 1024);
    
    const int numEntries = 10000;
    
    auto start = std::chrono::high_resolution_clock::now();
    
    for (int i = 0; i < numEntries; ++i) {
        int64_t key = i;
        double value = i * 1.5;
        uint64_t hash = std::hash<int64_t>{}(key);
        
        hashMap.insert(hash, &key, &value);
    }
    
    auto insertTime = std::chrono::high_resolution_clock::now() - start;
    
    EXPECT_EQ(hashMap.size(), numEntries);
    
    start = std::chrono::high_resolution_clock::now();
    
    for (int i = 0; i < numEntries; ++i) {
        int64_t key = i;
        uint64_t hash = std::hash<int64_t>{}(key);
        
        auto* entry = hashMap.find(hash);
        ASSERT_NE(entry, nullptr);
        
        int64_t* foundKey = reinterpret_cast<int64_t*>(hashMap.getKey(entry));
        EXPECT_EQ(*foundKey, key);
    }
    
    auto findTime = std::chrono::high_resolution_clock::now() - start;
    
    auto insertMs = std::chrono::duration_cast<std::chrono::milliseconds>(insertTime).count();
    auto findMs = std::chrono::duration_cast<std::chrono::milliseconds>(findTime).count();
    
    std::cout << "Inserted " << numEntries << " entries in " << insertMs << "ms" << std::endl;
    std::cout << "Found " << numEntries << " entries in " << findMs << "ms" << std::endl;
    std::cout << "Memory usage: " << hashMap.extractState().memory.size() << " bytes" << std::endl;
}