cmake_minimum_required(VERSION 3.5)

set(TARGET_NAME http_client)

set(EXTENSION_NAME ${TARGET_NAME}_extension)
set(LOADABLE_EXTENSION_NAME ${TARGET_NAME}_loadable_extension)

project(${TARGET_NAME})
include_directories(src/include)

# Requests go through DuckDB's HTTPUtil (httpfs for https:// and non-GET
# requests), so the extension links no HTTP or TLS library of its own.
set(EXTENSION_SOURCES src/http_client_extension.cpp src/query_farm_telemetry.cpp)

build_static_extension(${TARGET_NAME} ${EXTENSION_SOURCES})
build_loadable_extension(${TARGET_NAME} " " ${EXTENSION_SOURCES})

install(
  TARGETS ${EXTENSION_NAME}
  EXPORT "${DUCKDB_EXPORT_SET}"
  LIBRARY DESTINATION "${INSTALL_LIB_DIR}"
  ARCHIVE DESTINATION "${INSTALL_LIB_DIR}")
