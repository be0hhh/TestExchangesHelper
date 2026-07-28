cmake_minimum_required(VERSION 3.20)

project(exchange_api_probe VERSION 3.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(OpenSSL REQUIRED)
find_package(Threads REQUIRED)
find_package(Boost REQUIRED COMPONENTS json)

add_library(exchange_probe_lib
    src/application.cpp
    src/audit.cpp
    src/cli.cpp
    src/compare.cpp
    src/contracts.cpp
    src/credentials.cpp
    src/gzip.cpp
    src/http_client.cpp
    src/model.cpp
    src/net_common.cpp
    src/output.cpp
    src/placement.cpp
    src/profiler.cpp
    src/profiles.cpp
    src/profiles_extended.cpp
    src/profiles_major.cpp
    src/profiles_misc.cpp
    src/probe_runner.cpp
    src/redaction.cpp
    src/sandbox.cpp
    src/ws_client.cpp
)

target_include_directories(exchange_probe_lib PUBLIC include)
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

add_executable(exchange-api-probe src/main.cpp)
target_link_libraries(exchange-api-probe PRIVATE exchange_probe_lib)

add_executable(binance-ws-race src/binance_ws_race.cpp)
target_link_libraries(binance-ws-race PRIVATE exchange_probe_lib)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(binance-ws-race PRIVATE
        -Wall -Wextra -Wpedantic -Wconversion -Wshadow
    )
endif()

include(CTest)
if(BUILD_TESTING)
    add_executable(exchange-probe-cli-tests tests/cli_tests.cpp)
    target_link_libraries(exchange-probe-cli-tests PRIVATE exchange_probe_lib)
    add_test(NAME exchange-probe-cli-tests COMMAND exchange-probe-cli-tests)

    add_executable(exchange-probe-rest-contract-tests tests/rest_contract_tests.cpp)
    target_link_libraries(exchange-probe-rest-contract-tests PRIVATE exchange_probe_lib)
    add_test(NAME exchange-probe-rest-contract-tests COMMAND exchange-probe-rest-contract-tests)

    add_executable(exchange-probe-ws-contract-tests tests/ws_contract_tests.cpp)
    target_link_libraries(exchange-probe-ws-contract-tests PRIVATE exchange_probe_lib)
    add_test(NAME exchange-probe-ws-contract-tests COMMAND exchange-probe-ws-contract-tests)

    add_executable(exchange-probe-redaction-tests tests/redaction_tests.cpp)
    target_link_libraries(exchange-probe-redaction-tests PRIVATE exchange_probe_lib)
    add_test(NAME exchange-probe-redaction-tests COMMAND exchange-probe-redaction-tests)

    add_executable(exchange-probe-gzip-tests tests/gzip_tests.cpp)
    target_link_libraries(exchange-probe-gzip-tests PRIVATE exchange_probe_lib)
    add_test(NAME exchange-probe-gzip-tests COMMAND exchange-probe-gzip-tests)

    add_executable(exchange-probe-audit-tests tests/audit_tests.cpp)
    target_link_libraries(exchange-probe-audit-tests PRIVATE exchange_probe_lib)
    add_test(NAME exchange-probe-audit-tests COMMAND exchange-probe-audit-tests)

    add_executable(exchange-probe-compare-tests tests/compare_tests.cpp)
    target_link_libraries(exchange-probe-compare-tests PRIVATE exchange_probe_lib)
    add_test(NAME exchange-probe-compare-tests COMMAND exchange-probe-compare-tests)

    add_test(NAME binance-ws-race-self-test COMMAND binance-ws-race --self-test)
endif()

install(TARGETS exchange-api-probe RUNTIME DESTINATION bin)
