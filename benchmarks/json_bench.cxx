import std;
import conflux.json;

using namespace std;
using namespace conflux::json;

namespace {

// ---------------------------------------------------------------------------
// Timing helpers
// ---------------------------------------------------------------------------

struct Stats {
	double median_ns;
	double throughput_mbs; // 0 if not a throughput benchmark
};

template<typename F>
Stats measure(
	F &&fn,
	size_t warmup,
	size_t iters,
	size_t bytes = 0) {
	for (size_t i = 0; i < warmup; ++i) {
		fn();
	}
	vector<double> samples;
	samples.reserve(iters);
	for (size_t i = 0; i < iters; ++i) {
		auto t0 = chrono::steady_clock::now();
		fn();
		auto t1 = chrono::steady_clock::now();
		samples.push_back(static_cast<double>(chrono::duration_cast<chrono::nanoseconds>(t1 - t0).count()));
	}
	sort(samples.begin(), samples.end());
	double const med = samples[iters / 2];
	double const mbs = (bytes > 0 && med > 0.0) ? static_cast<double>(bytes) / (med / 1e9) / (1024.0 * 1024.0) : 0.0;
	return {med, mbs};
}

void print(
	string_view name,
	Stats const &s) {
	if (s.throughput_mbs > 0.0) {
		print("[json-bench] {:<40} {:>10.1f} ns  {:>8.1f} MB/s\n", name, s.median_ns, s.throughput_mbs);
	} else {
		print("[json-bench] {:<40} {:>10.1f} ns\n", name, s.median_ns);
	}
}

// ---------------------------------------------------------------------------
// Corpus builders
// ---------------------------------------------------------------------------

// Typical config corpus: ~4 KB flat object with string/number/bool values.
string make_config_corpus() {
	string out;
	out.reserve(4096);
	out += '{';
	for (int i = 0; i < 64; ++i) {
		if (i > 0) {
			out += ',';
		}
		out += format(
			R"("key_{}":{{"value":{},"label":"item_{}","active":{}}})",
			i,
			i * 17,
			i,
			(i % 2 == 0) ? "true" : "false");
	}
	out += '}';
	return out;
}

// Struct-decode corpus: array of objects with string_view-compatible fields.
string make_decode_corpus() {
	string out;
	out.reserve(8192);
	out += '[';
	for (int i = 0; i < 200; ++i) {
		if (i > 0) {
			out += ',';
		}
		out += format(R"({{"id":{},"name":"user_{}","score":{}}})", i, i, i * 3.14);
	}
	out += ']';
	return out;
}

// Lookup corpus: object with 1024 members.
string make_lookup_corpus() {
	string out;
	out.reserve(32768);
	out += '{';
	for (int i = 0; i < 1024; ++i) {
		if (i > 0) {
			out += ',';
		}
		out += format(R"("member_{}":{},)", i, i); // extra comma intentional — remove after
		out.pop_back();
	}
	out += '}';
	return out;
}

// Array traversal corpus: array of 10000 numbers.
string make_array_corpus() {
	string out;
	out.reserve(65536);
	out += '[';
	for (int i = 0; i < 10000; ++i) {
		if (i > 0) {
			out += ',';
		}
		out += to_string(i);
	}
	out += ']';
	return out;
}

// Large corpus for parse throughput gate: ~1 MB nested structure.
string make_large_corpus() {
	string out;
	out.reserve(1024UZ * 1024UZ);
	out += '[';
	for (int i = 0; i < 2000; ++i) {
		if (i > 0) {
			out += ',';
		}
		out += format(
			R"({{"id":{},"name":"entry_{}","tags":["alpha","beta","gamma"],"meta":{{"score":{},"active":{}}}}})",
			i,
			i,
			i * 1.5,
			(i % 2 == 0) ? "true" : "false");
	}
	out += ']';
	return out;
}

// ---------------------------------------------------------------------------
// Benchmark drivers
// ---------------------------------------------------------------------------

void bench_parse_small(
	string const &corpus) {
	auto s = measure([&] { (void)parse(corpus); }, 50, 500, corpus.size());
	print("parse/small (~4KB config)", s);
}

void bench_parse_large(
	string const &corpus) {
	auto s = measure([&] { (void)parse(corpus); }, 5, 20, corpus.size());
	print("parse/large (~1MB nested)", s);
}

void bench_decode(
	string const &corpus) {
	auto doc = parse(corpus);
	if (!doc) {
		return;
	}
	// Measure: parse + extract name field as string_view from each object
	auto s = measure(
		[&] {
			auto res = parse(corpus);
			if (!res) {
				return;
			}
			auto arr = res->root().as_array();
			if (!arr) {
				return;
			}
			for (NodeRef const elem: arr->elements()) {
				auto obj = elem.as_object();
				if (!obj) {
					continue;
				}
				auto name = obj->find_member("name");
				if (name) {
					(void)name->as_string();
				}
			}
		},
		10,
		100,
		corpus.size());
	print("decode/struct-like (sv fields)", s);
}

void bench_find_member(
	string const &corpus) {
	auto doc = parse(corpus);
	if (!doc) {
		return;
	}
	auto obj = doc->root().as_object();
	if (!obj) {
		return;
	}
	// Look up 3 well-spread keys; median over all lookups
	auto s = measure(
		[&] {
			(void)obj->find_member("member_0");
			(void)obj->find_member("member_511");
			(void)obj->find_member("member_1023");
		},
		200,
		2000);
	// Divide by 3 to get per-lookup median
	s.median_ns /= 3.0;
	print("find_member/1024-member object (per lookup)", s);
}

void bench_array_traversal(
	string const &corpus) {
	auto doc = parse(corpus);
	if (!doc) {
		return;
	}
	auto arr = doc->root().as_array();
	if (!arr) {
		return;
	}
	auto s = measure(
		[&] {
			int64_t sum = 0;
			for (NodeRef const elem: arr->elements()) {
				auto n = elem.as_number();
				if (n) {
					auto v = n->to_i64();
					if (v) {
						sum += *v;
					}
				}
			}
			(void)sum;
		},
		50,
		500);
	print("array/traverse 10k numbers", s);
}

void bench_builder() {
	auto s = measure(
		[&] {
			auto b = value_builder();
			auto obj = b.begin_object();
			if (!obj) {
				return;
			}
			for (int i = 0; i < 64; ++i) {
				(void)obj->insert_string(format("key_{}", i), format("value_{}", i));
			}
			(void)move(*obj).commit();
			(void)move(b).finish();
		},
		50,
		500);
	print("builder/64-member object", s);
}

void bench_dump_plain(
	string const &corpus) {
	auto doc = parse(corpus);
	if (!doc) {
		return;
	}
	auto json_str = doc->dump();
	if (!json_str) {
		return;
	}
	auto s = measure([&] { (void)doc->dump(); }, 50, 500, json_str->size());
	print("dump/plain (no sort, no ascii_only)", s);
}

void bench_dump_sorted(
	string const &corpus) {
	auto doc = parse(corpus);
	if (!doc) {
		return;
	}
	JsonDumpOptions opts;
	opts.sort_object_keys = true;
	auto json_str = doc->dump(opts);
	if (!json_str) {
		return;
	}
	auto s = measure([&] { (void)doc->dump(opts); }, 20, 200, json_str->size());
	print("dump/sort_object_keys", s);
}

} // namespace

