#define DUCKDB_EXTENSION_MAIN
#include "http_client_extension.hpp"
#include "duckdb.hpp"
#include "duckdb/common/types.hpp"
#include "duckdb/common/vector_operations/generic_executor.hpp"
#include "duckdb/function/scalar_function.hpp"
#include "duckdb/common/atomic.hpp"
#include "duckdb/common/exception/http_exception.hpp"
#include <duckdb/parser/parsed_data/create_scalar_function_info.hpp>
#include "query_farm_telemetry.hpp"
#include "duckdb/main/secret/secret.hpp"
#include "duckdb/main/secret/secret_manager.hpp"
#include "duckdb/catalog/catalog_transaction.hpp"
#include "duckdb/common/types/blob.hpp"
#include "duckdb/common/http_util.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/main/settings.hpp"
#include "duckdb/main/config.hpp"
#ifdef USE_ZLIB
#define CPPHTTPLIB_ZLIB_SUPPORT
#endif

#define CPPHTTPLIB_OPENSSL_SUPPORT
#include "httplib.hpp"

#include <string>
#include <sstream>
#include <algorithm>
#include <iomanip>
#include <thread>
#include <chrono>
#include <cmath>
#include <mutex>
#include <condition_variable>

namespace duckdb
{

    struct ProxyConfig
    {
        string host;
        idx_t port = 80;
        string username;
        string password;

        bool IsSet() const
        {
            return !host.empty();
        }
    };

    static bool ParseProxySpec(const string &spec, ProxyConfig &out)
    {
        if (spec.empty())
        {
            return false;
        }
        auto sanitized = spec;
        if (StringUtil::StartsWith(sanitized, "http://"))
        {
            sanitized = sanitized.substr(7);
        }
        else if (StringUtil::StartsWith(sanitized, "https://"))
        {
            sanitized = sanitized.substr(8);
        }
        auto at = sanitized.find('@');
        if (at != string::npos)
        {
            auto userinfo = sanitized.substr(0, at);
            sanitized = sanitized.substr(at + 1);
            auto colon = userinfo.find(':');
            if (colon == string::npos)
            {
                out.username = userinfo;
            }
            else
            {
                out.username = userinfo.substr(0, colon);
                out.password = userinfo.substr(colon + 1);
            }
        }
        HTTPUtil::ParseHTTPProxyHost(sanitized, out.host, out.port);
        return out.IsSet();
    }

    static ProxyConfig ResolveProxyConfig(ExpressionState &state, const string &url)
    {
        ProxyConfig cfg;
        if (!state.HasContext())
        {
            return cfg;
        }
        auto &context = state.GetContext();
        auto &secret_manager = SecretManager::Get(context);
        auto transaction = CatalogTransaction::GetSystemCatalogTransaction(context);
        auto match = secret_manager.LookupSecret(transaction, url, "http");
        if (match.HasMatch())
        {
            auto &kv = dynamic_cast<const KeyValueSecret &>(match.GetSecret());
            Value proxy;
            if (kv.TryGetValue("http_proxy", proxy) && !proxy.IsNull() && !proxy.ToString().empty())
            {
                ParseProxySpec(proxy.ToString(), cfg);
            }
            Value user;
            if (kv.TryGetValue("http_proxy_username", user) && !user.IsNull() && cfg.username.empty())
            {
                cfg.username = user.ToString();
            }
            Value pass;
            if (kv.TryGetValue("http_proxy_password", pass) && !pass.IsNull() && cfg.password.empty())
            {
                cfg.password = pass.ToString();
            }
            if (cfg.IsSet())
            {
                return cfg;
            }
        }

        auto &db = DatabaseInstance::GetDatabase(context);
        if (!db.config.options.http_proxy.empty())
        {
            ParseProxySpec(db.config.options.http_proxy, cfg);
            if (cfg.username.empty())
            {
                cfg.username = Settings::Get<HTTPProxyUsernameSetting>(db);
            }
            if (cfg.password.empty())
            {
                cfg.password = Settings::Get<HTTPProxyPasswordSetting>(db);
            }
        }
        return cfg;
    }

    static void ApplyProxy(duckdb_httplib_openssl::Client &client, ExpressionState &state, const string &url)
    {
        auto cfg = ResolveProxyConfig(state, url);
        if (!cfg.IsSet())
        {
            return;
        }
        client.set_proxy(cfg.host, NumericCast<int>(cfg.port));
        if (!cfg.username.empty())
        {
            client.set_proxy_basic_auth(cfg.username, cfg.password);
        }
    }

