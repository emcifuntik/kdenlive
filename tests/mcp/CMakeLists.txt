# SPDX-FileCopyrightText: 2026 Kdenlive contributors
# SPDX-License-Identifier: BSD-2-Clause

# May be configured independently: cmake -S tests/mcp -B build-mcp
cmake_minimum_required(VERSION 3.16)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(KdenliveMcpTests LANGUAGES CXX)
    enable_testing()
endif()
find_package(Qt6 REQUIRED COMPONENTS Core Network Test)
set(CMAKE_AUTOMOC ON)
add_executable(mcptransporttest
    mcptransporttest.cpp
    ../../src/mcp/mcpserver.cpp
    ../../src/mcp/mcpserver.h
    ../../src/mcp/mcptoolregistry.cpp
    ../../src/mcp/mcptoolregistry.h
)
target_compile_features(mcptransporttest PRIVATE cxx_std_17)
target_link_libraries(mcptransporttest PRIVATE Qt6::Core Qt6::Network Qt6::Test)
target_include_directories(mcptransporttest PRIVATE ../../src)
add_test(NAME mcptransporttest COMMAND mcptransporttest)
