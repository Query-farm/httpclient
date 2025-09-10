#define DUCKDB_EXTENSION_MAIN
#include "http_client_extension.hpp"
#include "duckdb.hpp"
#include "duckdb/common/types.hpp"
#include "duckdb/common/vector_operations/generic_executor.hpp"
#include "duckdb/function/scalar_function.hpp"
#include "duckdb/common/atomic.hpp"
#include "duckdb/common/exception/http_exception.hpp"
#include <duckdb/parser/parsed_data/create_scalar_function_info.hpp>

#ifdef USE_ZLIB
#define CPPHTTPLIB_ZLIB_SUPPORT
#endif

#define CPPHTTPLIB_OPENSSL_SUPPORT
#include "httplib.hpp"

#include <string>
#include <sstream>

namespace duckdb {

// Helper function to parse URL and setup client
static std::pair<duckdb_httplib_openssl::Client, std::string> SetupHttpClient(const std::string &url) {
    std::string scheme, domain, path, client_url;
    size_t pos = url.find("://");
    std::string mod_url = url;
    if (pos != std::string::npos) {
        scheme = mod_url.substr(0, pos);
        mod_url.erase(0, pos + 3);
    }

    pos = mod_url.find("/");
    if (pos != std::string::npos) {
        domain = mod_url.substr(0, pos);
        path = mod_url.substr(pos);
    } else {
        domain = mod_url;
        path = "/";
    }

    // Construct client url with scheme if specified
    if (scheme.length() > 0) {
        client_url = scheme + "://" + domain;
    } else {
        client_url = domain;
    }

    // Create client and set a reasonable timeout (e.g., 10 seconds)
    duckdb_httplib_openssl::Client client(client_url);
    client.set_read_timeout(10, 0);  // 10 seconds
    client.set_follow_location(true); // Follow redirects

    return std::make_pair(std::move(client), path);
}

// Helper function to escape chars of a string representing a JSON object
std::string escape_json(const std::string &input) {
    std::ostringstream output;

    for (auto c = input.cbegin(); c != input.cend(); c++) {
        switch (*c) {
        case '"' : output << "\\\""; break;
        case '\\': output << "\\\\"; break;
        case '\b': output << "\\b"; break;
        case '\f': output << "\\f"; break;
        case '\n': output << "\\n"; break;
        case '\r': output << "\\r"; break;
        case '\t': output << "\\t"; break;
        default:
            if ('\x00' <= *c && *c <= '\x1f') {
                output << "\\u"
                       << std::hex << std::setw(4) << std::setfill('0') << static_cast<int>(*c);
            } else {
                output << *c;
            }
        }
    }
    return output.str();
}

// Helper function to create a Response object as a string
static std::string GetJsonResponse(int status, const std::string &reason, const std::string &body) {
    std::string response = StringUtil::Format(
        "{ \"status\": %i, \"reason\": \"%s\", \"body\": \"%s\" }",
        status,
        escape_json(reason),
        escape_json(body)
    );
    return response;
}

// Helper function to return the description of one HTTP error.
static std::string GetHttpErrorMessage(const duckdb_httplib_openssl::Result &res, const std::string &request_type) {
    std::string err_message = "HTTP " + request_type + " request failed. ";

    switch (res.error()) {
        case duckdb_httplib_openssl::Error::Connection:
            err_message += "Connection error.";
            break;
        case duckdb_httplib_openssl::Error::BindIPAddress:
            err_message += "Failed to bind IP address.";
            break;
        case duckdb_httplib_openssl::Error::Read:
            err_message += "Error reading response.";
            break;
        case duckdb_httplib_openssl::Error::Write:
            err_message += "Error writing request.";
            break;
        case duckdb_httplib_openssl::Error::ExceedRedirectCount:
            err_message += "Too many redirects.";
            break;
        case duckdb_httplib_openssl::Error::Canceled:
            err_message += "Request was canceled.";
            break;
        case duckdb_httplib_openssl::Error::SSLConnection:
            err_message += "SSL connection failed.";
            break;
        case duckdb_httplib_openssl::Error::SSLLoadingCerts:
            err_message += "Failed to load SSL certificates.";
            break;
        case duckdb_httplib_openssl::Error::SSLServerVerification:
            err_message += "SSL server verification failed.";
            break;
        case duckdb_httplib_openssl::Error::UnsupportedMultipartBoundaryChars:
            err_message += "Unsupported characters in multipart boundary.";
            break;
        case duckdb_httplib_openssl::Error::Compression:
            err_message += "Error during compression.";
            break;
        default:
            err_message += "Unknown error.";
            break;
    }
    return err_message;
}

// Helper function to convert list of entries to a map of parameters.
template <class T>
static int ConvertListEntryToMap(const list_entry_t& list_entry, const duckdb::Vector& input, T& result) {
    for (idx_t i = list_entry.offset; i < list_entry.offset + list_entry.length; i++) {
        const auto &child_value = input.GetValue(i);

        Vector tmp(child_value);
        auto &children = StructVector::GetEntries(tmp);

        if (children.size() == 2) {
            auto name = FlatVector::GetData<string_t>(*children[0]);
            auto data = FlatVector::GetData<string_t>(*children[1]);
            std::string key = name->GetString();
            std::string val = data->GetString();
            result.emplace(key, val);
        }
    }
    return result.size();
}



std::string headers_to_string(const duckdb_httplib_openssl::Headers& headers) {
    std::string result = "{";

    for (const auto& pair : headers) {
        const std::string& key = pair.first;
        const std::string& value = pair.second;

        std::string lower_key = key;
        std::transform(lower_key.begin(), lower_key.end(), lower_key.begin(),
                       [](unsigned char c){ return std::tolower(c); });

        result += "\"" + escape_json(lower_key) + "\":\"" + escape_json(value) + "\",";
    }

    if (result.length() > 1) {
        result.pop_back(); // Remove trailing comma
    }
    result += "}";

    return result;
}


static void HTTPHeadRequestFunction(DataChunk &args, ExpressionState &state, Vector &result) {
    D_ASSERT(args.data.size() == 1);

    UnaryExecutor::Execute<string_t, string_t>(args.data[0], result, args.size(), [&](string_t input) {
        std::string url = input.GetString();

        // Use helper to setup client and parse URL
        auto client_and_path = SetupHttpClient(url);
        auto &client = client_and_path.first;
        auto &path = client_and_path.second;

        // Make the HEAD request
        auto res = client.Head(path.c_str());
        if (res) {
            auto headers = headers_to_string(res->headers);
            std::string response = StringUtil::Format(
                "{ \"status\": %i, \"reason\": \"%s\", \"headers\": \"%s\" }",
                res->status,
                escape_json(res->reason),
                escape_json(headers)
            );
            return StringVector::AddString(result, response);
        } else {
            std::string response = StringUtil::Format(
                "{ \"status\": %i, \"reason\": \"%s\", \"headers\": \"%s\" }",
                -1, GetHttpErrorMessage(res, "HEAD"), ""
            );
            return StringVector::AddString(result, response);
        }
    });
}

static void HTTPGetRequestFunction(DataChunk &args, ExpressionState &state, Vector &result) {
    D_ASSERT(args.data.size() == 1);

    UnaryExecutor::Execute<string_t, string_t>(args.data[0], result, args.size(), [&](string_t input) {
        std::string url = input.GetString();

        // Use helper to setup client and parse URL
        auto client_and_path = SetupHttpClient(url);
        auto &client = client_and_path.first;
        auto &path = client_and_path.second;

        // Make the GET request
        auto res = client.Get(path.c_str());
        if (res) {
            std::string response = GetJsonResponse(res->status, res->reason, res->body);
            return StringVector::AddString(result, response);
        } else {
            std::string response = GetJsonResponse(-1, GetHttpErrorMessage(res, "GET"), "");
            return StringVector::AddString(result, response);
        }
    });
}

static void HTTPGetExRequestFunction(DataChunk &args, ExpressionState &state, Vector &result) {
    D_ASSERT(args.data.size() == 3);

    using STRING_TYPE = PrimitiveType<string_t>;
    using LENTRY_TYPE = PrimitiveType<list_entry_t>;

    auto &url_vector = args.data[0];
    auto &headers_vector = args.data[1];
    auto &headers_entry = ListVector::GetEntry(headers_vector);
    auto &params_vector = args.data[2];
    auto &params_entry = ListVector::GetEntry(params_vector);

    GenericExecutor::ExecuteTernary<STRING_TYPE, LENTRY_TYPE, LENTRY_TYPE, STRING_TYPE>(
        url_vector, headers_vector, params_vector, result, args.size(),
        [&](STRING_TYPE url, LENTRY_TYPE headers, LENTRY_TYPE params) {
            std::string url_str = url.val.GetString();

            // Use helper to setup client and parse URL
            auto client_and_path = SetupHttpClient(url_str);
            auto &client = client_and_path.first;
            auto &path = client_and_path.second;

            // Prepare headers
            duckdb_httplib_openssl::Headers header_map;
            auto header_list = headers.val;
            ConvertListEntryToMap<duckdb_httplib_openssl::Headers>(header_list, headers_entry, header_map);

            // Prepare params
            duckdb_httplib_openssl::Params param_map;
            auto params_list = params.val;
            ConvertListEntryToMap<duckdb_httplib_openssl::Params>(params_list, params_entry, param_map);

            // Make the POST request with headers and params
            auto res = client.Get(path.c_str(), param_map, header_map);
            if (res) {
                std::string response = GetJsonResponse(res->status, res->reason, res->body);
                return StringVector::AddString(result, response);
            } else {
                std::string response = GetJsonResponse(-1, GetHttpErrorMessage(res, "GET"), "");
                return StringVector::AddString(result, response);
            }
        });
}

static void HTTPPostRequestFunction(DataChunk &args, ExpressionState &state, Vector &result) {
    D_ASSERT(args.data.size() == 3);

    using STRING_TYPE = PrimitiveType<string_t>;
    using LENTRY_TYPE = PrimitiveType<list_entry_t>;

    auto &url_vector = args.data[0];
    auto &headers_vector = args.data[1];
    auto &headers_entry = ListVector::GetEntry(headers_vector);
    auto &body_vector = args.data[2];

    GenericExecutor::ExecuteTernary<STRING_TYPE, LENTRY_TYPE, STRING_TYPE, STRING_TYPE>(
        url_vector, headers_vector, body_vector, result, args.size(),
        [&](STRING_TYPE url, LENTRY_TYPE headers, STRING_TYPE body) {
            std::string url_str = url.val.GetString();

            // Use helper to setup client and parse URL
            auto client_and_path = SetupHttpClient(url_str);
            auto &client = client_and_path.first;
            auto &path = client_and_path.second;

            // Prepare headers
            duckdb_httplib_openssl::Headers header_map;
            auto header_list = headers.val;
            ConvertListEntryToMap<duckdb_httplib_openssl::Headers>(header_list, headers_entry, header_map);

            // Make the POST request with headers and body
            auto res = client.Post(path.c_str(), header_map, body.val.GetString(), "application/json");
            if (res) {
                std::string response = GetJsonResponse(res->status, res->reason, res->body);
                return StringVector::AddString(result, response);
            } else {
                std::string response = GetJsonResponse(-1, GetHttpErrorMessage(res, "POST"), "");
                return StringVector::AddString(result, response);
            }
        });
}

static void HTTPPostFormRequestFunction(DataChunk &args, ExpressionState &state, Vector &result) {
    D_ASSERT(args.data.size() == 3);

    using STRING_TYPE = PrimitiveType<string_t>;
    using LENTRY_TYPE = PrimitiveType<list_entry_t>;

    auto &url_vector = args.data[0];
    auto &headers_vector = args.data[1];
    auto &headers_entry = ListVector::GetEntry(headers_vector);
    auto &body_vector = args.data[2];
    auto &body_entry = ListVector::GetEntry(body_vector);

    GenericExecutor::ExecuteTernary<STRING_TYPE, LENTRY_TYPE, LENTRY_TYPE, STRING_TYPE>(
        url_vector, headers_vector, body_vector, result, args.size(),
        [&](STRING_TYPE url, LENTRY_TYPE headers, LENTRY_TYPE params) {
            std::string url_str = url.val.GetString();

            // Use helper to setup client and parse URL
            auto client_and_path = SetupHttpClient(url_str);
            auto &client = client_and_path.first;
            auto &path = client_and_path.second;

            // Prepare headers and parameters
            duckdb_httplib_openssl::Headers header_map;
            duckdb_httplib_openssl::Params params_map;
            ConvertListEntryToMap<duckdb_httplib_openssl::Headers>(headers.val, headers_entry, header_map);
            ConvertListEntryToMap<duckdb_httplib_openssl::Params>(params.val, body_entry, params_map);

            // Make the POST request with headers and params
            auto res = client.Post(path.c_str(), header_map, params_map);
            if (res) {
                std::string response = GetJsonResponse(res->status, res->reason, res->body);
                return StringVector::AddString(result, response);
            } else {
                std::string response = GetJsonResponse(-1, GetHttpErrorMessage(res, "POST"), "");
                return StringVector::AddString(result, response);
            }
        });
}


static void LoadInternal(ExtensionLoader &loader) {
    ScalarFunctionSet http_head("http_head");
    http_head.AddFunction(ScalarFunction({LogicalType::VARCHAR}, LogicalType::JSON(), HTTPHeadRequestFunction));
    loader.RegisterFunction(http_head);

    ScalarFunctionSet http_get("http_get");
    http_get.AddFunction(ScalarFunction({LogicalType::VARCHAR}, LogicalType::JSON(), HTTPGetRequestFunction));
    http_get.AddFunction(ScalarFunction(
        {LogicalType::VARCHAR, LogicalType::MAP(LogicalType::VARCHAR, LogicalType::VARCHAR),
        LogicalType::MAP(LogicalType::VARCHAR, LogicalType::VARCHAR)},
        LogicalType::JSON(), HTTPGetExRequestFunction));
    loader.RegisterFunction(http_get);

    ScalarFunctionSet http_post("http_post");
    http_post.AddFunction(ScalarFunction(
        {LogicalType::VARCHAR, LogicalType::MAP(LogicalType::VARCHAR, LogicalType::VARCHAR), LogicalType::JSON()},
        LogicalType::JSON(), HTTPPostRequestFunction));
    loader.RegisterFunction(http_post);

    ScalarFunctionSet http_post_form("http_post_form");
    http_post_form.AddFunction(ScalarFunction(
        {LogicalType::VARCHAR, LogicalType::MAP(LogicalType::VARCHAR, LogicalType::VARCHAR),
            LogicalType::MAP(LogicalType::VARCHAR, LogicalType::VARCHAR)},
        LogicalType::JSON(), HTTPPostFormRequestFunction));
    loader.RegisterFunction(http_post_form);
}

void HttpClientExtension::Load(ExtensionLoader &loader) {
    LoadInternal(loader);
}

std::string HttpClientExtension::Name() {
    return "http_client";
}

std::string HttpClientExtension::Version() const {
#ifdef EXT_VERSION_HTTPCLIENT
    return EXT_VERSION_HTTPCLIENT;
#else
    return "";
#endif
}


} // namespace duckdb

extern "C" {

DUCKDB_CPP_EXTENSION_ENTRY(http_client, loader) {
	duckdb::LoadInternal(loader);
}
}