    // Helper function to parse URL and setup client
    static std::pair<duckdb_httplib_openssl::Client, std::string> SetupHttpClient(const std::string &url,
                                                                                 ExpressionState &state)
    {
        std::string scheme, domain, path, client_url;
        size_t pos = url.find("://");
        std::string mod_url = url;
        if (pos != std::string::npos)
        {
            scheme = mod_url.substr(0, pos);
            mod_url.erase(0, pos + 3);
        }

        pos = mod_url.find("/");
        if (pos != std::string::npos)
        {
            domain = mod_url.substr(0, pos);
            path = mod_url.substr(pos);
        }
        else
        {
            domain = mod_url;
            path = "/";
        }

        // Construct client url with scheme if specified
        if (scheme.length() > 0)
        {
            client_url = scheme + "://" + domain;
        }
        else
        {
            client_url = domain;
        }

        // Create client and set a reasonable timeout (e.g., 10 seconds)
        duckdb_httplib_openssl::Client client(client_url);
        client.set_connection_timeout(5, 0);
        client.set_read_timeout(10, 0);   // 10 seconds
        client.set_follow_location(true); // Follow redirects
        ApplyProxy(client, state, url);

        return std::make_pair(std::move(client), path);
    }

    struct RetryConfig
    {
        uint64_t retries = 0;
        uint64_t wait_ms = 100;
        double backoff = 4.0;
    };

    static uint64_t SettingUBigint(ClientContext &context, const char *name, uint64_t fallback)
    {
        Value v;
        if (context.TryGetCurrentSetting(name, v) && !v.IsNull())
        {
            return v.GetValue<uint64_t>();
        }
        return fallback;
    }

    struct ParallelGate
    {
        std::mutex mu;
        std::condition_variable cv;
        uint64_t in_flight = 0;
    };

    static ParallelGate &GetParallelGate()
    {
        static ParallelGate gate;
        return gate;
    }

    // Caps in-flight HTTP calls across DuckDB worker threads. 0 = unlimited (default).
    struct ParallelSlot
    {
        ParallelSlot() = delete;
        explicit ParallelSlot(ExpressionState &state)
        {
            uint64_t max_parallel = 0;
            if (state.HasContext())
            {
                max_parallel = SettingUBigint(state.GetContext(), "http_client_max_parallel", 0);
            }
            if (max_parallel == 0)
            {
                return;
            }
            auto &gate = GetParallelGate();
            mu = &gate.mu;
            cv = &gate.cv;
            in_flight = &gate.in_flight;
            std::unique_lock<std::mutex> lock(*mu);
            cv->wait(lock, [&]() { return *in_flight < max_parallel; });
            (*in_flight)++;
            held = true;
        }
        ~ParallelSlot()
        {
            if (!held)
            {
                return;
            }
            std::lock_guard<std::mutex> lock(*mu);
            (*in_flight)--;
            cv->notify_all();
        }
        ParallelSlot(const ParallelSlot &) = delete;
        ParallelSlot &operator=(const ParallelSlot &) = delete;

    private:
        std::mutex *mu = nullptr;
        std::condition_variable *cv = nullptr;
        uint64_t *in_flight = nullptr;
        bool held = false;
    };

    static RetryConfig GetRetryConfig(ExpressionState &state)
    {
        RetryConfig cfg;
        if (!state.HasContext())
        {
            return cfg;
        }
        auto &context = state.GetContext();
        cfg.retries = SettingUBigint(context, "http_client_retries", 0);
        cfg.wait_ms = SettingUBigint(context, "http_client_retry_wait_ms", 100);
        Value backoff;
        if (context.TryGetCurrentSetting("http_client_retry_backoff", backoff) && !backoff.IsNull())
        {
            cfg.backoff = backoff.GetValue<double>();
        }
        return cfg;
    }

    static bool ShouldRetryRequest(const std::string &method, const duckdb_httplib_openssl::Result &res)
    {
        if (!res)
        {
            return true;
        }
        const auto status = res->status;
        if (status == 401 || status == 403)
        {
            return false;
        }
        if (status == 408 || status == 429 || status == 502 || status == 503 || status == 504)
        {
            return true;
        }
        if (status == 500 && (method == "GET" || method == "HEAD"))
        {
            return true;
        }
        return false;
    }

    static uint64_t RetryDelayMs(const RetryConfig &cfg, uint64_t attempt,
                                 const duckdb_httplib_openssl::Result &res)
    {
        double wait = static_cast<double>(cfg.wait_ms) * std::pow(cfg.backoff, static_cast<double>(attempt));
        if (res && res->status == 429)
        {
            auto it = res->headers.find("Retry-After");
            if (it != res->headers.end())
            {
                try
                {
                    wait = std::max(wait, std::stod(it->second) * 1000.0);
                }
                catch (...)
                {
                }
            }
            else
            {
                wait = std::max(wait, 1000.0);
            }
        }
        if (wait < 0)
        {
            return 0;
        }
        return static_cast<uint64_t>(wait);
    }

