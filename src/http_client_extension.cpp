#define DUCKDB_EXTENSION_MAIN
#include "http_client_extension.hpp"

#include "duckdb.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/exception/http_exception.hpp"
#include "duckdb/common/operator/cast_operators.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/types/blob.hpp"
#include "duckdb/common/vector/flat_vector.hpp"
#include "duckdb/common/vector/string_vector.hpp"
#include "duckdb/function/scalar_function.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/main/extension_helper.hpp"
#include "duckdb/main/http/http_util.hpp"
#include "duckdb/main/settings.hpp"
#include "duckdb/parser/parsed_data/create_scalar_function_info.hpp"

#include "query_farm_telemetry.hpp"

#include <string>

namespace duckdb {

//! The extension version, reported by Version() and sent with the telemetry ping.
static constexpr const char *HTTP_CLIENT_VERSION = "2026110701";

static constexpr const char *MAX_RESPONSE_SIZE_SETTING = "http_client_max_response_size";
static constexpr idx_t DEFAULT_MAX_RESPONSE_SIZE = 100ULL * 1024ULL * 1024ULL; // 100 MiB
static constexpr const char *MAX_REDIRECTS_SETTING = "http_client_max_redirects";
static constexpr idx_t DEFAULT_MAX_REDIRECTS = 10;

// CamelCase: DELETE is a macro in <winnt.h>
enum class HttpMethod { Head, Get, Post, PostForm, Put, Delete };

static const char *MethodName(HttpMethod method) {
	switch (method) {
	case HttpMethod::Head:
		return "HEAD";
	case HttpMethod::Get:
		return "GET";
	case HttpMethod::Post:
	case HttpMethod::PostForm:
		return "POST";
	case HttpMethod::Put:
		return "PUT";
	case HttpMethod::Delete:
		return "DELETE";
	}
	return "GET";
}

//===--------------------------------------------------------------------===//
// Text helpers
//===--------------------------------------------------------------------===//

// Length of the valid UTF-8 sequence starting at s[i], or 0 if it is not valid (overlong forms, surrogates and
// code points above U+10FFFF are rejected).
static idx_t Utf8SequenceLength(const string &s, idx_t i) {
	auto c = static_cast<unsigned char>(s[i]);
	if (c < 0x80) {
		return 1;
	}
	idx_t len;
	uint32_t cp;
	if (c >= 0xC2 && c <= 0xDF) {
		len = 2;
		cp = c & 0x1F;
	} else if (c >= 0xE0 && c <= 0xEF) {
		len = 3;
		cp = c & 0x0F;
	} else if (c >= 0xF0 && c <= 0xF4) {
		len = 4;
		cp = c & 0x07;
	} else {
		return 0;
	}
	if (i + len > s.size()) {
		return 0;
	}
	for (idx_t k = 1; k < len; k++) {
		auto cc = static_cast<unsigned char>(s[i + k]);
		if ((cc & 0xC0) != 0x80) {
			return 0;
		}
		cp = (cp << 6) | (cc & 0x3F);
	}
	if ((len == 3 && cp < 0x800) || (len == 4 && (cp < 0x10000 || cp > 0x10FFFF)) || (cp >= 0xD800 && cp <= 0xDFFF)) {
		return 0;
	}
	return len;
}

static bool IsValidUtf8(const string &s) {
	for (idx_t i = 0; i < s.size();) {
		auto len = Utf8SequenceLength(s, i);
		if (len == 0) {
			return false;
		}
		i += len;
	}
	return true;
}

// Replaces every byte that is not part of a valid UTF-8 sequence with U+FFFD (for reasons and header values).
static string SanitizeUtf8(const string &s) {
	if (IsValidUtf8(s)) {
		return s;
	}
	string out;
	for (idx_t i = 0; i < s.size();) {
		auto len = Utf8SequenceLength(s, i);
		if (len == 0) {
			out += "\xEF\xBF\xBD";
			i++;
		} else {
			out.append(s, i, len);
			i += len;
		}
	}
	return out;
}

// Escapes a (valid UTF-8) string for use inside a JSON string literal.
static string EscapeJson(const string &input) {
	string output;
	output.reserve(input.size() + 16);
	for (auto ch : input) {
		auto c = static_cast<unsigned char>(ch);
		switch (c) {
		case '"':
			output += "\\\"";
			break;
		case '\\':
			output += "\\\\";
			break;
		case '\b':
			output += "\\b";
			break;
		case '\f':
			output += "\\f";
			break;
		case '\n':
			output += "\\n";
			break;
		case '\r':
			output += "\\r";
			break;
		case '\t':
			output += "\\t";
			break;
		default:
			if (c < 0x20 || c == 0x7f) {
				output += StringUtil::Format("\\u%04x", static_cast<int>(c));
			} else {
				output += ch;
			}
		}
	}
	return output;
}

static bool IsUnreserved(unsigned char c) {
	return StringUtil::CharacterIsAlphaNumeric(static_cast<char>(c)) || c == '-' || c == '_' || c == '.' || c == '~';
}

// Percent-encodes everything except RFC 3986 unreserved characters (query parameters and form bodies).
static string PercentEncode(const string &input) {
	static const char *HEX = "0123456789ABCDEF";
	string out;
	for (auto ch : input) {
		auto c = static_cast<unsigned char>(ch);
		if (IsUnreserved(c)) {
			out += ch;
		} else {
			out += '%';
			out += HEX[c >> 4];
			out += HEX[c & 0x0F];
		}
	}
	return out;
}

static int HexValue(char c) {
	if (c >= '0' && c <= '9') {
		return c - '0';
	}
	if (c >= 'a' && c <= 'f') {
		return c - 'a' + 10;
	}
	if (c >= 'A' && c <= 'F') {
		return c - 'A' + 10;
	}
	return -1;
}

static string PercentDecode(const string &input) {
	string out;
	for (idx_t i = 0; i < input.size(); i++) {
		if (input[i] == '%' && i + 2 < input.size()) {
			auto hi = HexValue(input[i + 1]);
			auto lo = HexValue(input[i + 2]);
			if (hi >= 0 && lo >= 0) {
				out += static_cast<char>(hi * 16 + lo);
				i += 2;
				continue;
			}
		}
		out += input[i];
	}
	return out;
}

//===--------------------------------------------------------------------===//
// Settings and policy
//===--------------------------------------------------------------------===//

static idx_t GetUBigIntSetting(ClientContext &context, const char *name, idx_t default_value) {
	Value value;
	if (context.TryGetCurrentSetting(name, value) && !value.IsNull()) {
		return UBigIntValue::Get(value.DefaultCastAs(LogicalType::UBIGINT));
	}
	return default_value;
}

static void CheckExternalAccess(ClientContext &context, HttpMethod method) {
	if (!Settings::Get<EnableExternalAccessSetting>(context)) {
		throw PermissionException("http_client: HTTP %s requests are disabled because enable_external_access is false",
		                          MethodName(method));
	}
}

struct ParsedUrl {
	//! The URL that is sent (no userinfo, no fragment)
	string url;
	bool https = false;
	//! user:password from the URL, percent-decoded, if there was one
	bool has_userinfo = false;
	string userinfo;
};

// Validates an http(s) URL and splits off userinfo and the fragment. A URL without a scheme is treated as
// http://, as earlier versions of the extension did.
static ParsedUrl ParseUrl(const string &input) {
	for (auto ch : input) {
		auto c = static_cast<unsigned char>(ch);
		if (c <= 0x20 || c == 0x7f) {
			throw InvalidInputException("http_client: URL \"%s\" contains whitespace or control characters - "
			                            "percent-encode them",
			                            SanitizeUtf8(input));
		}
	}
	ParsedUrl result;
	string rest;
	auto scheme_end = input.find("://");
	if (scheme_end == string::npos) {
		rest = input;
	} else {
		auto scheme = StringUtil::Lower(input.substr(0, scheme_end));
		if (scheme == "https") {
			result.https = true;
		} else if (scheme != "http") {
			throw InvalidInputException("http_client: unsupported URL scheme \"%s\" - only http:// and https:// URLs "
			                            "are supported",
			                            SanitizeUtf8(input.substr(0, scheme_end)));
		}
		rest = input.substr(scheme_end + 3);
	}
	auto fragment = rest.find('#');
	if (fragment != string::npos) {
		rest = rest.substr(0, fragment);
	}
	auto authority_end = rest.find_first_of("/?");
	auto authority = rest.substr(0, authority_end);
	auto path = authority_end == string::npos ? string("/") : rest.substr(authority_end);
	if (!path.empty() && path[0] == '?') {
		path = "/" + path;
	}
	auto at = authority.rfind('@');
	if (at != string::npos) {
		result.has_userinfo = true;
		result.userinfo = PercentDecode(authority.substr(0, at));
		authority = authority.substr(at + 1);
	}
	if (authority.empty()) {
		throw InvalidInputException("http_client: URL \"%s\" has no host", SanitizeUtf8(input));
	}
	result.url = (result.https ? "https://" : "http://") + authority + path;
	return result;
}

static string ProtoHostPort(const string &url) {
	string path, proto_host_port;
	HTTPUtil::DecomposeURL(url, path, proto_host_port);
	return StringUtil::Lower(proto_host_port);
}

// Resolves a Location header against the URL that returned it.
static string ResolveRedirect(const string &base_url, const string &location) {
	auto lower = StringUtil::Lower(location);
	if (lower.find("://") != string::npos) {
		return location;
	}
	string path, proto_host_port;
	HTTPUtil::DecomposeURL(base_url, path, proto_host_port);
	if (StringUtil::StartsWith(location, "//")) {
		return base_url.substr(0, base_url.find("://") + 1) + location;
	}
	if (StringUtil::StartsWith(location, "/")) {
		return proto_host_port + location;
	}
	auto dir = path.substr(0, path.find('?'));
	auto slash = dir.rfind('/');
	dir = slash == string::npos ? "/" : dir.substr(0, slash + 1);
	return proto_host_port + dir + location;
}

static bool IsRedirect(HTTPStatusCode status) {
	switch (status) {
	case HTTPStatusCode::MovedPermanently_301:
	case HTTPStatusCode::Found_302:
	case HTTPStatusCode::SeeOther_303:
	case HTTPStatusCode::TemporaryRedirect_307:
	case HTTPStatusCode::PermanentRedirect_308:
		return true;
	default:
		return false;
	}
}

// DuckDB's built-in HTTP client only speaks plain-HTTP GET. https:// and every other method need httpfs, which
// DuckDB autoloads when it is installed (or installable).
static HTTPUtil &GetHttpUtil(ClientContext &context, bool https, HttpMethod method) {
	auto &db = DatabaseInstance::GetDatabase(context);
	auto &http_util = HTTPUtil::Get(db);
#ifndef __EMSCRIPTEN__
	if (http_util.GetName() == "Built-In" && (https || method != HttpMethod::Get)) {
		ExtensionHelper::TryAutoLoadExtension(context, "httpfs");
		auto &loaded_util = HTTPUtil::Get(db);
		if (loaded_util.GetName() == "Built-In") {
			throw MissingExtensionException("http_client needs the httpfs extension %s. Run INSTALL httpfs; LOAD "
			                                "httpfs; and try again",
			                                https ? string("for https:// URLs")
			                                      : StringUtil::Format("for HTTP %s requests", MethodName(method)));
		}
		return loaded_util;
	}
#endif
	return http_util;
}

//===--------------------------------------------------------------------===//
// Requests
//===--------------------------------------------------------------------===//

using HeaderList = vector<std::pair<string, string>>;

struct HttpRequestSpec {
	HttpMethod method;
	string url;
	HeaderList headers;
	string body;
	//! Content-Type sent with a body unless the caller set one
	string default_content_type;
};

struct HttpResult {
	//! false when no HTTP response was received (connection refused, timeout, TLS failure, ...)
	bool ok = false;
	int status = -1;
	string reason;
	string body;
	HeaderList headers;
};

static bool HasHeader(const HeaderList &headers, const string &name) {
	for (auto &h : headers) {
		if (StringUtil::CIEquals(h.first, name)) {
			return true;
		}
	}
	return false;
}

static void RemoveHeader(HeaderList &headers, const string &name) {
	HeaderList kept;
	for (auto &h : headers) {
		if (!StringUtil::CIEquals(h.first, name)) {
			kept.push_back(h);
		}
	}
	headers = std::move(kept);
}

static void ValidateHeader(const string &name, const string &value) {
	if (name.empty()) {
		throw InvalidInputException("http_client: header names must not be empty");
	}
	for (auto ch : name) {
		auto c = static_cast<unsigned char>(ch);
		if (c <= 0x20 || c >= 0x7f || ch == ':' || ch == '(' || ch == ')' || ch == ',' || ch == ';' || ch == '<' ||
		    ch == '>' || ch == '=' || ch == '@' || ch == '[' || ch == ']' || ch == '{' || ch == '}' || ch == '"' ||
		    ch == '/' || ch == '?' || ch == '\\') {
			throw InvalidInputException("http_client: invalid HTTP header name \"%s\"", SanitizeUtf8(name));
		}
	}
	for (auto ch : value) {
		if (ch == '\r' || ch == '\n' || ch == '\0') {
			throw InvalidInputException("http_client: the value of header \"%s\" contains a line break or NUL byte",
			                            name);
		}
	}
}

static void CollectResponseHeaders(const HTTPResponse &response, HeaderList &out) {
	for (auto &entry : response.headers) {
		auto values = response.headers.GetHeaderValues(entry.first);
		string joined;
		if (values.empty()) {
			joined = entry.second;
		} else {
			joined = StringUtil::Join(values, ", ");
		}
		out.emplace_back(StringUtil::Lower(entry.first), joined);
	}
}

static string ResponseTooLargeMessage(const string &url, idx_t max_size) {
	return StringUtil::Format("http_client: the response from \"%s\" is larger than %s = %llu bytes", url,
	                          MAX_RESPONSE_SIZE_SETTING, max_size);
}

// Sends one request (no redirect handling) through DuckDB's HTTPUtil.
static unique_ptr<HTTPResponse> SendOnce(ClientContext &context, HttpMethod method, const string &url, bool https,
                                         const HeaderList &header_list, const string &body,
                                         const string &content_type, idx_t max_size, string &body_out) {
	auto &http_util = GetHttpUtil(context, https, method);
	auto params = http_util.InitializeParameters(context, url);
	params->follow_location = false;

	HTTPHeaders headers;
	for (auto &h : header_list) {
		headers.Insert(h.first, h.second);
	}
	body_out.clear();
	unique_ptr<HTTPResponse> response;
	switch (method) {
	case HttpMethod::Get: {
		bool too_large = false;
		bool interrupted = false;
		GetRequestInfo request(
		    url, headers, *params,
		    [&](const HTTPResponse &resp) {
			    body_out.clear();
			    if (too_large || interrupted) {
				    return false;
			    }
			    if (resp.HasHeader("Content-Length")) {
				    idx_t content_length;
				    auto header = resp.GetHeaderValue("Content-Length");
				    if (TryCast::Operation<string_t, idx_t>(string_t(header), content_length, false) &&
				        content_length > max_size) {
					    too_large = true;
					    return false;
				    }
			    }
			    return true;
		    },
		    [&](const_data_ptr_t data, idx_t data_length) {
			    if (context.IsInterrupted()) {
				    interrupted = true;
				    return false;
			    }
			    if (body_out.size() + data_length > max_size) {
				    too_large = true;
				    return false;
			    }
			    body_out.append(const_char_ptr_cast(data), data_length);
			    return true;
		    });
		request.try_request = true;
		response = http_util.Request(request);
		if (interrupted) {
			throw InterruptException();
		}
		if (too_large) {
			throw InvalidInputException(ResponseTooLargeMessage(url, max_size));
		}
		return response;
	}
	case HttpMethod::Head: {
		HeadRequestInfo request(url, headers, *params);
		request.try_request = true;
		response = http_util.Request(request);
		break;
	}
	case HttpMethod::Delete: {
		DeleteRequestInfo request(url, headers, *params);
		request.try_request = true;
		response = http_util.Request(request);
		break;
	}
	case HttpMethod::Put: {
		PutRequestInfo request(url, headers, *params, const_data_ptr_cast(body.data()), body.size(), content_type);
		request.try_request = true;
		response = http_util.Request(request);
		break;
	}
	case HttpMethod::Post:
	case HttpMethod::PostForm: {
		if (!headers.HasHeader("Content-Type")) {
			headers.Insert("Content-Type", content_type);
		}
		PostRequestInfo request(url, headers, *params, const_data_ptr_cast(body.data()), body.size());
		request.try_request = true;
		response = http_util.Request(request);
		if (response && !response->HasRequestError()) {
			body_out = request.buffer_out.empty() ? response->body : request.buffer_out;
		}
		break;
	}
	}
	if (context.IsInterrupted()) {
		throw InterruptException();
	}
	if (response && !response->HasRequestError() && method != HttpMethod::Post &&
	    method != HttpMethod::PostForm) {
		body_out = response->body;
	}
	if (body_out.size() > max_size) {
		throw InvalidInputException(ResponseTooLargeMessage(url, max_size));
	}
	return response;
}

// Performs a request, following redirects by hand so every hop is checked again: https:// is never downgraded to
// http://, and credentials the caller supplied (Authorization, Cookie, URL userinfo) are dropped when a redirect
// leaves the original scheme/host/port. HTTP secrets are looked up again for every hop.
static HttpResult PerformRequest(ClientContext &context, HttpRequestSpec spec) {
	CheckExternalAccess(context, spec.method);
	const auto max_size = GetUBigIntSetting(context, MAX_RESPONSE_SIZE_SETTING, DEFAULT_MAX_RESPONSE_SIZE);
	const auto max_redirects = GetUBigIntSetting(context, MAX_REDIRECTS_SETTING, DEFAULT_MAX_REDIRECTS);

	for (auto &h : spec.headers) {
		ValidateHeader(h.first, h.second);
	}
	auto parsed = ParseUrl(spec.url);
	if (parsed.has_userinfo && !HasHeader(spec.headers, "Authorization")) {
		spec.headers.emplace_back("Authorization",
		                          "Basic " + Blob::ToBase64(string_t(parsed.userinfo.data(),
		                                                             NumericCast<uint32_t>(parsed.userinfo.size()))));
	}
	string url = parsed.url;
	bool https = parsed.https;
	const auto origin = ProtoHostPort(url);
	auto method = spec.method;

	HttpResult result;
	for (idx_t hop = 0;; hop++) {
		CheckExternalAccess(context, method);
		if (context.IsInterrupted()) {
			throw InterruptException();
		}
		string body_out;
		auto response = SendOnce(context, method, url, https, spec.headers, spec.body, spec.default_content_type,
		                         max_size, body_out);
		if (!response || response->HasRequestError()) {
			result.ok = false;
			result.status = -1;
			result.reason = StringUtil::Format("HTTP %s request failed. %s", MethodName(method),
			                                   response ? response->GetRequestError() : string("Unknown error."));
			return result;
		}
		if (IsRedirect(response->status) && response->HasHeader("Location") && max_redirects > 0) {
			if (hop >= max_redirects) {
				result.ok = false;
				result.status = -1;
				result.reason = StringUtil::Format("HTTP %s request failed. Too many redirects (more than %s = %llu).",
				                                   MethodName(method), MAX_REDIRECTS_SETTING, max_redirects);
				return result;
			}
			auto next = ParseUrl(ResolveRedirect(url, response->GetHeaderValue("Location")));
			if (https && !next.https) {
				throw IOException("http_client: refusing to follow a redirect from https:// to plain http:// (\"%s\" "
				                  "-> \"%s\")",
				                  url, next.url);
			}
			if (ProtoHostPort(next.url) != origin) {
				RemoveHeader(spec.headers, "Authorization");
				RemoveHeader(spec.headers, "Cookie");
				RemoveHeader(spec.headers, "Proxy-Authorization");
			}
			if (next.has_userinfo) {
				throw IOException("http_client: refusing to follow a redirect to a URL with credentials (\"%s\")",
				                  next.url);
			}
			auto status = response->status;
			if (method != HttpMethod::Head &&
			    (status == HTTPStatusCode::SeeOther_303 ||
			     ((status == HTTPStatusCode::MovedPermanently_301 || status == HTTPStatusCode::Found_302) &&
			      (method == HttpMethod::Post || method == HttpMethod::PostForm)))) {
				// As browsers do: the redirected request becomes a GET without a body
				method = HttpMethod::Get;
				spec.body.clear();
				RemoveHeader(spec.headers, "Content-Type");
			}
			url = next.url;
			https = next.https;
			continue;
		}
		result.ok = true;
		result.status = static_cast<int>(response->status);
		result.reason = response->reason.empty() ? HTTPUtil::GetStatusMessage(response->status) : response->reason;
		result.body = std::move(body_out);
		CollectResponseHeaders(*response, result.headers);
		return result;
	}
}

//===--------------------------------------------------------------------===//
// Result JSON
//===--------------------------------------------------------------------===//

static string HeadersToJson(const HeaderList &headers) {
	string out = "{";
	for (idx_t i = 0; i < headers.size(); i++) {
		if (i > 0) {
			out += ", ";
		}
		out += "\"" + EscapeJson(SanitizeUtf8(headers[i].first)) + "\": \"" +
		       EscapeJson(SanitizeUtf8(headers[i].second)) + "\"";
	}
	out += "}";
	return out;
}

static string ResultToJson(const HttpResult &result, bool include_body) {
	string out = StringUtil::Format("{ \"status\": %d, \"reason\": \"%s\"", result.status,
	                                EscapeJson(SanitizeUtf8(result.reason)));
	if (include_body) {
		if (IsValidUtf8(result.body)) {
			out += ", \"body\": \"" + EscapeJson(result.body) + "\"";
		} else {
			// JSON strings must be UTF-8: binary bodies are returned base64-encoded
			out += ", \"body\": \"" +
			       Blob::ToBase64(string_t(result.body.data(), NumericCast<uint32_t>(result.body.size()))) +
			       "\", \"body_encoding\": \"base64\"";
		}
	}
	out += ", \"headers\": " + HeadersToJson(result.headers) + " }";
	return out;
}

//===--------------------------------------------------------------------===//
// Scalar functions
//===--------------------------------------------------------------------===//

// Reads a MAP(VARCHAR, VARCHAR) argument; NULL means no entries and NULL values become empty strings.
static HeaderList MapToList(const Value &value) {
	HeaderList result;
	if (value.IsNull()) {
		return result;
	}
	for (auto &entry : MapValue::GetChildren(value)) {
		auto &kv = StructValue::GetChildren(entry);
		if (kv.size() != 2 || kv[0].IsNull()) {
			continue;
		}
		result.emplace_back(kv[0].ToString(), kv[1].IsNull() ? string() : kv[1].ToString());
	}
	return result;
}

static string EncodeForm(const HeaderList &params) {
	string out;
	for (auto &p : params) {
		if (!out.empty()) {
			out += '&';
		}
		out += PercentEncode(p.first) + "=" + PercentEncode(p.second);
	}
	return out;
}

static string AppendQuery(const string &url, const HeaderList &params) {
	if (params.empty()) {
		return url;
	}
	auto fragment = url.find('#');
	auto base = url.substr(0, fragment);
	auto tail = fragment == string::npos ? string() : url.substr(fragment);
	string sep;
	auto q = base.find('?');
	if (q == string::npos) {
		sep = "?";
	} else if (q != base.size() - 1 && base.back() != '&') {
		sep = "&";
	}
	return base + sep + EncodeForm(params) + tail;
}

template <HttpMethod METHOD>
static void HttpRequestFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &context = state.GetContext();
	const auto count = args.size();
	result.SetVectorType(VectorType::FLAT_VECTOR);
	auto result_data = FlatVector::GetDataMutable<string_t>(result);

