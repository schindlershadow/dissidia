// Host test for the HTTP/TLS client (psp_http) against a local server (tests/http_test_server.py, run by run_http.sh):
//   test_http trusted <http_port> <https_port>     PSPRECOMP_CA_FILE points at the server's self-signed certificate
//   test_http untrusted <http_port> <https_port>   no extra CA: https must be refused
#include "psp_http.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>

using namespace psp_http;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { ++g_fail; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static Request get(const std::string& url) { Request r; r.url = url; return r; }

int main(int argc, char** argv) {
    if (argc < 4) return 2;
    const std::string mode = argv[1];
    const std::string H = std::string("http://127.0.0.1:") + argv[2];
    const std::string S = std::string("https://localhost:") + argv[3];
    const std::string SIP = std::string("https://127.0.0.1:") + argv[3];

    // pure helpers
    CHECK(url_encode("a b/c~d&é") == "a%20b%2Fc~d%26%C3%A9");
    CHECK(form_encode({{"a", "1 2"}, {"b", "x&y"}}) == "a=1%202&b=x%26y");
    CHECK(base64("") == "" && base64("f") == "Zg==" && base64("fo") == "Zm8=" && base64("foo") == "Zm9v" && base64("foob") == "Zm9vYg==");
    {
        Url u = parse_url("https://example.com/a/b?x=1#frag");
        CHECK(u.valid && u.scheme == "https" && u.host == "example.com" && u.port == 443 && u.path == "/a/b?x=1");
        u = parse_url("http://h:8080");
        CHECK(u.valid && u.port == 8080 && u.path == "/");
        u = parse_url("http://[::1]:99/z");
        CHECK(u.valid && u.host == "::1" && u.port == 99);
        CHECK(!parse_url("ftp://x/").valid && !parse_url("http://u:p@h/").valid && !parse_url("nonsense").valid && !parse_url("http://h:0/").valid);
    }
    CHECK(tls_available());

    Response r = perform(get(H + "/hello"));
    CHECK(r.ok() && r.text() == "hello" && r.header("content-length") == "5");
    r = perform(get(H + "/404"));
    CHECK(r.status == 404 && r.text() == "nope" && !r.ok() && r.error.empty());

    if (mode == "untrusted") {
        r = perform(get(S + "/hello"));
        CHECK(r.status == 0 && r.error.find("handshake") != std::string::npos);
        std::printf("untrusted: %s\n", r.error.c_str());
    } else {
        r = perform(get(S + "/hello"));
        CHECK(r.ok() && r.text() == "hello");
        if (!r.ok()) std::printf("  error: %s\n", r.error.c_str());
        // host name mismatch (the certificate is for localhost only)
        r = perform(get(SIP + "/hello"));
        CHECK(r.status == 0 && r.error.find("handshake") != std::string::npos);
        std::printf("name mismatch: %s\n", r.error.c_str());

        Request p; p.method = "POST"; p.url = S + "/echo";
        p.headers = {{"Content-Type", "application/json"}, {"Authorization", "Bearer abc"}};
        p.body = Bytes{'{', '"', 'a', '"', ':', '1', '}'};
        r = perform(p);
        CHECK(r.ok() && r.text() == "{\"a\":1}" && r.header("X-Method") == "POST" && r.header("x-auth") == "Bearer abc" &&
              r.header("X-Type") == "application/json");
        p.method = "PUT"; p.body.assign(100000, 'z');
        r = perform(p);
        CHECK(r.ok() && r.body.size() == 100000 && r.header("X-Method") == "PUT");
        p.method = "PROPFIND"; p.body.clear();
        r = perform(p);
        CHECK(r.ok() && r.header("X-Method") == "PROPFIND");
    }

    // framing (plain and TLS): chunked, close-delimited, large, truncated
    for (const std::string& base : {H, mode == "trusted" ? S : H}) {
        r = perform(get(base + "/chunked"));
        CHECK(r.ok() && r.text() == "chunk1-chunk2-" + std::string(5000, 'x'));
        r = perform(get(base + "/noclen"));
        CHECK(r.ok() && r.text() == "until-close");
        r = perform(get(base + "/big"));
        CHECK(r.ok() && r.body.size() == 3u * 1024 * 1024 && r.body[1000] == uint8_t((1000 * 31) & 255));
        r = perform(get(base + "/trunc"));
        CHECK(r.status == 0 && !r.error.empty());
        Request big = get(base + "/big"); big.max_body = 1000;
        r = perform(big);
        CHECK(r.status == 0 && r.error.find("large") != std::string::npos);
    }

    // redirects
    r = perform(get(H + "/redir"));
    CHECK(r.status == 302 && r.header("Location") == "/hello");
    Request rd = get(H + "/redir"); rd.max_redirects = 2;
    r = perform(rd);
    CHECK(r.ok() && r.text() == "hello");
    Request p7 = get(H + "/redir307"); p7.method = "POST"; p7.body = {'k'}; p7.max_redirects = 2;
    r = perform(p7);
    CHECK(r.ok() && r.header("X-Method") == "POST" && r.text() == "k");           // 307 keeps method and body
    Request px = get(H + "/redir-x"); px.headers = {{"Authorization", "secret"}}; px.max_redirects = 2;
    px.url = H + "/redir-x";
    r = perform(px);
    CHECK(r.ok() && r.header("X-Auth").empty());                                   // dropped: different host name

    // timeouts and refused connections
    {
        Request s = get(H + "/slow"); s.timeout_ms = 500;
        const auto t0 = std::chrono::steady_clock::now();
        r = perform(s);
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
        CHECK(r.status == 0 && ms < 2500);
        r = perform(get("http://127.0.0.1:1/"));
        CHECK(r.status == 0 && !r.error.empty());
        r = perform(get("http://no-such-host.invalid/"));
        CHECK(r.status == 0 && r.error.find("resolve") != std::string::npos);
        r = perform(get("not a url"));
        CHECK(r.status == 0 && r.error.find("bad URL") != std::string::npos);
        Request hi = get(H + "/hello"); hi.headers = {{"X", "a\r\nInjected: 1"}};
        r = perform(hi);
        CHECK(r.status == 0 && r.error == "bad header");
    }

    if (g_fail) { std::printf("%d check(s) failed\n", g_fail); return 1; }
    std::printf("http (%s): all checks passed\n", mode.c_str());
    return 0;
}
