import std;
import conflux.types;
import conflux.crypto;
import conflux.utils;
import bench_common;
namespace {

template<typename F>
BenchStats measure(
	F &&fn,
	std::size_t warmup,
	std::size_t iters,
	std::size_t batch = 1,
	std::size_t bytes = 0) {
	iters = std::max(iters, std::size_t{1});
	batch = std::max(batch, std::size_t{1});
	BenchSamplePlan const plan = bench_sample_plan(iters * batch, warmup * batch);
	return bench_measure_batched(std::forward<F>(fn), plan, bytes);
}

bool g_json = false;
bool g_first = true;
void emit(
	std::string_view name,
	BenchStats s) {
	s.variant = name;
	if (g_json) {
		bench_print(s, true, g_first);
		g_first = false;
	} else if (s.throughput > 0.0) {
		print("[crypto-bench] {:<40} {:>10.1f} ns  {:>8.1f} MB/s\n", name, s.ns_per_iter, s.throughput);
	} else {
		print("[crypto-bench] {:<40} {:>10.1f} ns\n", name, s.ns_per_iter);
	}
}

} // namespace
int main(
	int argc,
	char **argv) {
	bench_info_if_requested(
		argc,
		argv,
		R"({"name":"crypto","parser":"standard","configs":[{"name":"default","extra":{},"target_ms":500,"max_iterations":1000000,"calibration_iterations":16,"args":["--iterations","0","--warmup","0"]}]})");
	auto const cfg = bench_parse_args(std::span{argv, static_cast<std::size_t>(argc)});
	g_json = cfg.json_out;

	std::array<unsigned char, 32> key{};
	std::array<unsigned char, 12> iv{};
	std::array<unsigned char, 16> aad{};
	conflux::utils::crypto_random_bytes(key);
	conflux::utils::crypto_random_bytes(iv);
	conflux::utils::crypto_random_bytes(aad);

	for (std::size_t sz: {64UZ, 256UZ, 1024UZ, 4096UZ, 16384UZ, 65536UZ}) {
		std::vector<unsigned char> pt(sz);
		conflux::utils::crypto_random_bytes(pt);

		auto enc_name = std::format("gcm_encrypt/{}", sz);
		emit(
			enc_name,
			measure(
				[&] {
					auto ct = conflux::crypto::aes_gcm_encrypt(key, iv, pt, aad).value();
					asm volatile("" : : "r"(ct.data()) : "memory");
				},
				cfg.warmup / 10,
				cfg.iterations / 10,
				1,
				sz));

		auto ct = conflux::crypto::aes_gcm_encrypt(key, iv, pt, aad).value();
		auto dec_name = std::format("gcm_decrypt/{}", sz);
		emit(
			dec_name,
			measure(
				[&] {
					auto r = conflux::crypto::aes_gcm_decrypt(key, iv, ct, aad).value();
					asm volatile("" : : "r"(r.data()) : "memory");
				},
				cfg.warmup / 10,
				cfg.iterations / 10,
				1,
				sz));
	}

	for (std::size_t sz: {16UZ, 64UZ, 256UZ, 1024UZ, 4096UZ}) {
		std::vector<unsigned char> data(sz);
		conflux::utils::crypto_random_bytes(data);

		auto name = std::format("conflux::utils::hex_encode/{}", sz);
		emit(
			name,
			measure(
				[&] {
					auto h = conflux::utils::hex_encode(data);
					asm volatile("" : : "r"(h.data()) : "memory");
				},
				cfg.warmup,
				cfg.iterations,
				1,
				sz));
	}

	for (std::size_t sz: {32UZ, 64UZ, 256UZ}) {
		std::vector<unsigned char> msg(sz);
		conflux::utils::crypto_random_bytes(msg);

		auto name = std::format("sha256/{}", sz);
		emit(
			name,
			measure(
				[&] {
					auto h = conflux::crypto::sha256(msg);
					asm volatile("" : : "r"(h.data()) : "memory");
				},
				cfg.warmup / 10,
				cfg.iterations / 10));

		auto hmac_name = std::format("hmac_sha256/{}", sz);
		emit(
			hmac_name,
			measure(
				[&] {
					auto h = conflux::crypto::hmac_sha256(key, msg);
					asm volatile("" : : "r"(h.data()) : "memory");
				},
				cfg.warmup / 10,
				cfg.iterations / 10));
	}

	std::string_view a = "this is a constant time comparison test string!";
	std::string_view b = "this is a constant time comparison test string!";
	emit(
		"constant_time_eq/48",
		measure(
			[&] {
				bool r = conflux::crypto::constant_time_eq(a, b);
				asm volatile("" : : "r"(r) : "memory");
			},
			cfg.warmup,
			cfg.iterations,
			50));

	for (std::size_t sz: {32UZ, 128UZ, 512UZ, 4096UZ}) {
		std::vector<char> buf(sz, 'X');
		auto lower_name = std::format("ascii_lower/{}", sz);
		emit(
			lower_name,
			measure(
				[&] {
					for (auto &c: buf) {
						c = 'X';
					}
					asm volatile("" : "+m"(*buf.data()));
					conflux::utils::ascii_lower_inplace(std::span{buf});
					asm volatile("" : : "r"(buf.data()) : "memory");
				},
				cfg.warmup,
				cfg.iterations,
				10,
				sz));
	}

	std::string const url_plain = "https://example.com/api/v1/users/12345/profile?format=json&lang=en";
	emit(
		"conflux::utils::url_decode/plain_65",
		measure(
			[&] {
				auto r = conflux::utils::url_decode(url_plain);
				asm volatile("" : : "r"(r.data()) : "memory");
			},
			cfg.warmup,
			cfg.iterations,
			10));

	std::string const url_encoded = "key1%3Dval1%26key2%3Dval2%26key3%3Dval3%26key4%3Dval4%26key5%3Dval5";
	emit(
		"conflux::utils::url_decode/encoded_67",
		measure(
			[&] {
				auto r = conflux::utils::url_decode(url_encoded);
				asm volatile("" : : "r"(r.data()) : "memory");
			},
			cfg.warmup,
			cfg.iterations,
			10));
}
