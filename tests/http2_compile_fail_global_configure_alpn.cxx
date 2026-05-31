#include <openssl/ssl.h>

import conflux.net.http2;

int main() {
	http2_configure_alpn(static_cast<SSL_CTX *>(nullptr));
}
