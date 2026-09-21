// Request parsing and response helpers shared by every route.
#pragma once

#include <set>
#include <string>

#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <nlohmann/json.hpp>

#include "archivum/status.h"

namespace archivum::server {

// Parses a JSON object body and applies the contract rules: unknown keys
// are refused, and the key `employee_id` is refused wherever it appears
// (the server derives the employee from the credential or the enrollment
// record; a request never asserts it). Both are 400 with a message naming
// the offending key, never silently ignored.
Result<nlohmann::json> parse_body(const drogon::HttpRequestPtr& req, const std::set<std::string>& allowed_keys);

// True when `s` is parse_body's refusal of `employee_id`; `rejected_key_path`
// gives the path it was found at ("meta.employee_id"). The Punchline routes
// use both to log the tamper signal (Stage 6 ruling).
bool is_employee_id_rejection(const Status& s);
std::string rejected_key_path(const Status& s);

// A JSON error response: {"error": code, "message": message, "request_id": id}.
drogon::HttpResponsePtr error_response(int http_status, const std::string& code, const std::string& message,
                                       const std::string& request_id);
drogon::HttpResponsePtr ok_response(nlohmann::json body, const std::string& request_id, int http_status = 200);

// Maps an engine or module status to a response: Constraint -> 409,
// NotFound -> 404, InvalidArgument -> 400, AlreadyExists -> 409, else 500
// (logged with the request id; the message is not returned).
drogon::HttpResponsePtr status_response(const Status& s, const std::string& request_id);

// Reads a required string, or a required integer, out of a parsed body.
Result<std::string> body_string(const nlohmann::json& body, const char* key, bool required = true);
Result<std::int64_t> body_int(const nlohmann::json& body, const char* key, bool required = true);

}  // namespace archivum::server