    template <class FN>
    static duckdb_httplib_openssl::Result ExecuteWithRetry(ExpressionState &state, const std::string &method, FN &&fn)
    {
        ParallelSlot slot(state);
        auto cfg = GetRetryConfig(state);
        duckdb_httplib_openssl::Result res;
        for (uint64_t attempt = 0; attempt <= cfg.retries; attempt++)
        {
            res = fn();
            if (attempt == cfg.retries || !ShouldRetryRequest(method, res))
            {
                return res;
            }
            auto delay = RetryDelayMs(cfg, attempt, res);
            if (delay > 0)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(delay));
            }
        }
        return res;
    }

    static ScalarFunction HttpFn(vector<LogicalType> arguments, LogicalType return_type, scalar_function_t fn)
    {
        ScalarFunction function(std::move(arguments), std::move(return_type), std::move(fn));
        function.SetVolatile();
        return function;
    }

    static ScalarFunction HttpFn(vector<LogicalType> arguments, scalar_function_t fn)
    {
        return HttpFn(std::move(arguments), LogicalType::JSON(), std::move(fn));
    }

    static LogicalType MultipartFileType()
    {
        child_list_t<LogicalType> fields;
        fields.emplace_back("name", LogicalType::VARCHAR);
        fields.emplace_back("content", LogicalType::BLOB);
        fields.emplace_back("filename", LogicalType::VARCHAR);
        fields.emplace_back("content_type", LogicalType::VARCHAR);
        return LogicalType::STRUCT(std::move(fields));
    }

    static std::string ResolveContentType(const duckdb_httplib_openssl::Headers &headers,
                                          const std::string &default_type)
    {
        auto it = headers.find("Content-Type");
        if (it != headers.end() && !it->second.empty())
        {
            return it->second;
        }
        return default_type;
    }

    std::string escape_json(const std::string &input);

    static void ApplyHttpSecrets(ExpressionState &state, const std::string &url,
                                 duckdb_httplib_openssl::Headers &headers)
    {
        if (!state.HasContext())
        {
            return;
        }
        auto &context = state.GetContext();
        auto &secret_manager = SecretManager::Get(context);
        auto transaction = CatalogTransaction::GetSystemCatalogTransaction(context);
        auto match = secret_manager.LookupSecret(transaction, url, "http");
        if (!match.HasMatch())
        {
            return;
        }

        auto &kv = dynamic_cast<const KeyValueSecret &>(match.GetSecret());

        Value extra;
        if (kv.TryGetValue("extra_http_headers", extra) && !extra.IsNull())
        {
            for (auto &entry : MapValue::GetChildren(extra))
            {
                auto &kv_pair = StructValue::GetChildren(entry);
                if (kv_pair.size() < 2 || kv_pair[0].IsNull() || kv_pair[1].IsNull())
                {
                    continue;
                }
                auto key = kv_pair[0].ToString();
                if (headers.find(key) == headers.end())
                {
                    headers.emplace(key, kv_pair[1].ToString());
                }
            }
        }

        Value bearer;
        if (kv.TryGetValue("bearer_token", bearer) && !bearer.IsNull())
        {
            auto token = bearer.ToString();
            if (!token.empty() && headers.find("Authorization") == headers.end())
            {
                headers.emplace("Authorization", "Bearer " + token);
            }
        }
    }

    static bool IsValidUtf8(const std::string &input)
    {
        const auto *data = reinterpret_cast<const unsigned char *>(input.data());
        const idx_t len = input.size();
        idx_t i = 0;
        while (i < len)
        {
            unsigned char c = data[i];
            if (c <= 0x7F)
            {
                i++;
                continue;
            }
            idx_t extra = 0;
            uint32_t min_cp = 0;
            uint32_t cp = 0;
            if ((c & 0xE0) == 0xC0)
            {
                extra = 1;
                cp = c & 0x1F;
                min_cp = 0x80;
            }
            else if ((c & 0xF0) == 0xE0)
            {
                extra = 2;
                cp = c & 0x0F;
                min_cp = 0x800;
            }
            else if ((c & 0xF8) == 0xF0)
            {
                extra = 3;
                cp = c & 0x07;
                min_cp = 0x10000;
            }
            else
            {
                return false;
            }
            if (i + extra >= len)
            {
                return false;
            }
            for (idx_t j = 1; j <= extra; j++)
            {
                unsigned char cc = data[i + j];
                if ((cc & 0xC0) != 0x80)
                {
                    return false;
                }
                cp = (cp << 6) | (cc & 0x3F);
            }
            if (cp < min_cp || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF))
            {
                return false;
            }
            i += extra + 1;
        }
        return true;
    }

    static vector<pair<string, string>> FlattenHeaders(const duckdb_httplib_openssl::Headers &headers)
    {
        vector<pair<string, string>> out;
        unordered_map<string, idx_t> index;
        for (const auto &pair : headers)
        {
            string key = pair.first;
            std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c)
                           { return std::tolower(c); });
            auto it = index.find(key);
            if (it == index.end())
            {
                index[key] = out.size();
                out.emplace_back(std::move(key), pair.second);
            }
            else
            {
                out[it->second].second += ", " + pair.second;
            }
        }
        return out;
    }

    static std::string HeadersToJsonObject(const duckdb_httplib_openssl::Headers &headers)
    {
        auto flattened = FlattenHeaders(headers);
        std::string result = "{";
        for (const auto &pair : flattened)
        {
            result += "\"" + escape_json(pair.first) + "\":\"" + escape_json(pair.second) + "\",";
        }
        if (result.length() > 1)
        {
            result.pop_back();
        }
        result += "}";
        return result;
    }

    static Value HeadersToMapValue(const duckdb_httplib_openssl::Headers &headers)
    {
        vector<Value> keys;
        vector<Value> values;
        for (const auto &pair : FlattenHeaders(headers))
        {
            keys.emplace_back(pair.first);
            values.emplace_back(pair.second);
        }
        return Value::MAP(LogicalType::VARCHAR, LogicalType::VARCHAR, keys, values);
    }

    static LogicalType HttpBlobResponseType()
    {
        child_list_t<LogicalType> fields;
        fields.emplace_back("status", LogicalType::INTEGER);
        fields.emplace_back("reason", LogicalType::VARCHAR);
        fields.emplace_back("headers", LogicalType::MAP(LogicalType::VARCHAR, LogicalType::VARCHAR));
        fields.emplace_back("body", LogicalType::BLOB);
        return LogicalType::STRUCT(std::move(fields));
    }

    static Value MakeBlobResponse(int32_t status, const std::string &reason,
                                  const duckdb_httplib_openssl::Headers &headers, const std::string &body)
    {
        child_list_t<Value> children;
        children.emplace_back("status", Value::INTEGER(status));
        children.emplace_back("reason", Value(reason));
        children.emplace_back("headers", HeadersToMapValue(headers));
        children.emplace_back("body", Value::BLOB(const_data_ptr_cast(body.data()), body.size()));
        return Value::STRUCT(std::move(children));
    }

    // Helper function to escape chars of a string representing a JSON object
    std::string escape_json(const std::string &input)
    {
        std::ostringstream output;

        for (auto c = input.cbegin(); c != input.cend(); c++)
        {
            switch (*c)
            {
            case '"':
                output << "\\\"";
                break;
            case '\\':
                output << "\\\\";
                break;
            case '\b':
                output << "\\b";
                break;
            case '\f':
                output << "\\f";
                break;
            case '\n':
                output << "\\n";
                break;
            case '\r':
                output << "\\r";
                break;
            case '\t':
                output << "\\t";
                break;
            default:
                if ('\x00' <= *c && *c <= '\x1f')
                {
                    output << "\\u"
                           << std::hex << std::setw(4) << std::setfill('0') << static_cast<int>(*c);
                }
                else
                {
                    output << *c;
                }
            }
        }
        return output.str();
    }

    // Helper function to create a Response object as a string.
    // Response headers are included as a JSON object (optional for callers: ignore the field).
    // Non-UTF-8 bodies are base64-encoded and marked with "body_base64": true so VARCHAR/JSON
    // never receives invalid unicode.
    static std::string GetJsonResponse(int status, const std::string &reason, const std::string &body,
                                       const duckdb_httplib_openssl::Headers *headers = nullptr)
    {
        std::string headers_json = headers ? HeadersToJsonObject(*headers) : "{}";
        if (!IsValidUtf8(body))
        {
            auto encoded = Blob::ToBase64(string_t(body));
            return StringUtil::Format(
                "{ \"status\": %i, \"reason\": \"%s\", \"body\": \"%s\", \"body_base64\": true, \"headers\": %s }",
                status, escape_json(reason), escape_json(encoded), headers_json);
        }
        return StringUtil::Format(
            "{ \"status\": %i, \"reason\": \"%s\", \"body\": \"%s\", \"headers\": %s }",
            status, escape_json(reason), escape_json(body), headers_json);
    }

    // Helper function to return the description of one HTTP error.
    static std::string GetHttpErrorMessage(const duckdb_httplib_openssl::Result &res, const std::string &request_type)
    {
        std::string err_message = "HTTP " + request_type + " request failed. ";

        switch (res.error())
        {
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

    static string_t HttpResultToJson(Vector &result, const duckdb_httplib_openssl::Result &res,
                                     const std::string &method)
    {
        if (res)
        {
            return StringVector::AddString(result, GetJsonResponse(res->status, res->reason, res->body, &res->headers));
        }
        return StringVector::AddString(result, GetJsonResponse(-1, GetHttpErrorMessage(res, method), ""));
    }

    // Helper function to convert list of entries to a map of parameters.
    template <class T>
    static int ConvertListEntryToMap(const list_entry_t &list_entry, const duckdb::Vector &input, T &result)
    {
        for (idx_t i = list_entry.offset; i < list_entry.offset + list_entry.length; i++)
        {
            const auto &child_value = input.GetValue(i);

            Vector tmp(child_value);
            auto &children = StructVector::GetEntries(tmp);

            if (children.size() == 2)
            {
                auto name = FlatVector::GetData<string_t>(*children[0]);
                auto data = FlatVector::GetData<string_t>(*children[1]);
                std::string key = name->GetString();
                std::string val = data->GetString();
                result.emplace(key, val);
            }
        }
        return result.size();
    }

    static std::string MapListToJsonObject(const list_entry_t &list_entry, const Vector &input)
    {
        duckdb_httplib_openssl::Params params;
        ConvertListEntryToMap(list_entry, input, params);
        std::ostringstream oss;
        oss << "{";
        bool first = true;
        for (const auto &pair : params)
        {
            if (!first)
            {
                oss << ", ";
            }
            first = false;
            oss << "\"" << escape_json(pair.first) << "\": \"" << escape_json(pair.second) << "\"";
        }
        oss << "}";
        return oss.str();
    }

    static void AppendMultipartFields(const list_entry_t &list_entry, const duckdb::Vector &input,
                                      duckdb_httplib_openssl::UploadFormDataItems &items)
    {
        duckdb_httplib_openssl::Params fields;
        ConvertListEntryToMap<duckdb_httplib_openssl::Params>(list_entry, input, fields);
        for (const auto &pair : fields)
        {
            items.push_back({pair.first, pair.second, "", ""});
        }
    }

    static void AppendMultipartFiles(const list_entry_t &list_entry, const duckdb::Vector &input,
                                     duckdb_httplib_openssl::UploadFormDataItems &items)
    {
        for (idx_t i = list_entry.offset; i < list_entry.offset + list_entry.length; i++)
        {
            const auto child_value = input.GetValue(i);
            if (child_value.IsNull() || child_value.type().id() != LogicalTypeId::STRUCT)
            {
                continue;
            }
            auto &children = StructValue::GetChildren(child_value);
            duckdb_httplib_openssl::UploadFormData item;
            if (!children.empty() && !children[0].IsNull())
            {
                item.name = children[0].ToString();
            }
            if (children.size() >= 2 && !children[1].IsNull())
            {
                auto blob = StringValue::Get(children[1]);
                item.content.assign(blob.data(), blob.size());
            }
            if (children.size() >= 3 && !children[2].IsNull())
            {
                item.filename = children[2].ToString();
            }
            if (children.size() >= 4 && !children[3].IsNull())
            {
                item.content_type = children[3].ToString();
            }
            if (!item.name.empty())
            {
                items.push_back(std::move(item));
            }
        }
    }

    static void HTTPHeadRequestFunction(DataChunk &args, ExpressionState &state, Vector &result)
    {
        D_ASSERT(args.data.size() == 1);

        UnaryExecutor::Execute<string_t, string_t>(args.data[0], result, args.size(), [&](string_t input)
                                                   {
        std::string url = input.GetString();

        duckdb_httplib_openssl::Headers header_map;
        ApplyHttpSecrets(state, url, header_map);

        auto client_and_path = SetupHttpClient(url, state);
        auto &client = client_and_path.first;
        auto &path = client_and_path.second;

        auto res = ExecuteWithRetry(state, "HEAD", [&]() { return client.Head(path.c_str(), header_map); });
        if (res) {
            return StringVector::AddString(result, GetJsonResponse(res->status, res->reason, "", &res->headers));
        }
        return StringVector::AddString(result, GetJsonResponse(-1, GetHttpErrorMessage(res, "HEAD"), "")); });
    }

    static void HTTPGetRequestFunction(DataChunk &args, ExpressionState &state, Vector &result)
    {
        D_ASSERT(args.data.size() == 1);

        UnaryExecutor::Execute<string_t, string_t>(args.data[0], result, args.size(), [&](string_t input)
                                                   {
        std::string url = input.GetString();

        duckdb_httplib_openssl::Headers header_map;
        ApplyHttpSecrets(state, url, header_map);

        auto client_and_path = SetupHttpClient(url, state);
        auto &client = client_and_path.first;
        auto &path = client_and_path.second;

        auto res = ExecuteWithRetry(state, "GET", [&]() { return client.Get(path.c_str(), header_map); });
        return HttpResultToJson(result, res, "GET"); });
    }

    static void HTTPGetExRequestFunction(DataChunk &args, ExpressionState &state, Vector &result)
    {
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
            [&](STRING_TYPE url, LENTRY_TYPE headers, LENTRY_TYPE params)
            {
                std::string url_str = url.val.GetString();

                // Use helper to setup client and parse URL
                auto client_and_path = SetupHttpClient(url_str, state);
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

                ApplyHttpSecrets(state, url_str, header_map);

                auto res = ExecuteWithRetry(state, "GET",
                                            [&]() { return client.Get(path.c_str(), param_map, header_map); });
                return HttpResultToJson(result, res, "GET");
            });
    }

    static void HTTPPostRequestFunction(DataChunk &args, ExpressionState &state, Vector &result)
    {
        D_ASSERT(args.data.size() == 3);

        using STRING_TYPE = PrimitiveType<string_t>;
        using LENTRY_TYPE = PrimitiveType<list_entry_t>;

        auto &url_vector = args.data[0];
        auto &headers_vector = args.data[1];
        auto &headers_entry = ListVector::GetEntry(headers_vector);
        auto &body_vector = args.data[2];

        GenericExecutor::ExecuteTernary<STRING_TYPE, LENTRY_TYPE, STRING_TYPE, STRING_TYPE>(
            url_vector, headers_vector, body_vector, result, args.size(),
            [&](STRING_TYPE url, LENTRY_TYPE headers, STRING_TYPE body)
            {
                std::string url_str = url.val.GetString();

                // Use helper to setup client and parse URL
                auto client_and_path = SetupHttpClient(url_str, state);
                auto &client = client_and_path.first;
                auto &path = client_and_path.second;

                // Prepare headers
                duckdb_httplib_openssl::Headers header_map;
                auto header_list = headers.val;
                ConvertListEntryToMap<duckdb_httplib_openssl::Headers>(header_list, headers_entry, header_map);

                ApplyHttpSecrets(state, url_str, header_map);

                auto res = ExecuteWithRetry(state, "POST", [&]() {
                    return client.Post(path.c_str(), header_map, body.val.GetString(), "application/json");
                });
                return HttpResultToJson(result, res, "POST");
            });
    }

    static void HTTPPostMapRequestFunction(DataChunk &args, ExpressionState &state, Vector &result)
    {
        D_ASSERT(args.data.size() == 3);

        using STRING_TYPE = PrimitiveType<string_t>;
        using LENTRY_TYPE = PrimitiveType<list_entry_t>;

        auto &headers_entry = ListVector::GetEntry(args.data[1]);
        auto &params_entry = ListVector::GetEntry(args.data[2]);

        GenericExecutor::ExecuteTernary<STRING_TYPE, LENTRY_TYPE, LENTRY_TYPE, STRING_TYPE>(
            args.data[0], args.data[1], args.data[2], result, args.size(),
            [&](STRING_TYPE url, LENTRY_TYPE headers, LENTRY_TYPE params)
            {
                std::string url_str = url.val.GetString();
                auto client_and_path = SetupHttpClient(url_str, state);
                auto &client = client_and_path.first;
                auto &path = client_and_path.second;

                duckdb_httplib_openssl::Headers header_map;
                ConvertListEntryToMap<duckdb_httplib_openssl::Headers>(headers.val, headers_entry, header_map);
                ApplyHttpSecrets(state, url_str, header_map);

                auto body = MapListToJsonObject(params.val, params_entry);
                auto res = ExecuteWithRetry(state, "POST", [&]() {
                    return client.Post(path.c_str(), header_map, body, "application/json");
                });
                return HttpResultToJson(result, res, "POST");
            });
    }

    static void HTTPPostFormRequestFunction(DataChunk &args, ExpressionState &state, Vector &result)
    {
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
            [&](STRING_TYPE url, LENTRY_TYPE headers, LENTRY_TYPE params)
            {
                std::string url_str = url.val.GetString();

                // Use helper to setup client and parse URL
                auto client_and_path = SetupHttpClient(url_str, state);
                auto &client = client_and_path.first;
                auto &path = client_and_path.second;

                // Prepare headers and parameters
                duckdb_httplib_openssl::Headers header_map;
                duckdb_httplib_openssl::Params params_map;
                ConvertListEntryToMap<duckdb_httplib_openssl::Headers>(headers.val, headers_entry, header_map);
                ConvertListEntryToMap<duckdb_httplib_openssl::Params>(params.val, body_entry, params_map);

                ApplyHttpSecrets(state, url_str, header_map);

                auto res = ExecuteWithRetry(state, "POST",
                                            [&]() { return client.Post(path.c_str(), header_map, params_map); });
                return HttpResultToJson(result, res, "POST");
            });
    }

    static void HTTPPostRawBodyFunction(DataChunk &args, ExpressionState &state, Vector &result)
    {
        using STRING_TYPE = PrimitiveType<string_t>;
        using LENTRY_TYPE = PrimitiveType<list_entry_t>;

        auto emit = [&](const std::string &url_str, duckdb_httplib_openssl::Headers header_map,
                        const std::string &body)
        {
            ApplyHttpSecrets(state, url_str, header_map);
            auto client_and_path = SetupHttpClient(url_str, state);
            auto content_type = ResolveContentType(header_map, "text/plain");
            auto res = ExecuteWithRetry(state, "POST", [&]() {
                return client_and_path.first.Post(client_and_path.second.c_str(), header_map, body, content_type);
            });
            return HttpResultToJson(result, res, "POST");
        };

        if (args.data.size() == 2)
        {
            GenericExecutor::ExecuteBinary<STRING_TYPE, STRING_TYPE, STRING_TYPE>(
                args.data[0], args.data[1], result, args.size(),
                [&](STRING_TYPE url, STRING_TYPE body)
                {
                    return emit(url.val.GetString(), duckdb_httplib_openssl::Headers {}, body.val.GetString());
                });
            return;
        }

        D_ASSERT(args.data.size() == 3);
        auto &headers_entry = ListVector::GetEntry(args.data[1]);
        GenericExecutor::ExecuteTernary<STRING_TYPE, LENTRY_TYPE, STRING_TYPE, STRING_TYPE>(
            args.data[0], args.data[1], args.data[2], result, args.size(),
            [&](STRING_TYPE url, LENTRY_TYPE headers, STRING_TYPE body)
            {
                duckdb_httplib_openssl::Headers header_map;
                ConvertListEntryToMap<duckdb_httplib_openssl::Headers>(headers.val, headers_entry, header_map);
                return emit(url.val.GetString(), std::move(header_map), body.val.GetString());
            });
    }

    static void HTTPPostMultipartRequestFunction(DataChunk &args, ExpressionState &state, Vector &result)
    {
        D_ASSERT(args.data.size() == 4);

        using STRING_TYPE = PrimitiveType<string_t>;
        using LENTRY_TYPE = PrimitiveType<list_entry_t>;

        auto &headers_entry = ListVector::GetEntry(args.data[1]);
        auto &fields_entry = ListVector::GetEntry(args.data[2]);
        auto &files_entry = ListVector::GetEntry(args.data[3]);

        GenericExecutor::ExecuteQuaternary<STRING_TYPE, LENTRY_TYPE, LENTRY_TYPE, LENTRY_TYPE, STRING_TYPE>(
            args.data[0], args.data[1], args.data[2], args.data[3], result, args.size(),
            [&](STRING_TYPE url, LENTRY_TYPE headers, LENTRY_TYPE fields, LENTRY_TYPE files)
            {
                std::string url_str = url.val.GetString();

                auto client_and_path = SetupHttpClient(url_str, state);
                auto &client = client_and_path.first;
                auto &path = client_and_path.second;

                duckdb_httplib_openssl::Headers header_map;
                ConvertListEntryToMap<duckdb_httplib_openssl::Headers>(headers.val, headers_entry, header_map);
                ApplyHttpSecrets(state, url_str, header_map);

                duckdb_httplib_openssl::UploadFormDataItems items;
                AppendMultipartFields(fields.val, fields_entry, items);
                AppendMultipartFiles(files.val, files_entry, items);

                auto res = ExecuteWithRetry(state, "POST",
                                            [&]() { return client.Post(path.c_str(), header_map, items); });
                return HttpResultToJson(result, res, "POST");
            });
    }

    static Value PerformHttpGetBlob(ExpressionState &state, const std::string &url,
                                    duckdb_httplib_openssl::Headers header_map,
                                    const duckdb_httplib_openssl::Params *params = nullptr)
    {
        ApplyHttpSecrets(state, url, header_map);
        auto client_and_path = SetupHttpClient(url, state);
        auto &client = client_and_path.first;
        auto &path = client_and_path.second;
        auto res = ExecuteWithRetry(state, "GET", [&]() {
            return params ? client.Get(path.c_str(), *params, header_map) : client.Get(path.c_str(), header_map);
        });
        if (res)
        {
            return MakeBlobResponse(NumericCast<int32_t>(res->status), res->reason, res->headers, res->body);
        }
        return MakeBlobResponse(-1, GetHttpErrorMessage(res, "GET"), duckdb_httplib_openssl::Headers {}, "");
    }

    static void HTTPGetBlobRequestFunction(DataChunk &args, ExpressionState &state, Vector &result)
    {
        D_ASSERT(args.data.size() == 1);
        for (idx_t i = 0; i < args.size(); i++)
        {
            auto url_val = args.data[0].GetValue(i);
            if (url_val.IsNull())
            {
                FlatVector::SetNull(result, i, true);
                continue;
            }
            result.SetValue(i, PerformHttpGetBlob(state, url_val.ToString(), {}));
        }
        if (args.size() == 1 && args.data[0].GetVectorType() == VectorType::CONSTANT_VECTOR)
        {
            result.SetVectorType(VectorType::CONSTANT_VECTOR);
        }
    }

    static void HTTPGetBlobExRequestFunction(DataChunk &args, ExpressionState &state, Vector &result)
    {
        D_ASSERT(args.data.size() == 3);

        UnifiedVectorFormat url_data, headers_data, params_data;
        args.data[0].ToUnifiedFormat(args.size(), url_data);
        args.data[1].ToUnifiedFormat(args.size(), headers_data);
        args.data[2].ToUnifiedFormat(args.size(), params_data);

        auto url_ptr = UnifiedVectorFormat::GetData<string_t>(url_data);
        auto header_lists = ListVector::GetData(args.data[1]);
        auto param_lists = ListVector::GetData(args.data[2]);
        auto &headers_entry = ListVector::GetEntry(args.data[1]);
        auto &params_entry = ListVector::GetEntry(args.data[2]);

        for (idx_t i = 0; i < args.size(); i++)
        {
            auto uidx = url_data.sel->get_index(i);
            auto hidx = headers_data.sel->get_index(i);
            auto pidx = params_data.sel->get_index(i);
            if (!url_data.validity.RowIsValid(uidx) || !headers_data.validity.RowIsValid(hidx) ||
                !params_data.validity.RowIsValid(pidx))
            {
                FlatVector::SetNull(result, i, true);
                continue;
            }
            duckdb_httplib_openssl::Headers header_map;
            duckdb_httplib_openssl::Params param_map;
            ConvertListEntryToMap(header_lists[hidx], headers_entry, header_map);
            ConvertListEntryToMap(param_lists[pidx], params_entry, param_map);
            result.SetValue(i, PerformHttpGetBlob(state, url_ptr[uidx].GetString(), std::move(header_map), &param_map));
        }
    }

    static unique_ptr<FunctionData> BindHttpPostBody(ClientContext &, ScalarFunction &bound_function,
                                                     vector<unique_ptr<Expression>> &arguments)
    {
        D_ASSERT(arguments.size() == 3);
        const auto &body_type = arguments[2]->return_type;
        if (body_type.id() == LogicalTypeId::MAP)
        {
            bound_function.arguments[2] = LogicalType::MAP(LogicalType::VARCHAR, LogicalType::VARCHAR);
            bound_function.function = HTTPPostMapRequestFunction;
        }
        else if (body_type.IsJSONType() || body_type.id() == LogicalTypeId::STRUCT ||
                 body_type.id() == LogicalTypeId::LIST)
        {
            bound_function.arguments[2] = LogicalType::JSON();
            bound_function.function = HTTPPostRequestFunction;
        }
        else
        {
            bound_function.arguments[2] = LogicalType::VARCHAR;
            bound_function.function = HTTPPostRawBodyFunction;
        }
        return nullptr;
    }

    static void LoadInternal(ExtensionLoader &loader)
    {
        ScalarFunctionSet http_head("http_head");
        http_head.AddFunction(HttpFn({LogicalType::VARCHAR}, HTTPHeadRequestFunction));
        loader.RegisterFunction(http_head);

        ScalarFunctionSet http_get("http_get");
        http_get.AddFunction(HttpFn({LogicalType::VARCHAR}, HTTPGetRequestFunction));
        http_get.AddFunction(HttpFn(
            {LogicalType::VARCHAR, LogicalType::MAP(LogicalType::VARCHAR, LogicalType::VARCHAR),
             LogicalType::MAP(LogicalType::VARCHAR, LogicalType::VARCHAR)},
            HTTPGetExRequestFunction));
        loader.RegisterFunction(http_get);

        ScalarFunctionSet http_get_blob("http_get_blob");
        http_get_blob.AddFunction(HttpFn({LogicalType::VARCHAR}, HttpBlobResponseType(), HTTPGetBlobRequestFunction));
        http_get_blob.AddFunction(HttpFn(
            {LogicalType::VARCHAR, LogicalType::MAP(LogicalType::VARCHAR, LogicalType::VARCHAR),
             LogicalType::MAP(LogicalType::VARCHAR, LogicalType::VARCHAR)},
            HttpBlobResponseType(), HTTPGetBlobExRequestFunction));
        loader.RegisterFunction(http_get_blob);

        auto header_map_type = LogicalType::MAP(LogicalType::VARCHAR, LogicalType::VARCHAR);
        auto field_map_type = LogicalType::MAP(LogicalType::VARCHAR, LogicalType::VARCHAR);

        ScalarFunctionSet http_post("http_post");
        auto http_post_body = HttpFn({LogicalType::VARCHAR, header_map_type, LogicalType::ANY},
                                     HTTPPostRequestFunction);
        http_post_body.SetBindCallback(BindHttpPostBody);
        http_post.AddFunction(std::move(http_post_body));
        http_post.AddFunction(HttpFn({LogicalType::VARCHAR, LogicalType::VARCHAR}, HTTPPostRawBodyFunction));
        loader.RegisterFunction(http_post);

        ScalarFunctionSet http_post_form("http_post_form");
        http_post_form.AddFunction(HttpFn({LogicalType::VARCHAR, header_map_type, field_map_type},
                                          HTTPPostFormRequestFunction));
        loader.RegisterFunction(http_post_form);

        ScalarFunctionSet http_post_multipart("http_post_multipart");
        http_post_multipart.AddFunction(HttpFn(
            {LogicalType::VARCHAR, header_map_type, field_map_type, LogicalType::LIST(MultipartFileType())},
            HTTPPostMultipartRequestFunction));
        loader.RegisterFunction(http_post_multipart);

        auto &config = DBConfig::GetConfig(loader.GetDatabaseInstance());
        if (!config.HasExtensionOption("http_client_retries"))
        {
            config.AddExtensionOption(
                "http_client_retries",
                "Number of extra attempts after a failed HTTP client request (transport errors and selected 408/429/5xx). Default 0.",
                LogicalType::UBIGINT, Value::UBIGINT(0));
            config.AddExtensionOption("http_client_retry_wait_ms",
                                      "Base wait in milliseconds before the first HTTP client retry.",
                                      LogicalType::UBIGINT, Value::UBIGINT(100));
            config.AddExtensionOption("http_client_retry_backoff",
                                      "Exponential backoff multiplier applied after each HTTP client retry.",
                                      LogicalType::DOUBLE, Value(4.0));
        }
        if (!config.HasExtensionOption("http_client_max_parallel"))
        {
            config.AddExtensionOption(
                "http_client_max_parallel",
                "Maximum in-flight HTTP client requests across DuckDB threads. 0 means unlimited (default).",
                LogicalType::UBIGINT, Value::UBIGINT(0));
        }

        QueryFarmSendTelemetry(loader, "http_client", "2026082701");
    }

    void HttpClientExtension::Load(ExtensionLoader &loader)
    {
        LoadInternal(loader);
    }

    std::string HttpClientExtension::Name()
    {
        return "http_client";
    }

    std::string HttpClientExtension::Version() const
    {
        return "2026082701";
    }

} // namespace duckdb

extern "C"
{
    DUCKDB_CPP_EXTENSION_ENTRY(http_client, loader)
    {
        duckdb::LoadInternal(loader);
    }
}
