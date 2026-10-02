cmake_minimum_required(VERSION 3.20)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/CxetToolchain.cmake")
endif()
project(exchange_api_probe VERSION 3.0.0 LANGUAGES CXX)

include(GNUInstallDirs)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(OpenSSL REQUIRED)
find_package(Threads REQUIRED)
find_package(Boost REQUIRED COMPONENTS json)

add_library(exchange_probe_lib STATIC
    src/Application.cpp
    src/Cli.cpp
    src/Compare.cpp
    src/Contracts.cpp
    src/Credentials.cpp
    src/Gzip.cpp
    src/HttpClient.cpp
    src/Model.cpp
    src/NetCommon.cpp
    src/Output.cpp
    src/Placement.cpp
    src/Profiler.cpp
    src/Profiles.cpp
    src/ProfilesExtended.cpp
    src/ProfilesMajor.cpp
    src/ProfilesMisc.cpp
    src/ProbeRunner.cpp
    src/Redaction.cpp
    src/Research.cpp
    src/ResearchAnalysis.cpp
    src/ResearchCapture.cpp
    src/ResearchCommands.cpp
    src/ResearchProfile.cpp
    src/Sandbox.cpp
    src/ViewerServer.cpp
    src/WsClient.cpp
)

target_include_directories(exchange_probe_lib PUBLIC include)
target_compile_definitions(exchange_probe_lib PRIVATE
    EXCHANGE_PROBE_SOURCE_PROFILE_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/profiles"
    EXCHANGE_PROBE_INSTALL_PROFILE_ROOT="${CMAKE_INSTALL_FULL_DATADIR}/exchange-api-probe/profiles"
)
target_link_libraries(exchange_probe_lib
    PUBLIC
        OpenSSL::Crypto
        OpenSSL::SSL
        Boost::json
        Threads::Threads
)

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(exchange_probe_lib PRIVATE
        -Wall -Wextra -Wpedantic -Wconversion -Wshadow
        -Wno-missing-field-initializers
    )
endif()

add_executable(exchange-api-probe src/Main.cpp)
target_link_libraries(exchange-api-probe PRIVATE exchange_probe_lib)

install(TARGETS exchange-api-probe RUNTIME DESTINATION bin)
install(DIRECTORY profiles schemas DESTINATION share/exchange-api-probe)
install(FILES doc/ADAPTER_PROTOCOL.md
    DESTINATION share/doc/exchange-api-probe)

add_custom_target(exchange-probe-product DEPENDS exchange-api-probe)
add_custom_target(exchange-probe-all DEPENDS exchange-probe-product)
