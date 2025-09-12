# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at

#    https://www.apache.org/licenses/LICENSE-2.0

# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

# include registrar generation functions
include(${PROJECT_SOURCE_DIR}/cmake/PluginRegistrationUtil.cmake)

# Set Sources and Headers
add_subdirectory(src)
get_source(nes-physical-operators NES_PHYSICAL_OPERATORS_SOURCE_FILES)

# Add Library
add_library(nes-physical-operators ${NES_PHYSICAL_OPERATORS_SOURCE_FILES})
target_link_libraries(nes-physical-operators PUBLIC nes-sources nes-sinks nes-nautilus)
target_include_directories(nes-physical-operators PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include/nebulastream/>)

# Optional io_uring backend toggle (Phase 2)
option(NES_WITH_IO_URING "Enable io_uring backend for Store operator" ON)
if(NES_WITH_IO_URING)
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(LIBURING liburing)
    endif()
    if(LIBURING_FOUND)
        target_compile_definitions(nes-physical-operators PUBLIC NES_WITH_IO_URING=1)
        target_include_directories(nes-physical-operators PUBLIC ${LIBURING_INCLUDE_DIRS})
        target_link_libraries(nes-physical-operators PUBLIC ${LIBURING_LIBRARIES})
        message(STATUS "io_uring enabled for Store operator")
    else()
        target_compile_definitions(nes-physical-operators PUBLIC NES_WITH_IO_URING=0)
        message(WARNING "liburing not found via pkg-config; io_uring backend disabled")
    endif()
else()
    target_compile_definitions(nes-physical-operators PUBLIC NES_WITH_IO_URING=0)
endif()

create_registries_for_component(PhysicalFunction AggregationPhysicalFunction)

add_tests_if_enabled(tests)