	for (idx_t row = 0; row < count; row++) {
		if (context.IsInterrupted()) {
			throw InterruptException();
		}
		auto url_value = args.data[0].GetValue(row);
		if (url_value.IsNull()) {
			FlatVector::SetNull(result, row, true);
			continue;
		}
		HttpRequestSpec spec;
		spec.method = METHOD;
		spec.url = url_value.ToString();
		spec.headers = MapToList(args.data[1].GetValue(row));

		switch (METHOD) {
		case HttpMethod::Get:
			spec.url = AppendQuery(spec.url, MapToList(args.data[2].GetValue(row)));
			break;
		case HttpMethod::Post:
		case HttpMethod::Put: {
			auto body = args.data[2].GetValue(row);
			if (METHOD == HttpMethod::Post) {
				// `params` is the name earlier versions gave the body; it is still accepted
				auto legacy = args.data[3].GetValue(row);
				if (!legacy.IsNull()) {
					if (!body.IsNull()) {
						throw InvalidInputException("http_post: pass the request body as either body or params, "
						                            "not both");
					}
					body = legacy;
				}
			}
			spec.body = body.IsNull() ? string() : body.ToString();
			spec.default_content_type = "application/json";
			break;
		}
		case HttpMethod::PostForm:
			spec.body = EncodeForm(MapToList(args.data[2].GetValue(row)));
			spec.default_content_type = "application/x-www-form-urlencoded";
			break;
		default:
			break;
		}
		auto response = PerformRequest(context, std::move(spec));
		result_data[row] = StringVector::AddString(result, ResultToJson(response, METHOD != HttpMethod::Head));
	}
}

