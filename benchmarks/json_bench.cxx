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
	size_t batch = 1, // calls per timed window — amortises clock overhead for fast ops
	size_t bytes = 0) {
	for (size_t i = 0; i < warmup * batch; ++i) {
		fn();
	}
	vector<double> samples;
	samples.reserve(iters);
	for (size_t i = 0; i < iters; ++i) {
		auto t0 = chrono::steady_clock::now();
		for (size_t j = 0; j < batch; ++j) {
			fn();
		}
		auto t1 = chrono::steady_clock::now();
		samples.push_back(
			static_cast<double>(chrono::duration_cast<chrono::nanoseconds>(t1 - t0).count())
			/ static_cast<double>(batch));
	}
	sort(samples.begin(), samples.end());
	double const med = samples[iters / 2];
	double const mbs = (bytes > 0 && med > 0.0) ? static_cast<double>(bytes) / (med / 1e9) / (1024.0 * 1024.0) : 0.0;
	return {med, mbs};
}

void print_row(
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

// R0 — long-string-heavy corpus: 32 elements of 32 KiB ASCII payload, no
// escapes. Exercises memcpy-free zero-copy string slice + the SIMD scan_str
// fast path on long unescaped runs.
string make_long_strings_corpus() {
	string out;
	out.reserve(1024UZ * 1024UZ + 4096);
	out += '[';
	constexpr int kElems = 32;
	constexpr int kLen = 32 * 1024;
	for (int i = 0; i < kElems; ++i) {
		if (i > 0) {
			out += ',';
		}
		out += '"';
		for (int k = 0; k < kLen; ++k) {
			out += static_cast<char>('a' + (k % 26));
		}
		out += '"';
	}
	out += ']';
	return out;
}

// R0 — pretty-printed corpus: ~1 MB flat object, 2-space indent + newlines.
// Exposes skip_ws cost; today's compact corpora hide it.
string make_pretty_ws_corpus() {
	string out;
	out.reserve(1024UZ * 1024UZ + 4096);
	out += "{\n";
	constexpr int kMembers = 16000;
	for (int i = 0; i < kMembers; ++i) {
		out += "  \"key_";
		out += to_string(i);
		out += "\" : ";
		out += to_string(i * 17);
		if (i + 1 < kMembers) {
			out += ',';
		}
		out += '\n';
	}
	out += "}\n";
	return out;
}

// R0 — escape-heavy corpus: a single 256 KiB string with backslash escapes
// at high density. Stresses the parse-side slow path (parse_str_decode_tail)
// and the dump-side escape scan.
string make_escape_heavy_corpus() {
	string out;
	constexpr size_t kTarget = 256UZ * 1024UZ;
	out.reserve(kTarget + 16);
	out += '"';
	while (out.size() + 8 < kTarget) {
		out += R"(\n\t\")"; // 6 source bytes → 3 JSON escapes per cycle
	}
	out += '"';
	return out;
}

// R0 — deeply-nested array: 256 levels of [[…]] with a single 0 at center.
// Tests recursion / iterative parse depth handling without tripping the
// 512-frame default max_depth.
string make_deep_nest_corpus() {
	string out;
	constexpr int kDepth = 256;
	out.reserve(kDepth * 2 + 4);
	out.append(kDepth, '[');
	out += '0';
	out.append(kDepth, ']');
	return out;
}

// R0 — mixed-number corpus: ~1 MB array of integers, scientific,
// long fractions, signed values. Stresses number-lexeme parse paths.
string make_mixed_numbers_corpus() {
	string out;
	out.reserve(1024UZ * 1024UZ + 4096);
	out += '[';
	bool first = true;
	int i = 0;
	constexpr size_t kTarget = 1024UZ * 1024UZ - 16;
	while (out.size() < kTarget) {
		if (!first) {
			out += ',';
		}
		first = false;
		switch (i % 4) {
		case 0 : out += to_string(i); break;
		case 1 : out += format("{}.{}e{}", i, i * 3, (i % 7) - 3); break;
		case 2 : out += format("0.{}", i); break;
		case 3 : out += format("-{}.{}", i, i * 9); break;
		default: break;
		}
		++i;
	}
	out += ']';
	return out;
}

// ---------------------------------------------------------------------------
// Benchmark drivers
// ---------------------------------------------------------------------------

void bench_parse_small(
	string const &corpus) {
	auto s = measure([&] { (void)parse(corpus); }, 50, 500, 1, corpus.size());
	print_row("parse/small (~4KB config)", s);
}

void bench_parse_large(
	string const &corpus) {
	auto s = measure([&] { (void)parse(corpus); }, 5, 20, 1, corpus.size());
	print_row("parse/large (~1MB nested)", s);
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
		1,
		corpus.size());
	print_row("decode/struct-like (sv fields)", s);
}

