# Findfolly.cmake - NebulaStream compatibility wrapper
# This module provides lowercase folly:: targets for compatibility across different environments
#
# - Nix environment: Provides Folly::folly, we create folly::folly alias
# - vcpkg environment: May provide different target names
# - System environment: Uses pkg-config fallback

# Check if aliases were already created (e.g., by main CMakeLists.txt in Nix environment)
if(TARGET folly::folly)
    # Aliases already exist, just mark as found
    set(folly_FOUND TRUE PARENT_SCOPE)
    set(FOLLY_FOUND TRUE PARENT_SCOPE)
    set(folly_FOUND TRUE)
    set(FOLLY_FOUND TRUE)
    message(STATUS "Found folly: Using existing target")
    return()
endif()

# Ensure glog is available for folly
find_package(glog CONFIG QUIET)

# Try to find Folly with CONFIG mode first (Nix, vcpkg environments)
find_package(Folly CONFIG QUIET)

if(Folly_FOUND)
    # Create lowercase aliases for NebulaStream compatibility
    if(TARGET Folly::folly AND NOT TARGET folly::folly)
        add_library(folly::folly ALIAS Folly::folly)
        message(STATUS "Created alias folly::folly -> Folly::folly")
    endif()
    
    if(TARGET Folly::folly_deps AND NOT TARGET folly::folly_deps)
        add_library(folly::folly_deps ALIAS Folly::folly_deps)
        message(STATUS "Created alias folly::folly_deps -> Folly::folly_deps")
    endif()
    
    if(TARGET Folly::folly_test_util AND NOT TARGET folly::folly_test_util)
        add_library(folly::folly_test_util ALIAS Folly::folly_test_util)
        message(STATUS "Created alias folly::folly_test_util -> Folly::folly_test_util")
    endif()
    
    if(TARGET Folly::follybenchmark AND NOT TARGET folly::follybenchmark)
        add_library(folly::follybenchmark ALIAS Folly::follybenchmark)
        message(STATUS "Created alias folly::follybenchmark -> Folly::follybenchmark")
    endif()
    
    # Set folly as found with appropriate variables
    set(folly_FOUND TRUE PARENT_SCOPE)
    set(folly_VERSION ${Folly_VERSION} PARENT_SCOPE)
    set(FOLLY_FOUND TRUE PARENT_SCOPE)
    set(FOLLY_VERSION ${Folly_VERSION} PARENT_SCOPE)
    
    # Also set them locally
    set(folly_FOUND TRUE)
    set(folly_VERSION ${Folly_VERSION})
    set(FOLLY_FOUND TRUE)
    set(FOLLY_VERSION ${Folly_VERSION})
    
    message(STATUS "Found folly: ${folly_VERSION}")
else()
    # Try pkg-config as fallback for system installations
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(FOLLY_PKG folly)
        if(FOLLY_PKG_FOUND)
            # Create imported target from pkg-config results
            add_library(folly::folly INTERFACE IMPORTED)
            target_link_libraries(folly::folly INTERFACE ${FOLLY_PKG_LIBRARIES})
            target_include_directories(folly::folly INTERFACE ${FOLLY_PKG_INCLUDE_DIRS})
            target_compile_options(folly::folly INTERFACE ${FOLLY_PKG_CFLAGS_OTHER})
            
            set(folly_FOUND TRUE PARENT_SCOPE)
            set(FOLLY_FOUND TRUE PARENT_SCOPE)
            set(folly_FOUND TRUE)
            set(FOLLY_FOUND TRUE)
            set(folly_VERSION ${FOLLY_PKG_VERSION} PARENT_SCOPE)
            set(FOLLY_VERSION ${FOLLY_PKG_VERSION} PARENT_SCOPE)
            
            message(STATUS "Found folly via pkg-config: ${FOLLY_PKG_VERSION}")
            return()
        endif()
    endif()
    
    # Neither CONFIG nor pkg-config found folly
    set(folly_FOUND FALSE PARENT_SCOPE)
    set(FOLLY_FOUND FALSE PARENT_SCOPE)
    set(folly_FOUND FALSE)
    set(FOLLY_FOUND FALSE)
    if(folly_FIND_REQUIRED)
        message(FATAL_ERROR "Could not find folly package via CONFIG or pkg-config")
    elseif(NOT folly_FIND_QUIETLY)
        message(STATUS "Could not find folly package")
    endif()
endif()