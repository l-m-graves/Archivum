#include "json_bridge.h"

#include <json/json.h>

namespace archivum::server {

drogon::HttpResponsePtr json_response(const nlohmann::json& body, drogon::HttpStatusCode status) {
  auto resp = drogon::HttpResponse::newHttpResponse();
  resp->setStatusCode(status);
  resp->setContentTypeCode(drogon::CT_APPLICATION_JSON);
  resp->setBody(body.dump());
  return resp;
}

bool json_from_request(const drogon::HttpRequestPtr& req, nlohmann::json& out) {
  // Drogon already parsed the body with jsoncpp. Re-serialising it is the
  // simplest exact conversion and keeps jsoncpp confined to this file.
  const std::shared_ptr<Json::Value> value = req->getJsonObject();
  if (!value || !value->isObject()) return false;
  Json::StreamWriterBuilder writer;
  writer["indentation"] = "";
  const std::string text = Json::writeString(writer, *value);
  out = nlohmann::json::parse(text, nullptr, false);
  return out.is_object();
}

}  // namespace archivum::server