void bench_find_member(
	string const &corpus) {
	auto doc = parse(corpus);
	if (!doc) {
		return;
	}
	(void)doc->warm_member_index(doc->root()); // pre-build hash index
	auto obj = doc->root().as_object();
	if (!obj) {
		return;
	}
	// batch=1000: amortise clock overhead for sub-microsecond lookup
	auto s = measure(
		[&] {
			(void)obj->find_member("member_0");
			(void)obj->find_member("member_511");
			(void)obj->find_member("member_1023");
		},
		200,
		500,
		1000);
	s.median_ns /= 3.0;
	print_row("find_member/1024-member object (per lookup)", s);
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
	print_row("array/traverse 10k numbers", s);
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
	print_row("builder/64-member object", s);
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
	auto s = measure([&] { (void)doc->dump(); }, 50, 500, 1, json_str->size());
	print_row("dump/plain (no sort, no ascii_only)", s);
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
	auto s = measure([&] { (void)doc->dump(opts); }, 20, 200, 1, json_str->size());
	print_row("dump/sort_object_keys", s);
}

// Item C — 1024-member object where every key has a \u escape → arena storage.
// Decoded names are identical to make_lookup_corpus() ("member_N"), so the
// same lookup keys can be used for apples-to-apples comparison.
string make_lookup_escaped_corpus() {
	// Keys: "member_N" (JSON) → decoded "member_N".
	// All MemberEntry flags = 0 (arena); kStorageInputView never set.
	string out;
	out.reserve(65536);
	out += '{';
	for (int i = 0; i < 1024; ++i) {
		if (i > 0) {
			out += ',';
		}
		out += format("\"\\u006Dember_{}\":{}", i, i);
	}
	out += '}';
	return out;
}

// Item C — 1024-member object with alternating plain/escaped keys.
// Even indices: plain ("member_N", kStorageInputView).
// Odd indices:  "member_N" decoded to "member_N" (arena storage).
// Half-half pattern is worst-case for branch prediction in member_name() dispatch.
string make_lookup_mixed_corpus() {
	string out;
	out.reserve(65536);
	out += '{';
	for (int i = 0; i < 1024; ++i) {
		if (i > 0) {
			out += ',';
		}
		if (i % 2 == 0) {
			out += format("\"member_{}\":{}", i, i);
		} else {
			out += format("\"\\u006Dember_{}\":{}", i, i);
		}
	}
	out += '}';
	return out;
}

// Item C — probe throughput on arena-storage names (baseline: bench_find_member
// uses kStorageInputView names). Delta isolates member_name() dispatch overhead.
void bench_find_member_escaped(
	string const &corpus) {
	auto doc = parse(corpus);
	if (!doc) {
		return;
	}
	(void)doc->warm_member_index(doc->root());
	auto obj = doc->root().as_object();
	if (!obj) {
		return;
	}
	auto s = measure(
		[&] {
			(void)obj->find_member("member_0");
			(void)obj->find_member("member_511");
			(void)obj->find_member("member_1023");
		},
		200,
		500,
		1000);
	s.median_ns /= 3.0;
	print_row("find_member/1024-member escaped names (per lookup)", s);
}

// Item C — worst-case dispatch: alternating kStorageInputView/arena per probe.
void bench_find_member_mixed(
	string const &corpus) {
	auto doc = parse(corpus);
	if (!doc) {
		return;
	}
	(void)doc->warm_member_index(doc->root());
	auto obj = doc->root().as_object();
	if (!obj) {
		return;
	}
	auto s = measure(
		[&] {
			(void)obj->find_member("member_0"); // plain (even)
			(void)obj->find_member("member_511"); // escaped (odd)
			(void)obj->find_member("member_1023"); // escaped (odd)
		},
		200,
		500,
		1000);
	s.median_ns /= 3.0;
	print_row("find_member/1024-member mixed names (per lookup)", s);
}

// Item E — builder name-copy cost: same value ("v"), varying key length.
// If per-insert ns scales with key length, arena copy is the hot path.
// If flat, overhead is in tree structure — name-copy optimisation is not justified.
void bench_builder_name_length() {
	constexpr int kMembers = 256;
	auto gen_keys = [](size_t n, size_t total_len) {
		vector<string> keys;
		keys.reserve(n);
		for (size_t i = 0; i < n; ++i) {
			string const suffix = to_string(i);
			size_t const pad = total_len > suffix.size() ? total_len - suffix.size() : 0;
			string k(pad, 'k');
			k += suffix;
			keys.push_back(move(k));
		}
		return keys;
	};
	vector<string> const k5 = gen_keys(static_cast<size_t>(kMembers), 5);
	vector<string> const k32 = gen_keys(static_cast<size_t>(kMembers), 32);
	vector<string> const k128 = gen_keys(static_cast<size_t>(kMembers), 128);

	auto run = [&](vector<string> const &keys, string_view label) {
		auto s = measure(
			[&] {
				auto b = value_builder();
				auto obj = b.begin_object();
				if (!obj) {
					return;
				}
				for (int i = 0; i < kMembers; ++i) {
					(void)obj->insert_string(keys[static_cast<size_t>(i)], "v");
				}
				move(*obj).commit();
				(void)move(b).finish();
			},
			50,
			500);
		s.median_ns /= static_cast<double>(kMembers);
		print_row(label, s);
	};

	run(k5, "builder/insert_string   5-char keys (per insert)");
	run(k32, "builder/insert_string  32-char keys (per insert)");
	run(k128, "builder/insert_string 128-char keys (per insert)");

	auto run_view = [&](vector<string> const &keys, string_view label) {
		auto s = measure(
			[&] {
				auto b = value_builder();
				auto obj = b.begin_object();
				if (!obj) {
					return;
				}
				for (size_t i = 0; i < static_cast<size_t>(kMembers); ++i) {
					(void)obj->insert_string_view(keys[i], "v");
				}
				move(*obj).commit();
				(void)move(b).finish();
			},
			50,
			500);
		s.median_ns /= static_cast<double>(kMembers);
		print_row(label, s);
	};

	run_view(k5, "builder/insert_string_view   5-char keys (per insert)");
	run_view(k32, "builder/insert_string_view  32-char keys (per insert)");
	run_view(k128, "builder/insert_string_view 128-char keys (per insert)");
}

