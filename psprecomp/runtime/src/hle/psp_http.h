// A small blocking HTTP/1.1 client (http and https) for cloud sync: one request per connection, TLS through mbedTLS with
// the embedded Mozilla CA bundle (assets/ca) and full certificate + host name verification. Pure library: no SDL, no
// game state; call it from a worker thread (every call blocks up to its timeouts).
//
// Built without mbedTLS (PSPRECOMP_HAVE_MBEDTLS undefined) it still does plain http, and https requests fail with a clear
// error, so the rest of the runtime builds either way. `PSPRECOMP_CA_FILE=<pem>` adds extra trusted roots (a private
// server's own CA, the tests).
#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace psp_http {

using Bytes = std::vector<uint8_t>;
using Headers = std::vector<std::pair<std::string, std::string>>;

struct Request {
    std::string method = "GET";
    std::string url;                 // http://host[:port]/path?query  or  https://...
    Headers headers;                 // Host, Content-Length, Connection and User-Agent are added
    Bytes body;
    int timeout_ms = 20000;          // per connect / read / write step, not for the whole transfer
    int max_redirects = 0;           // follow 301/302/303/307/308 this many times (Authorization is dropped when the host changes)
    size_t max_body = 64u << 20;     // a larger response is an error
};

struct Response {
    int status = 0;                  // 0 = the request did not complete: see `error`
    Headers headers;                 // names as sent
    Bytes body;
    std::string error;
    bool ok() const { return status >= 200 && status < 300; }
    // First header with this name (case-insensitive), "" when absent.
    std::string header(const std::string& name) const;
    std::string text() const { return std::string(body.begin(), body.end()); }
};

Response perform(const Request& r);

bool tls_available();                // compiled with mbedTLS

// ---- small helpers the providers share ----
std::string url_encode(const std::string& s);          // percent-encodes everything but A-Z a-z 0-9 - . _ ~
std::string form_encode(const std::vector<std::pair<std::string, std::string>>& kv);   // k=v&k=v, both encoded
std::string base64(const Bytes& data);
std::string base64(const std::string& s);

struct Url {
    std::string scheme, host, path;  // path includes the query, always starts with '/'
    int port = 0;
    bool valid = false;
};
Url parse_url(const std::string& url);

}  // namespace psp_http
