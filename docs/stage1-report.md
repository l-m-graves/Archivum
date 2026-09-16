# Stage 1 report: the Drogon spike

Gate from instructions v2, Q4, and rulings v3, section 6. Status per point
below. CI run 9 (commit aa6c9df) is green on all four jobs:
https://github.com/l-m-graves/Archivum/actions/runs/35133533348. Windows
Debug and Release run the same eleven integration tests and the saturation
test as Linux; CTest hides passing-test output, so the saturation numbers
below are from Linux and the Windows numbers will be captured when the
test is run with verbose output on a Windows host.

## Gate status

| # | Requirement | Status |
|---|---|---|
| 1 | TLS server with OpenSSL, statically linked, on Windows, with a certificate from a real issuer | **Partly.** TLS server with OpenSSL statically linked (vcpkg `x64-windows-static-cxx20`) builds and its tests pass on the Windows CI runner with a test-issued certificate. The run with a certificate from your issuer is yours: point `tls.certificate_pem` and `tls.private_key_pem` at it and hit `/healthz`. Blocked on `[FILL: TLS certificate issuer]` |
| 2 | `drogon::HttpClient` fetching the discovery document and JWKS over HTTPS | **Done.** Chain, expiry, and hostname verified; a fetch against an untrusted certificate, or with the wrong trust anchor, fails (test `jwks_fetch_fails_against_untrusted_certificate`). No libcurl, no WinHTTP |
| 3 | Entra-shaped token validated with jwt-cpp end to end against the local test issuer | **Done.** RS256, `iss`, `aud`, `exp`, `nbf`, 120 s skew, `oid` and `tid` required; unknown `kid` triggers one rate-limited refresh; JWKS cached per `Cache-Control: max-age`; expired, future, wrong-audience, wrong-issuer, tampered, rogue-key, no-`oid`, no-`exp` tokens rejected |
| 4 | Event-loop behaviour under a saturating client, reported not tuned | **Done.** Numbers and reading below |
| + | Coroutine handlers link cleanly | **Done.** Both routes are `drogon::Task<HttpResponsePtr>` and the validator is a coroutine; linked and tested on GCC 12 (CI), GCC 13 (local), and MSVC 19.44 (CI) |
| + | `std::format` check | CI step "Feature probes": `ARCHIVUM_HAS_STD_FORMAT=1` on MSVC 19.44; absent on GCC 12 (the CI container and Linux deployment compiler). Not used |
| + | vcpkg manifest with pinned baseline | **Done.** `vcpkg.json` builtin-baseline `1577f17ee57f42a0ef6d75bbb82cb37d0b76d7e8`; CI checks vcpkg out at that commit |

## What was built

- `server/`: configuration loader (strict allow-list keys, fails closed),
  OIDC validator, JSON bridge, app wiring, `archivum serve --config`.
- `tests/server/`: run-time test PKI (RSA keys, self-signed certificates,
  nothing stored in the repository), a local test issuer serving discovery
  and JWKS on the same Drogon app, eleven integration tests, and the
  saturation test.
- Two listeners in the tests: the main one at minimum TLS 1.3, a second at
  minimum 1.2, so the protocol policy is asserted with raw OpenSSL
  handshakes rather than assumed.

## Saturation results

Linux, RelWithDebInfo, 4-core VM, client and server in the same process,
server on 2 IO threads, every request a full `/api/v1/whoami` with an RS256
verification. Warm connections; the connection storm is reported
separately.

| Connections | Client threads | Throughput req/s | p50 ms | p95 ms | p99 ms | max ms | Errors | Health probe max ms during load |
|---|---|---|---|---|---|---|---|---|
| 16 | 2 | 1232 | 12 | 25 | 42 | 61 | 0 | 268 |
| 64 | 2 | 1190 | 51 | 104 | 125 | 193 | 0 | 477 |
| 256 | 2 | 1372 | 183 | 255 | 285 | 462 | 0 | 1190 |
| 512 | 4 | 1369 | 373 | 447 | 535 | 789 | 0 | 2169 |

Connection storm (all connections opened at once, first response measured):
30 to 33 ms per connection at 16 to 256 connections, 49 ms at 512 with four
client threads competing for the same four cores. Recovery: `/healthz`
answered in 64 to 78 ms on a fresh connection within one second of the
load stopping, every time.

### Reading

- **When the loop is backed up, requests queue; nothing is rejected.**
  Throughput is flat at about 1,200 to 1,370 req/s from 16 to 512
  connections, and latency grows linearly with the number of connections
  (roughly 0.7 ms per open connection at p50). No 503s, no resets, no
  timeouts, zero non-200 responses to valid tokens. Drogon has no request
  queue limit by default; the only cap is `setMaxConnectionNum`
  (default 100,000).
- **The health probe shares the IO loops**, so under 512 connections it
  took up to 2.2 s. A monitoring system polling `/healthz` will see
  latency, not failure, when the server is saturated. That is acceptable
  for v1 and worth knowing when setting alert thresholds.