//===--------------------------------------------------------------------===//
// Registration
//===--------------------------------------------------------------------===//

struct HttpParam {
	const char *name;
	LogicalType type;
	bool optional;
};

static void RegisterHttpFunction(ExtensionLoader &loader, const string &name, const vector<HttpParam> &parameters,
                                 scalar_function_t function, const string &description, vector<string> examples) {
	FunctionSignature signature;
	for (auto &param : parameters) {
		if (param.optional) {
			// NULL by default: the parameter can be left out or passed by name
			signature.AddParameter(param.name, param.type, Value(param.type));
		} else {
			signature.AddParameter(param.name, param.type);
		}
	}
	signature.SetReturnType(LogicalType::JSON());
	ScalarFunction fn(Identifier(name), std::move(signature), std::move(function));
	// Every call performs a network request - a side effect. VOLATILE keeps the optimizer from constant-folding
	// a call with constant arguments, which would send the request twice (once at plan time).
	fn.SetVolatile();
	// URL validation, header validation, the external-access check and size limits raise errors.
	fn.SetFallible();
	// NULL headers/params/body mean "none"; only a NULL url gives a NULL result.
	fn.SetNullHandling(FunctionNullHandling::SPECIAL_HANDLING);

	CreateScalarFunctionInfo info(std::move(fn));
	FunctionDescription desc;
	desc.description = description;
	desc.examples = std::move(examples);
	desc.categories = {"http", "network"};
	info.descriptions.push_back(std::move(desc));
	loader.RegisterFunction(std::move(info));
}

