#include <openssl/ssl.h>

import conflux.net.http2;

int main() {
	return http2_negotiated(static_cast<SSL const *>(nullptr)) ? 0 : 1;
}