- **The connection storm cost is the client side.** trantor creates a new
  TLS context per client connection (`TcpClient::enableSSL`, loading the
  system trust store each time), about 30 ms here. It also means each
  JWKS refresh costs about 30 ms of setup; refreshes are rare so this is
  fine, but a client library built on Drogon would want a shared context.
- **The numbers are a floor.** Client and server shared four cores and the
  build carried debug info. Payroll load is 10,000 writes a day; this
  server answered 8,000 authenticated requests in six seconds.

## Findings that affect later stages

1. **trantor overrides the TLS policy after applying configuration
   commands, and is patched.** `OpenSSLProvider.cc` in trantor 1.5.28
   applies `SSL_CONF_cmd` options, then unconditionally calls
   `SSL_CTX_set_min_proto_version(TLS1_2)` and
   `SSL_CTX_set_cipher_list("MEDIUM:HIGH:!aNULL:!MD5:!RC4:!3DES")`, so a
   configured `MinProtocol` or `CipherString` was silently discarded. A
   first workaround (`Protocol=-TLSv1.2`, a deprecated OpenSSL command)
   held on Linux but not on Windows in CI run 7, so the root cause is
   fixed instead: the vcpkg overlay port in
   `cmake/vcpkg-overlay-ports/trantor/` carries
   `002-archivum-ssl-conf-commands-win.patch`, which applies trantor's
   defaults first and the configured commands last, and logs any command
   OpenSSL rejects. `tls_policy_min_version_is_enforced` now asserts both
   the 1.3 floor and that the configured TLS 1.2 cipher is the one
   negotiated. The patch is 51 lines against trantor v1.5.28 (upstream
   commit 63a4e5e164e219dc3bf30cdbfa1462ae5602fa97, the submodule of
   drogon v1.9.13); the port's upstream tarball hash is unchanged and the
   port-version is bumped so the binary cache rebuilds. This is the CVE
   rebuild path in miniature: we now own a patch on a dependency, and the
   handbook must say so.
1b. **Drogon drops per-listener SSL commands on Windows, and is patched.**
   `ListenerManager::createListeners` has two branches: on platforms with
   `SO_REUSEPORT` it merges the global and per-listener command lists; on
   the other branch (Windows, a dedicated listening thread) it copies only
   the global list. That is why CI run 8 negotiated TLS 1.2 and the default
   cipher on Windows while Linux passed. The overlay port in
   `cmake/vcpkg-overlay-ports/drogon/` carries
   `0006-archivum-per-listener-ssl-conf-cmds.patch` (five lines against
   drogon v1.9.13, commit 4c5430757ea5451a7c38fbbef4b4bef7dbb47f2f). The
   server itself now sets its one policy through the global
   `setSSLConfigCommands`, which works on both branches unpatched; the
   tests keep a second listener with a different policy so the patched
   path is exercised too.
2. **Startup order.** Drogon runs beginning advices before listeners are
   bound, and the IO loops bind asynchronously. The OIDC initialisation
   retries connection-class failures for up to five seconds, then gives
   up; any other failure is final. Worth knowing for Stage 5's health
   checks.
3. **jsoncpp stays confined.** `server/src/http/json_bridge.cpp` is the
   only file naming `Json::Value`; the CI step "JSON boundary rule" fails
   the build otherwise.
4. **The vcpkg Drogon port auto-detects the C++ standard** and on MSVC's
   default would build the library as C++17. The overlay triplets in
   `cmake/triplets/` build every port as C++20 so the library and this
   code agree.
5. **Windows trust store.** trantor loads the Windows system certificate
   store into OpenSSL for client connections (`loadWindowsSystemCert`),
   so the JWKS fetch to Entra trusts what the machine trusts without a
   bundled CA file. Linux uses OpenSSL's default paths. A production build
   refuses `oidc.ca_bundle_pem` and any loopback or private issuer host.
6. **Drogon's singleton dies at exit.** Calling `quit()` from a static
   destructor crashes inside Drogon; tests stop the app explicitly. Stage
   5's service wrapper must stop the app before `main` returns.

## Is the C++ web stack comfortable to work in?

Yes, with the reservations above. Coroutine handlers read as linear code
and the validator's fetch-then-verify path is exactly the case you wanted
them for. The costs are trantor's TLS policy overrides and the client-side
per-connection context, both fixable with small patches under our
control. Nothing here argues for changing course.

## How to run against your certificate (point 1)

1. Put the certificate chain and key on the host.
2. Copy `config/archivum.example.json`, set `tls.certificate_pem`,
   `tls.private_key_pem`, and the `oidc` block for a real tenant, or leave
   the test issuer to the tests.
3. `archivum serve --config <path>` and open `https://host:8443/healthz`.
   The response includes `days_remaining` for the certificate.
