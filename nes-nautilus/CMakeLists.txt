# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at

#    https://www.apache.org/licenses/LICENSE-2.0

# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

include_directories(tests/include)

# Set Sources and Headers
add_subdirectory(src)
get_source(nes-nautilus NES_NAUTILUS_SOURCE_FILES)

# Add Library
add_library(nes-nautilus ${NES_NAUTILUS_SOURCE_FILES})
target_link_libraries(nes-nautilus PUBLIC nes-memory)


if (NES_ENABLE_PRECOMPILED_HEADERS)
    target_precompile_headers(nes-nautilus REUSE_FROM nes-common)
    # We need to compile with -fPIC to include with nes-common compiled headers as it uses PIC
    target_compile_options(nes-nautilus PUBLIC "-fPIC")
endif ()

add_tests_if_enabled(tests)

# Add nes-nautilus to the include directories
target_include_directories(nes-nautilus PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include/nebulastream/>
)

# Link with the nautilus library
find_package(spdlog REQUIRED CONFIG)
find_package(MLIR REQUIRED CONFIG)

# Handle local nautilus build vs vcpkg
if (USE_LOCAL_NAUTILUS)
    # When using local nautilus, try to find it in a few common locations
    if (DEFINED ENV{NAUTILUS_DIR})
        # Try the install directory first if it exists
        if (EXISTS "$ENV{NAUTILUS_DIR}/install/lib64/cmake")
            list(APPEND CMAKE_PREFIX_PATH "$ENV{NAUTILUS_DIR}/install")
            message(STATUS "Looking for installed nautilus at: $ENV{NAUTILUS_DIR}/install")
        else()
            # Fall back to direct config path
            set(nautilus_DIR "$ENV{NAUTILUS_DIR}/build/nautilus/CMakeFiles/Export/06c502cb36a19255158111b9780c4eb3")
            message(STATUS "Looking for nautilus build at: $ENV{NAUTILUS_DIR}/build")
        endif()
    endif()
    # Also try the sibling directory (common development setup)
    if (EXISTS "${CMAKE_SOURCE_DIR}/../nautilus/install/lib64/cmake")
        list(APPEND CMAKE_PREFIX_PATH "${CMAKE_SOURCE_DIR}/../nautilus/install")
        message(STATUS "Using installed sibling nautilus")
    else()
        set(nautilus_DIR "${CMAKE_SOURCE_DIR}/../nautilus/build/nautilus/CMakeFiles/Export/06c502cb36a19255158111b9780c4eb3")
        message(STATUS "Using sibling nautilus build directory")
    endif()
endif()

find_package(nautilus REQUIRED CONFIG)

# We have to set the nes-nautilus to public to use the nautilus library in other modules
target_link_libraries(nes-nautilus PUBLIC nautilus::nautilus
        MLIRExecutionEngine
        MLIRFuncAllExtensions

        # Dialects
        MLIRSCFDialect
        MLIRFuncToLLVM
        MLIRSCFToControlFlow
)
