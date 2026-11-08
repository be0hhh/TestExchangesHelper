cmake_minimum_required(VERSION 3.20)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/CxetToolchain.cmake")
endif()
project(exchange_api_probe LANGUAGES CXX)

include(GNUInstallDirs)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(OpenSSL REQUIRED)
find_package(Threads REQUIRED)
find_package(Boost REQUIRED COMPONENTS json)
find_package(PkgConfig REQUIRED)
pkg_check_modules(NGHTTP2 REQUIRED IMPORTED_TARGET libnghttp2)

add_library(exchange_probe_lib STATIC
    src/Runtime/Application.cpp
    src/Runtime/Cli.cpp
    src/Contracts.cpp
    src/Network/Credentials.cpp
    src/Network/Gzip.cpp
    src/Network/HttpClient.cpp
    src/Model.cpp
    src/Network/NetCommon.cpp
    src/Network/TbankCredential.cpp
    src/Network/TbankNghttp2.cpp
    src/Presentation/Output.cpp
    src/Research/Placement.cpp
    src/Profiles/Profiles.cpp
    src/Profiles/ProfilesExtended.cpp
    src/Profiles/ProfilesMajor.cpp
    src/Profiles/ProfilesMisc.cpp
    src/Runtime/ProbeRunner.cpp
    src/Network/Redaction.cpp
    src/Research/Research.cpp
    src/Research/ResearchAnalysis.cpp
    src/Research/Analysis/Input.cpp
    src/Research/Analysis/Normalization.cpp
    src/Research/Analysis/Book.cpp
    src/Research/Analysis/Relations.cpp
    src/Research/ResearchCapture.cpp
    src/Research/ResearchRestCapture.cpp
    src/Research/ResearchCommands.cpp
    src/Research/ReferenceHttpFrequency.cpp
    src/Research/TbankGrpcLifecycle.cpp
    src/Research/TbankMarketSources.cpp
    src/Research/TbankGrpcProtocol.cpp
    src/Research/ResearchProfile.cpp
    src/Runtime/Sandbox.cpp
    src/Presentation/ViewerServer.cpp
    src/Network/WsClient.cpp
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
        PkgConfig::NGHTTP2
        Threads::Threads
)
if(TARGET hft_parser::reference_quote_runtime)
    target_compile_definitions(exchange_probe_lib PRIVATE EXCHANGE_PROBE_WITH_REFERENCE=1)
    target_link_libraries(exchange_probe_lib PRIVATE hft_parser::reference_quote_runtime)
endif()

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(exchange_probe_lib PRIVATE
        -Wall -Wextra -Wpedantic -Wconversion -Wshadow
        -Wno-missing-field-initializers
    )
endif()

add_executable(exchange-api-probe src/Runtime/Main.cpp)
target_link_libraries(exchange-api-probe PRIVATE exchange_probe_lib)
if(CXET_ROOT_SOURCE_GRAPH)
    target_sources(exchange-api-probe PRIVATE src/Network/TbankCxet.cpp)
    target_compile_definitions(exchange-api-probe PRIVATE CXET_EXCHANGE_PROBE_WITH_CXET=1)
    target_link_libraries(exchange-api-probe PRIVATE cxet::cxet_lib)
endif()

install(TARGETS exchange-api-probe RUNTIME DESTINATION bin)
install(DIRECTORY profiles schemas DESTINATION share/exchange-api-probe)
install(FILES doc/ADAPTER_PROTOCOL.md
    DESTINATION share/doc/exchange-api-probe)

add_custom_target(exchange-probe-product DEPENDS exchange-api-probe)
add_custom_target(exchange-probe-all DEPENDS exchange-probe-product)

if(CXET_LOCAL_TESTS)
    cxet_offline_instrument(exchange_probe_lib)
endif()
