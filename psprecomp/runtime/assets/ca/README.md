# CA root certificates

`cacert.pem` is the Mozilla CA root bundle as converted by the curl project (https://curl.se/docs/caextract.html, "Certificate
data from Mozilla as of: Thu Nov  5 04:12:01 2026 GMT"). The runtime embeds it (`embed_cacert.cmake`) and uses it to verify the
servers cloud sync talks to (psp_http.cpp), so verification works the same on Windows, Linux and Android and does not depend on
the system store. Licence: MPL-2.0 (the Mozilla data).

Update: download the file again from the URL above, replace it, update the date here. A stale bundle only matters when a
provider moves to a CA that is not in it (or a root is distrusted). `PSPRECOMP_CA_FILE=<pem>` adds extra trusted roots at run
time (a private server's own CA, tests).
