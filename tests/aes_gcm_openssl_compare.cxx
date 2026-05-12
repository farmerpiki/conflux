// Standalone comparison: conflux AES-256-GCM vs OpenSSL EVP.
// Build: clang++ -std=c++26 -O2 -o aes_gcm_cmp tests/aes_gcm_openssl_compare.cxx -lssl -lcrypto
// Not part of the normal test suite — requires OpenSSL headers.
#include <openssl/evp.h>
#include <sys/random.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <span>
#include <vector>

import std;
import conflux.types;
import conflux.crypto;
static bool openssl_encrypt(
	span<unsigned char const> key,
	span<unsigned char const> iv,
	span<unsigned char const> pt,
	span<unsigned char const> aad,
	V<unsigned char> &out) {
	EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
	if (!ctx) {
		return false;
	}
	EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, key.data(), iv.data());

	int len = 0;
	if (!aad.empty()) {
		EVP_EncryptUpdate(ctx, nullptr, &len, aad.data(), static_cast<int>(aad.size()));
	}

	out.resize(pt.size() + 16);
	EVP_EncryptUpdate(ctx, out.data(), &len, pt.data(), static_cast<int>(pt.size()));
	int ct_len = len;
	EVP_EncryptFinal_ex(ctx, out.data() + ct_len, &len);
	ct_len += len;

	EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, 16, out.data() + ct_len);
	out.resize(static_cast<SZ>(ct_len) + 16);
	EVP_CIPHER_CTX_free(ctx);
	return true;
}
static bool openssl_decrypt(
	span<unsigned char const> key,
	span<unsigned char const> iv,
	span<unsigned char const> ct_and_tag,
	span<unsigned char const> aad,
	V<unsigned char> &out) {
	if (ct_and_tag.size() < 16) {
		return false;
	}
	SZ ct_len = ct_and_tag.size() - 16;
	auto ct = ct_and_tag.subspan(0, ct_len);
	auto tag = ct_and_tag.subspan(ct_len, 16);

	EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
	if (!ctx) {
		return false;
	}
	EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, key.data(), iv.data());

	int len = 0;
	if (!aad.empty()) {
		EVP_DecryptUpdate(ctx, nullptr, &len, aad.data(), static_cast<int>(aad.size()));
	}

	out.resize(ct_len);
	EVP_DecryptUpdate(ctx, out.data(), &len, ct.data(), static_cast<int>(ct_len));
	int pt_len = len;

	EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, 16, const_cast<unsigned char *>(tag.data()));
	int ok = EVP_DecryptFinal_ex(ctx, out.data() + pt_len, &len);
	pt_len += len;
	out.resize(static_cast<SZ>(pt_len));
	EVP_CIPHER_CTX_free(ctx);
	return ok == 1;
}
int main() {
	using namespace std::chrono;

	A<unsigned char, 32> key{};
	A<unsigned char, 12> iv{};
	getrandom(key.data(), key.size(), 0);
	getrandom(iv.data(), iv.size(), 0);

	// Test correctness: compare outputs for various sizes
	std::printf("=== Correctness comparison ===\n");
	bool all_ok = true;
	for (SZ sz: A<SZ, 10>{0, 1, 15, 16, 17, 64, 256, 1024, 4096, 65536}) {
		V<unsigned char> pt(sz);
		getrandom(pt.data(), pt.size(), 0);

		V<unsigned char> aad(sz > 64 ? 32 : 0);
		if (!aad.empty()) {
			getrandom(aad.data(), aad.size(), 0);
		}

		// Encrypt with both
		V<unsigned char> ossl_ct;
		openssl_encrypt(key, iv, pt, aad, ossl_ct);

		auto conflux_ct = aes_gcm_encrypt(key, iv, pt, aad);
		if (!conflux_ct.has_value()) {
			std::printf("  FAIL sz=%zu: conflux encrypt error\n", sz);
			all_ok = false;
			continue;
		}

		if (ossl_ct.size() != conflux_ct->size()
			|| std::memcmp(ossl_ct.data(), conflux_ct->data(), ossl_ct.size()) != 0) {
			std::printf(
				"  FAIL sz=%zu: ciphertext mismatch (ossl=%zu, conflux=%zu)\n",
				sz,
				ossl_ct.size(),
				conflux_ct->size());
			all_ok = false;
			continue;
		}

		// Decrypt with both (cross-verify)
		V<unsigned char> ossl_pt;
		bool ossl_ok = openssl_decrypt(key, iv, ossl_ct, aad, ossl_pt);

		auto conflux_pt = aes_gcm_decrypt(key, iv, *conflux_ct, aad);
		if (!ossl_ok || !conflux_pt.has_value()) {
			std::printf("  FAIL sz=%zu: decrypt error\n", sz);
			all_ok = false;
			continue;
		}

		if (ossl_pt != *conflux_pt || *conflux_pt != pt) {
			std::printf("  FAIL sz=%zu: plaintext mismatch\n", sz);
			all_ok = false;
			continue;
		}

		std::printf("  OK sz=%zu\n", sz);
	}

	if (!all_ok) {
		std::printf("\nSome tests FAILED!\n");
		return 1;
	}
	std::printf("\nAll correctness tests passed.\n\n");

	// Speed comparison
	std::printf("=== Speed comparison (encrypt 4096 bytes, 10000 iterations) ===\n");
	constexpr SZ kBenchSize = 4096;
	constexpr int kIters = 10000;
	V<unsigned char> bench_pt(kBenchSize);
	getrandom(bench_pt.data(), bench_pt.size(), 0);

	// Warmup
	for (int i = 0; i < 100; ++i) {
		(void)aes_gcm_encrypt(key, iv, bench_pt, {});
	}

	// Conflux
	auto t0 = steady_clock::now();
	for (int i = 0; i < kIters; ++i) {
		auto r = aes_gcm_encrypt(key, iv, bench_pt, {});
		asm volatile("" ::"r"(r->data()));
	}
	auto t1 = steady_clock::now();
	double conflux_ns = static_cast<double>(duration_cast<nanoseconds>(t1 - t0).count());

	// OpenSSL warmup
	V<unsigned char> ossl_out;
	for (int i = 0; i < 100; ++i) {
		openssl_encrypt(key, iv, bench_pt, {}, ossl_out);
	}

	// OpenSSL
	auto t2 = steady_clock::now();
	for (int i = 0; i < kIters; ++i) {
		openssl_encrypt(key, iv, bench_pt, {}, ossl_out);
		asm volatile("" ::"r"(ossl_out.data()));
	}
	auto t3 = steady_clock::now();
	double ossl_ns = static_cast<double>(duration_cast<nanoseconds>(t3 - t2).count());

	double conflux_mbps = (static_cast<double>(kBenchSize) * kIters) / (conflux_ns / 1e9) / 1e6;
	double ossl_mbps = (static_cast<double>(kBenchSize) * kIters) / (ossl_ns / 1e9) / 1e6;

	std::printf("  conflux: %.1f MB/s (%.0f ns/op)\n", conflux_mbps, conflux_ns / kIters);
	std::printf("  openssl: %.1f MB/s (%.0f ns/op)\n", ossl_mbps, ossl_ns / kIters);
	std::printf("  ratio:   %.2fx (openssl/conflux)\n", ossl_mbps / conflux_mbps);

	return 0;
}
