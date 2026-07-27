cmake_minimum_required(VERSION 3.20)

project(exchange_api_probe VERSION 2.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(OpenSSL REQUIRED)
find_package(Threads REQUIRED)

add_library(exchange_probe_lib
    src/application.cpp
    src/audit.cpp
    src/cli.cpp
    src/contracts.cpp
    src/credentials.cpp
    src/gzip.cpp
    src/http_client.cpp
    src/model.cpp
    src/net_common.cpp
    src/output.cpp
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
endif()

install(TARGETS exchange-api-probe RUNTIME DESTINATION bin)