// R0 — generic parse/dump drivers used for the new corpora.
void bench_parse_named(
	string_view name,
	string const &corpus,
	size_t warmup = 5,
	size_t iters = 50) {
	auto s = measure([&] { (void)parse(corpus); }, warmup, iters, 1, corpus.size());
	print_row(name, s);
}

void bench_dump_named(
	string_view name,
	string const &corpus,
	size_t warmup = 5,
	size_t iters = 50) {
	auto doc = parse(corpus);
	if (!doc) {
		return;
	}
	auto json_str = doc->dump();
	if (!json_str) {
		return;
	}
	auto s = measure([&] { (void)doc->dump(); }, warmup, iters, 1, json_str->size());
	print_row(name, s);
}

} // namespace

int main() { // NOLINT(bugprone-exception-escape)
	println("[json-bench] building corpora…");
	string const config_corpus = make_config_corpus();
	string const decode_corpus = make_decode_corpus();
	string const lookup_corpus = make_lookup_corpus();
	string const array_corpus = make_array_corpus();
	string const large_corpus = make_large_corpus();
	string const long_strings_corpus = make_long_strings_corpus();
	string const pretty_ws_corpus = make_pretty_ws_corpus();
	string const escape_heavy_corpus = make_escape_heavy_corpus();
	string const deep_nest_corpus = make_deep_nest_corpus();
	string const mixed_numbers_corpus = make_mixed_numbers_corpus();
	string const lookup_escaped_corpus = make_lookup_escaped_corpus();
	string const lookup_mixed_corpus = make_lookup_mixed_corpus();

	println(
		"[json-bench] corpus sizes: config={}B decode={}B lookup={}B array={}B large={}B",
		config_corpus.size(),
		decode_corpus.size(),
		lookup_corpus.size(),
		array_corpus.size(),
		large_corpus.size());
	println(
		"[json-bench]                long_strings={}B pretty_ws={}B escape_heavy={}B deep_nest={}B mixed_numbers={}B",
		long_strings_corpus.size(),
		pretty_ws_corpus.size(),
		escape_heavy_corpus.size(),
		deep_nest_corpus.size(),
		mixed_numbers_corpus.size());
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
	println("[json-bench] -- R0 corpora (added v16) --");
	bench_parse_named("parse/long_strings (1MB, 32x32KiB)", long_strings_corpus);
	bench_dump_named("dump/long_strings", long_strings_corpus);
	bench_parse_named("parse/pretty_ws (1MB indented)", pretty_ws_corpus);
	bench_dump_named("dump/pretty_ws", pretty_ws_corpus);
	bench_parse_named("parse/escape_heavy (256KiB)", escape_heavy_corpus, 10, 100);
	bench_dump_named("dump/escape_heavy", escape_heavy_corpus, 10, 100);
	bench_parse_named("parse/deep_nest (256 levels)", deep_nest_corpus, 50, 500);
	bench_parse_named("parse/mixed_numbers (1MB)", mixed_numbers_corpus);
	bench_dump_named("dump/mixed_numbers", mixed_numbers_corpus);

	println("[json-bench]");
	println("[json-bench] -- v16 Item C/E: member_name dispatch + builder name-copy --");
	println("[json-bench]    Baseline (plain names, kStorageInputView) already shown above.");
	bench_find_member_escaped(lookup_escaped_corpus);
	bench_find_member_mixed(lookup_mixed_corpus);
	bench_builder_name_length();

	println("[json-bench]");
	println("[json-bench] Acceptance thresholds:");
	println("[json-bench]   parse >=500 MB/s on typical-config corpus");
	println("[json-bench]   find_member <=1000 ns median on 1024-member object");
	println("[json-bench]   dump >=1000 MB/s on plain path");
}
