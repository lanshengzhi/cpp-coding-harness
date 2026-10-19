# TLS test credentials (issue #638)

Committed credentials secure the local HTTPS mocks used by the coding-agent
MCP HTTP and OAuth session tests:

- `test-ca.pem` — test CA certificate loaded by the test process through
  `SSL_CERT_FILE`.
- `test-server.pem` / `test-server-key.pem` — mock server certificate and
  private key (SAN: IP 127.0.0.1, DNS localhost).

The CA private key is deliberately not committed. Regeneration creates a
fresh set. Regenerate everything with OpenSSL 3.x:

```sh
openssl req -x509 -newkey rsa:2048 -keyout test-ca-key.pem -out test-ca.pem \
    -days 3650 -nodes -subj "/CN=cch-test-ca" \
    -addext "basicConstraints=critical,CA:TRUE" \
    -addext "keyUsage=critical,keyCertSign,cRLSign"
openssl req -newkey rsa:2048 -keyout test-server-key.pem \
    -out test-server.csr -nodes -subj "/CN=127.0.0.1"
printf '%s\n' \
    "subjectAltName=IP:127.0.0.1,DNS:localhost" \
    "basicConstraints=critical,CA:FALSE" \
    "keyUsage=critical,digitalSignature,keyEncipherment" \
    "extendedKeyUsage=serverAuth" > test-server.ext
openssl x509 -req -in test-server.csr -CA test-ca.pem -CAkey test-ca-key.pem \
    -CAcreateserial -out test-server.pem -days 3650 -extfile test-server.ext
rm test-server.csr test-server.ext test-ca.srl test-ca-key.pem
```
