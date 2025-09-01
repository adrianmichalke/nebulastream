# FollyAliases.cmake - Global folly aliases for NebulaStream
# This module creates global lowercase folly:: aliases that persist across the entire project
#
# Include this once at the top level to ensure folly:: targets are available everywhere

# Only run this once
if(FOLLY_ALIASES_CREATED)
    return()
endif()
set(FOLLY_ALIASES_CREATED TRUE)

# Ensure glog and Folly are found
find_package(glog CONFIG QUIET)
find_package(Folly CONFIG REQUIRED)

if(Folly_FOUND)
    # Create global lowercase aliases
    if(TARGET Folly::folly AND NOT TARGET folly::folly)
        add_library(folly::folly ALIAS Folly::folly)
        message(STATUS "✅ Created global alias: folly::folly -> Folly::folly")
    endif()
    
    if(TARGET Folly::folly_deps AND NOT TARGET folly::folly_deps)
        add_library(folly::folly_deps ALIAS Folly::folly_deps)
        message(STATUS "✅ Created global alias: folly::folly_deps -> Folly::folly_deps")
    endif()
    
    if(TARGET Folly::folly_test_util AND NOT TARGET folly::folly_test_util)
        add_library(folly::folly_test_util ALIAS Folly::folly_test_util)
        message(STATUS "✅ Created global alias: folly::folly_test_util -> Folly::folly_test_util")
    endif()
    
    if(TARGET Folly::follybenchmark AND NOT TARGET folly::follybenchmark)
        add_library(folly::follybenchmark ALIAS Folly::follybenchmark)
        message(STATUS "✅ Created global alias: folly::follybenchmark -> Folly::follybenchmark")
    endif()
    
    message(STATUS "📦 Folly aliases created successfully for NebulaStream compatibility")
    
    # Export to parent scope
    set(folly_FOUND TRUE PARENT_SCOPE)
    set(FOLLY_FOUND TRUE PARENT_SCOPE)
else()
    message(FATAL_ERROR "❌ Could not find Folly package - required for NebulaStream")
endif()