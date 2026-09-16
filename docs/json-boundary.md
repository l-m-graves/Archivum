# JSON boundary rule

Decided in instructions v2, Q4.

- **Inside the server, JSON is `nlohmann::json`.** Request handlers, module
  code, the query service, configuration, and every test speak
  `nlohmann::json`.
- **`Json::Value` (jsoncpp) exists only because Drogon uses it.** It is
  converted to and from `nlohmann::json` in exactly one place: the HTTP
  boundary layer in `server/src/http/json_bridge.cpp`. No other translation unit includes `<json/json.h>`.
- Every endpoint declares an allow-list schema. Parsing is strict: unknown
  fields, wrong types, and out-of-range lengths are rejected with 400 before
  any handler code runs. This is the structural replacement for the
  substring parsing found in the Punchline server review.
- The CI step "JSON boundary rule" fails the build if `json/json.h` or
  `Json::Value` appears outside `server/src/http/json_bridge.*`.