static void LoadInternal(ExtensionLoader &loader) {
	auto &config = DBConfig::GetConfig(loader.GetDatabaseInstance());
	config.AddExtensionOption(MAX_RESPONSE_SIZE_SETTING,
	                          "Largest HTTP response body, in bytes, that the http_client functions will accept",
	                          LogicalType::UBIGINT, Value::UBIGINT(DEFAULT_MAX_RESPONSE_SIZE));
	config.AddExtensionOption(MAX_REDIRECTS_SETTING,
	                          "How many redirects the http_client functions follow (0 returns the 3xx response)",
	                          LogicalType::UBIGINT, Value::UBIGINT(DEFAULT_MAX_REDIRECTS));

	const auto map_type = LogicalType::MAP(LogicalType::VARCHAR, LogicalType::VARCHAR);
	const auto json_type = LogicalType::JSON();
	const HttpParam url {"url", LogicalType::VARCHAR, false};
	const HttpParam headers {"headers", map_type, true};
	const HttpParam map_params {"params", map_type, true};
	const HttpParam body {"body", json_type, true};

	const string common = " Returns JSON {status, reason, body, headers}; status is -1 and reason describes the "
	                      "error when no response was received. Uses DuckDB's HTTP stack (http_timeout, http_retries, "
	                      "http_proxy and HTTP secrets apply); refuses when enable_external_access is false.";

	RegisterHttpFunction(loader, "http_head", {url, headers}, HttpRequestFunction<HttpMethod::Head>,
	                     "Sends an HTTP HEAD request. Returns JSON {status, reason, headers} with the response headers "
	                     "as a JSON object (lower-case names). Refuses when enable_external_access is false.",
	                     {"http_head('https://example.com')"});
	RegisterHttpFunction(loader, "http_get", {url, headers, map_params}, HttpRequestFunction<HttpMethod::Get>,
	                     "Sends an HTTP GET request; params are URL-encoded and appended to the query string." +
	                         common,
	                     {"http_get('https://httpbin.org/get')",
	                      "http_get('https://httpbin.org/get', headers => MAP {'Accept': 'application/json'}, params "
	                      "=> MAP {'limit': '10'})"});
	RegisterHttpFunction(loader, "http_post", {url, headers, body, HttpParam {"params", json_type, true}},
	                     HttpRequestFunction<HttpMethod::Post>,
	                     "Sends an HTTP POST request with a JSON body (Content-Type application/json unless a header "
	                     "overrides it). params is a deprecated alias for body." +
	                         common,
	                     {"http_post('https://httpbin.org/post', headers => MAP {'Accept': 'application/json'}, body "
	                      "=> {'name': 'duck'})"});
	RegisterHttpFunction(loader, "http_post_form", {url, headers, map_params},
	                     HttpRequestFunction<HttpMethod::PostForm>,
	                     "Sends an HTTP POST request with an application/x-www-form-urlencoded body built from "
	                     "params." +
	                         common,
	                     {"http_post_form('https://httpbin.org/post', params => MAP {'limit': '10'})"});
	RegisterHttpFunction(loader, "http_put", {url, headers, body}, HttpRequestFunction<HttpMethod::Put>,
	                     "Sends an HTTP PUT request with a JSON body (Content-Type application/json unless a header "
	                     "overrides it)." +
	                         common,
	                     {"http_put('https://httpbin.org/put', body => {'name': 'duck'})"});
	RegisterHttpFunction(loader, "http_delete", {url, headers}, HttpRequestFunction<HttpMethod::Delete>,
	                     "Sends an HTTP DELETE request." + common, {"http_delete('https://httpbin.org/delete')"});

	QueryFarmSendTelemetry(loader, "http_client", HTTP_CLIENT_VERSION);
}

void HttpClientExtension::Load(ExtensionLoader &loader) {
	LoadInternal(loader);
}

std::string HttpClientExtension::Name() {
	return "http_client";
}

std::string HttpClientExtension::Version() const {
	return HTTP_CLIENT_VERSION;
}

} // namespace duckdb

extern "C" {
DUCKDB_CPP_EXTENSION_ENTRY(http_client, loader) {
	duckdb::LoadInternal(loader);
}
}
