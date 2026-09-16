// The one place where Drogon's Json::Value meets nlohmann::json.
// See docs/json-boundary.md. No other translation unit may include
// <json/json.h> or name Json::Value.
#pragma once

#include <string>

#include <drogon/HttpResponse.h>
#include <nlohmann/json.hpp>

namespace archivum::server {

// Builds an application/json response from the internal representation.
drogon::HttpResponsePtr json_response(const nlohmann::json& body, drogon::HttpStatusCode status);

// Converts a body Drogon parsed into the internal representation. Returns
// false if the request carried no valid JSON object.
bool json_from_request(const drogon::HttpRequestPtr& req, nlohmann::json& out);

}  // namespace archivum::server
