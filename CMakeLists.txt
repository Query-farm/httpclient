cmake_minimum_required(VERSION 3.5)

# Set extension name here
set(TARGET_NAME http_client)

# Make ZLIB support optional with default ON for desktop platforms and OFF for WASM
if(EMSCRIPTEN)
    option(USE_ZLIB "Enable ZLIB compression support" OFF)
else()
    option(USE_ZLIB "Enable ZLIB compression support" ON)
endif()

# Find OpenSSL before building extensions
find_package(OpenSSL REQUIRED)

set(EXTENSION_NAME ${TARGET_NAME}_extension)
set(LOADABLE_EXTENSION_NAME ${TARGET_NAME}_loadable_extension)

project(${TARGET_NAME})

include_directories(src/include duckdb/third_party/httplib)

set(EXTENSION_SOURCES src/http_client_extension.cpp
src/query_farm_telemetry.cpp)

if(MINGW)
    set(OPENSSL_USE_STATIC_LIBS TRUE)
endif()

# Common libraries needed for both targets
set(COMMON_LIBS
    duckdb_mbedtls
    ${OPENSSL_LIBRARIES}
)

# Handle ZLIB support
if(USE_ZLIB)
    find_package(ZLIB)
    if(ZLIB_FOUND)
        add_compile_definitions(CPPHTTPLIB_ZLIB_SUPPORT)
        list(APPEND COMMON_LIBS ZLIB::ZLIB)
        message(STATUS "Building with ZLIB support")
    else()
        message(STATUS "ZLIB not found, building without ZLIB support")
    endif()
endif()

# Windows-specific libraries
if(MINGW)
    set(WIN_LIBS crypt32 ws2_32 wsock32)
    list(APPEND COMMON_LIBS ${WIN_LIBS})
endif()

# Build extensions
build_static_extension(${TARGET_NAME} ${EXTENSION_SOURCES})
build_loadable_extension(${TARGET_NAME} " " ${EXTENSION_SOURCES})

# Include directories
include_directories(${OPENSSL_INCLUDE_DIR})

# Link libraries
target_link_libraries(${LOADABLE_EXTENSION_NAME} ${COMMON_LIBS})
target_link_libraries(${EXTENSION_NAME} ${COMMON_LIBS})

install(
    TARGETS ${EXTENSION_NAME}
    EXPORT "${DUCKDB_EXPORT_SET}"
    LIBRARY DESTINATION "${INSTALL_LIB_DIR}"
    ARCHIVE DESTINATION "${INSTALL_LIB_DIR}")
