# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at

#    https://www.apache.org/licenses/LICENSE-2.0

# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.


add_subdirectory(TestUtils)

add_nes_unit_test(paged-vector-unit-tests "UnitTests/PagedVectorTest.cpp")
target_link_libraries(paged-vector-unit-tests nes-nautilus-test-util)

add_nes_unit_test(variable-sized-data-unit-tests "UnitTests/VariableSizedDataTest.cpp")
target_link_libraries(variable-sized-data-unit-tests nes-nautilus-test-util)

add_nes_unit_test(var-val-unit-tests "UnitTests/VarValTest.cpp")
target_link_libraries(var-val-unit-tests nes-nautilus-test-util)

add_nes_unit_test(chained-hashmap-unit-tests "UnitTests/ChainedHashMapTest.cpp")
target_link_libraries(chained-hashmap-unit-tests nes-nautilus-test-util)

add_nes_unit_test(chained-hashmap-unit-tests-custom-value "UnitTests/ChainedHashMapCustomValueTest.cpp")
target_link_libraries(chained-hashmap-unit-tests-custom-value nes-nautilus-test-util)

add_nes_unit_test(offset-hashmap-unit-tests "UnitTests/OffsetHashMapTest.cpp")
target_link_libraries(offset-hashmap-unit-tests nes-nautilus-test-util)

add_nes_unit_test(state-serialization-unit-tests "UnitTests/StateSerializationTest.cpp")
target_sources(state-serialization-unit-tests PRIVATE
    UnitTests/AggregationSerializableRecoveryTest.cpp
    UnitTests/JoinSerializableStateTest.cpp
    UnitTests/JoinSerializableRecoveryTest.cpp
    UnitTests/JoinSerializableNonEmptyRecoveryTest.cpp
)
target_link_libraries(state-serialization-unit-tests nes-nautilus-test-util nes-physical-operators)
target_include_directories(state-serialization-unit-tests PRIVATE
    ${CMAKE_SOURCE_DIR}/nes-physical-operators/include
    ${CMAKE_SOURCE_DIR}/nes-runtime/include
    ${CMAKE_SOURCE_DIR}/nes-executable/include)

add_nes_unit_test(poc-aggregation-unit-tests "UnitTests/POCAggregationTest.cpp")
target_link_libraries(poc-aggregation-unit-tests nes-nautilus-test-util)

# Phase 4 Week 1: Checkpoint manager round-trip test
add_nes_unit_test(checkpoint-manager-unit-tests "UnitTests/CheckpointManagerTest.cpp")
target_link_libraries(checkpoint-manager-unit-tests nes-nautilus-test-util)

# Phase 4 Week 2: Recovery Manager test (Aggregation round-trip)
add_nes_unit_test(recovery-manager-unit-tests "UnitTests/RecoveryManagerTest.cpp")
target_link_libraries(recovery-manager-unit-tests nes-nautilus-test-util nes-physical-operators)
target_include_directories(recovery-manager-unit-tests PRIVATE
    ${CMAKE_SOURCE_DIR}/nes-physical-operators/include
    ${CMAKE_SOURCE_DIR}/nes-runtime/include
    ${CMAKE_SOURCE_DIR}/nes-executable/include)

# Phase 4 Week 2: Join reconstruction test
add_nes_unit_test(join-recovery-unit-tests "UnitTests/JoinRecoveryTest.cpp")
target_link_libraries(join-recovery-unit-tests nes-nautilus-test-util nes-physical-operators)
target_include_directories(join-recovery-unit-tests PRIVATE
    ${CMAKE_SOURCE_DIR}/nes-physical-operators/include
    ${CMAKE_SOURCE_DIR}/nes-runtime/include
    ${CMAKE_SOURCE_DIR}/nes-executable/include)

# Phase 4: Aggregation recovery test
add_nes_unit_test(aggregation-recovery-unit-tests "UnitTests/AggregationRecoveryTest.cpp")
target_link_libraries(aggregation-recovery-unit-tests nes-nautilus-test-util nes-physical-operators)
target_include_directories(aggregation-recovery-unit-tests PRIVATE
    ${CMAKE_SOURCE_DIR}/nes-physical-operators/include
    ${CMAKE_SOURCE_DIR}/nes-runtime/include
    ${CMAKE_SOURCE_DIR}/nes-executable/include)

# Link reflect-cpp to tests that need it
if(DEFINED ENV{NIX_BUILD_TOP})
    find_package(reflectcpp REQUIRED)
    find_package(CapnProto REQUIRED)
    target_link_libraries(state-serialization-unit-tests reflectcpp::reflectcpp CapnProto::capnp)
    target_link_libraries(offset-hashmap-unit-tests reflectcpp::reflectcpp CapnProto::capnp)
endif()
