#pragma once

#include "duckdb.hpp"

namespace duckdb {

struct HTTPFunctions {
public:
	static void Register(ExtensionLoader &loader) {
		RegisterHTTPRequestFunction(loader);
	}

private:
	//! Register HTTPRequest functions
	static void RegisterHTTPRequestFunction(ExtensionLoader &loader);
};

} // namespace duckdb
