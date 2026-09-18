#include "request.h"

#include "../http/json_bridge.h"
#include "archivum/server/log.h"

namespace archivum::server {
namespace {

bool contains_key(const nlohmann::json& j, const std::string& key, std::string& where) {
  if (j.is_object()) {
    for (auto& [k, v] : j.items()) {
      if (k == key) {
        where = k;
        return true;
      }
      if (contains_key(v, key, where)) {
        where = k + "." + where;
        return true;
      }
    }
  } else if (j.is_array()) {
    for (const auto& v : j) {
      if (contains_key(v, key, where)) return true;
    }
  }
  return false;
}

}  // namespace

Result<nlohmann::json> parse_body(const drogon::HttpRequestPtr& req, const std::set<std::string>& allowed_keys) {
  nlohmann::json body;
  if (req->body().empty()) {
    body = nlohmann::json::object();
  } else if (!json_from_request(req, body)) {
    return Status::invalid_argument("body must be a JSON object");
  }
  std::string where;
  if (contains_key(body, "employee_id", where)) {
    return Status::invalid_argument("employee_id is not accepted in a request body (at " + where +
                                    "); the server derives the employee from the credential");
  }
  for (auto& [k, v] : body.items()) {
    if (allowed_keys.count(k) == 0) return Status::invalid_argument("unknown key '" + k + "'");
  }
  return body;
}

drogon::HttpResponsePtr error_response(int http_status, const std::string& code, const std::string& message,
                                       const std::string& request_id) {
  nlohmann::json j;
  j["error"] = code;
  j["message"] = message;
  j["request_id"] = request_id;
  auto resp = json_response(j, static_cast<drogon::HttpStatusCode>(http_status));
  resp->addHeader("X-Request-Id", request_id);
  return resp;
}

drogon::HttpResponsePtr ok_response(nlohmann::json body, const std::string& request_id, int http_status) {
  body["request_id"] = request_id;
  auto resp = json_response(body, static_cast<drogon::HttpStatusCode>(http_status));
  resp->addHeader("X-Request-Id", request_id);
  return resp;
}

drogon::HttpResponsePtr status_response(const Status& s, const std::string& request_id) {
  switch (s.code()) {
    case ErrorCode::Constraint:
      return error_response(409, "constraint", s.message(), request_id);
    case ErrorCode::AlreadyExists:
      return error_response(409, "already_exists", s.message(), request_id);
    case ErrorCode::NotFound:
      return error_response(404, "not_found", s.message(), request_id);
    case ErrorCode::InvalidArgument:
      return error_response(400, "invalid", s.message(), request_id);
    default:
      log(LogLevel::Error, "request.failed", {{"request_id", request_id}, {"error", s.to_string()}});
      return error_response(500, "internal", "request failed; see the log for this request id", request_id);
  }
}

Result<std::string> body_string(const nlohmann::json& body, const char* key, bool required) {
  if (!body.contains(key)) {
    if (required) return Status::invalid_argument(std::string(key) + " is required");
    return std::string();
  }
  if (!body[key].is_string()) return Status::invalid_argument(std::string(key) + " must be a string");
  const std::string v = body[key].get<std::string>();
  if (required && v.empty()) return Status::invalid_argument(std::string(key) + " is empty");
  if (v.size() > 4096) return Status::invalid_argument(std::string(key) + " is too long");
  return v;
}

Result<std::int64_t> body_int(const nlohmann::json& body, const char* key, bool required) {
  if (!body.contains(key)) {
    if (required) return Status::invalid_argument(std::string(key) + " is required");
    return std::int64_t(0);
  }
  if (!body[key].is_number_integer()) return Status::invalid_argument(std::string(key) + " must be an integer");
  return body[key].get<std::int64_t>();
}

}  // namespace archivum::server