int main() { // NOLINT(bugprone-exception-escape)
	println("[json-bench] building corpora…");
	string const config_corpus = make_config_corpus();
	string const decode_corpus = make_decode_corpus();
	string const lookup_corpus = make_lookup_corpus();
	string const array_corpus = make_array_corpus();
	string const large_corpus = make_large_corpus();

	println(
		"[json-bench] corpus sizes: config={}B decode={}B lookup={}B array={}B large={}B",
		config_corpus.size(),
		decode_corpus.size(),
		lookup_corpus.size(),
		array_corpus.size(),
		large_corpus.size());
	println("[json-bench]");
	println("[json-bench] {:<40} {:>10}     {:>10}", "benchmark", "median", "throughput");
	println("[json-bench] {}", string(60, '-'));

	bench_parse_small(config_corpus);
	bench_parse_large(large_corpus);
	bench_decode(decode_corpus);
	bench_find_member(lookup_corpus);
	bench_array_traversal(array_corpus);
	bench_builder();
	bench_dump_plain(config_corpus);
	bench_dump_sorted(config_corpus);

	println("[json-bench]");
	println("[json-bench] Acceptance thresholds:");
	println("[json-bench]   parse >=500 MB/s on typical-config corpus");
	println("[json-bench]   find_member <=1000 ns median on 1024-member object");
	println("[json-bench]   dump >=1000 MB/s on plain path");
}
