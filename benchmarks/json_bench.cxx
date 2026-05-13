import std;
import conflux.types;
import conflux.json;
import bench_common;

using namespace conflux::json;
namespace {

template<typename F>
BenchStats measure(
	F &&fn,
	SZ warmup,
	SZ iters,
	SZ batch = 1,
	SZ bytes = 0) {
	for (SZ i = 0; i < warmup * batch; ++i) {
		fn();
	}
	V<u64> samples;
	samples.reserve(iters);
	u64 total = 0;
	for (SZ i = 0; i < iters; ++i) {
		u64 const t0 = bench_now_ns();
		for (SZ j = 0; j < batch; ++j) {
			fn();
		}
		u64 const elapsed = bench_now_ns() - t0;
		total += elapsed;
		samples.push_back(elapsed);
	}
	sort(samples.begin(), samples.end());
	double const med = static_cast<double>(samples[iters / 2]) / static_cast<double>(batch);
	double const mbs = (bytes > 0 && med > 0.0) ? static_cast<double>(bytes) / (med / 1e9) / (1024.0 * 1024.0) : 0.0;
	return {
		.iterations = iters * batch,
		.total_ns = total,
		.ns_per_iter = med,
		.throughput = mbs,
	};
}
bool g_csv = false;
bool g_first_row = true;
void print_row(
	SV name,
	BenchStats s) {
	s.variant = name;
	if (g_csv) {
		bench_print(s, true, g_first_row);
		g_first_row = false;
	} else if (s.throughput > 0.0) {
		print("[json-bench] {:<40} {:>10.1f} ns  {:>8.1f} MB/s\n", name, s.ns_per_iter, s.throughput);
	} else {
		print("[json-bench] {:<40} {:>10.1f} ns\n", name, s.ns_per_iter);
	}
}
// ---------------------------------------------------------------------------
// Corpus builders
// ---------------------------------------------------------------------------

// Typical config corpus: ~4 KB flat object with S/number/bool values.
S make_config_corpus() {
	S out;
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
// Struct-decode corpus: A of objects with SV-compatible fields.
S make_decode_corpus() {
	S out;
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
S make_lookup_corpus() {
	S out;
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
// Array traversal corpus: A of 10000 numbers.
S make_array_corpus() {
	S out;
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
S make_large_corpus() {
	S out;
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
// R0 — long-S-heavy corpus: 32 elements of 32 KiB ASCII payload, no
// escapes. Exercises memcpy-free zero-copy S slice + the SIMD scan_str
// fast path on long unescaped runs.
S make_long_strings_corpus() {
	S out;
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
S make_pretty_ws_corpus() {
	S out;
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
// R0 — escape-heavy corpus: a single 256 KiB S with backslash escapes
// at high density. Stresses the parse-side slow path (parse_str_decode_tail)
// and the dump-side escape scan.
S make_escape_heavy_corpus() {
	S out;
	constexpr SZ kTarget = 256UZ * 1024UZ;
	out.reserve(kTarget + 16);
	out += '"';
	while (out.size() + 8 < kTarget) {
		out += R"(\n\t\")"; // 6 source bytes → 3 JSON escapes per cycle
	}
	out += '"';
	return out;
}
// R0 — deeply-nested A: 256 levels of [[…]] with a single 0 at center.
// Tests recursion / iterative parse depth handling without tripping the
// 512-frame default max_depth.
S make_deep_nest_corpus() {
	S out;
	constexpr int kDepth = 256;
	out.reserve(kDepth * 2 + 4);
	out.append(kDepth, '[');
	out += '0';
	out.append(kDepth, ']');
	return out;
}
// R0 — mixed-number corpus: ~1 MB A of integers, scientific,
// long fractions, signed values. Stresses number-lexeme parse paths.
S make_mixed_numbers_corpus() {
	S out;
	out.reserve(1024UZ * 1024UZ + 4096);
	out += '[';
	bool first = true;
	int i = 0;
	constexpr SZ kTarget = 1024UZ * 1024UZ - 16;
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
	S const &corpus) {
	auto s = measure([&] { (void)parse(corpus); }, 50, 500, 1, corpus.size());
	print_row("parse/small (~4KB config)", s);
}
void bench_parse_large(
	S const &corpus) {
	auto s = measure([&] { (void)parse(corpus); }, 5, 20, 1, corpus.size());
	print_row("parse/large (~1MB nested)", s);
}
void bench_decode(
	S const &corpus) {
	auto doc = parse(corpus);
	if (!doc) {
		return;
	}
	// Measure: parse + extract name field as SV from each object
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
	S const &corpus) {
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
	s.ns_per_iter /= 3.0;
	print_row("find_member/1024-member object (per lookup)", s);
}
void bench_array_traversal(
	S const &corpus) {
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
			i64 sum = 0;
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
	print_row("A/traverse 10k numbers", s);
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
			move(*obj).commit();
			(void)move(b).finish();
		},
		50,
		500);
	print_row("builder/64-member object", s);
}
void bench_dump_plain(
	S const &corpus) {
	auto doc = parse(corpus);
	if (!doc) {
		return;
	}
	auto json_str = doc->dump();
	if (!json_str) {
		return;
	}
	auto s = measure([&] { (void)doc->dump(); }, 50, 500, 1, json_str->size());
	print_row("dump/plain (no sort / no ascii_only)", s);
}
void bench_dump_sorted(
	S const &corpus) {
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
void bench_accumulate_chunked(
	SV name,
	S const &corpus,
	SZ chunk_size) {
	auto s = measure(
		[&] {
			JsonAccumulator acc;
			auto const *ptr = corpus.data();
			SZ remaining = corpus.size();
			while (remaining > 0) {
				SZ const n = std::min(chunk_size, remaining);
				auto feed = acc.feed(span<byte const>{
					reinterpret_cast<byte const *>(ptr),
					n});
				if (!feed) {
					throw RE{"json accumulator feed failed"};
				}
				ptr += n;
				remaining -= n;
			}
			auto doc = acc.finish();
			if (!doc) {
				throw RE{"json accumulator finish failed"};
			}
			(void)doc->root();
		},
		20,
		100,
		1,
		corpus.size());
	print_row(name, s);
}
// Item C — 1024-member object where every key has a \u escape → arena storage.
// Decoded names are identical to make_lookup_corpus() ("member_N"), so the
// same lookup keys can be used for apples-to-apples comparison.
S make_lookup_escaped_corpus() {
	// Keys: "member_N" (JSON) → decoded "member_N".
	// All MemberEntry flags = 0 (arena); kStorageInputView never set.
	S out;
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
S make_lookup_mixed_corpus() {
	S out;
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
// FI-1 — small object (below kHashThreshold=32): find_member always does linear
// scan. Proxy for per-lookup cost after the sentinel caches a build failure.
S make_below_threshold_corpus() {
	S out = "{";
	for (int i = 0; i < 7; ++i) {
		if (i > 0) {
			out += ',';
		}
		out += format(R"("field_{}":{})", i, i);
	}
	out += '}';
	return out;
}
// 5.5-B gate: 31-member object — always linear (just below kHashThreshold=32).
// Isolates cache-line packing benefit of 16-byte vs 24-byte MemberEntry.
S make_linear31_corpus() {
	S out = "{";
	for (int i = 0; i < 31; ++i) {
		if (i > 0) {
			out += ',';
		}
		out += format(R"("member_{}":{},)", i, i);
		out.pop_back(); // remove trailing comma left by format string
	}
	out += '}';
	return out;
}
void bench_find_member_linear31(
	S const &corpus) {
	auto doc = parse(corpus);
	if (!doc) {
		return;
	}
	auto obj = doc->root().as_object();
	if (!obj) {
		return;
	}
	auto s = measure(
		[&] {
			(void)obj->find_member("member_0");
			(void)obj->find_member("member_15");
			(void)obj->find_member("member_30");
		},
		200,
		500,
		1000);
	s.ns_per_iter /= 3.0;
	print_row("find_member/31-member linear (per lookup)", s);
}
// Item C — probe throughput on arena-storage names (baseline: bench_find_member
// uses kStorageInputView names). Delta isolates member_name() dispatch overhead.
void bench_find_member_escaped(
	S const &corpus) {
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
	s.ns_per_iter /= 3.0;
	print_row("find_member/1024-member escaped names (per lookup)", s);
}
// Item C — worst-case dispatch: alternating kStorageInputView/arena per probe.
void bench_find_member_mixed(
	S const &corpus) {
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
	s.ns_per_iter /= 3.0;
	print_row("find_member/1024-member mixed names (per lookup)", s);
}
// Item E — builder name-copy cost: same value ("v"), varying key length.
// If per-insert ns scales with key length, arena copy is the hot path.
// If flat, overhead is in tree structure — name-copy optimisation is not justified.
void bench_builder_name_length() {
	constexpr int kMembers = 256;
	auto gen_keys = [](SZ n, SZ total_len) {
		V<S> keys;
		keys.reserve(n);
		for (SZ i = 0; i < n; ++i) {
			S const suffix = to_string(i);
			SZ const pad = total_len > suffix.size() ? total_len - suffix.size() : 0;
			S k(pad, 'k');
			k += suffix;
			keys.push_back(move(k));
		}
		return keys;
	};
	V<S> const k5 = gen_keys(static_cast<SZ>(kMembers), 5);
	V<S> const k32 = gen_keys(static_cast<SZ>(kMembers), 32);
	V<S> const k128 = gen_keys(static_cast<SZ>(kMembers), 128);

	auto run = [&](V<S> const &keys, SV label) {
		auto s = measure(
			[&] {
				auto b = value_builder();
				auto obj = b.begin_object();
				if (!obj) {
					return;
				}
				for (int i = 0; i < kMembers; ++i) {
					(void)obj->insert_string(keys[static_cast<SZ>(i)], "v");
				}
				move(*obj).commit();
				(void)move(b).finish();
			},
			50,
			500);
		s.ns_per_iter /= static_cast<double>(kMembers);
		print_row(label, s);
	};

	run(k5, "builder/insert_string   5-char keys (per insert)");
	run(k32, "builder/insert_string  32-char keys (per insert)");
	run(k128, "builder/insert_string 128-char keys (per insert)");

	auto run_view = [&](V<S> const &keys, SV label) {
		auto s = measure(
			[&] {
				auto b = value_builder();
				auto obj = b.begin_object();
				if (!obj) {
					return;
				}
				for (SZ i = 0; i < static_cast<SZ>(kMembers); ++i) {
					(void)obj->insert_string_borrowed_name(keys[i], "v");
				}
				move(*obj).commit();
				(void)move(b).finish();
			},
			50,
			500);
		s.ns_per_iter /= static_cast<double>(kMembers);
		print_row(label, s);
	};

	run_view(k5, "builder/insert_string_borrowed_name   5-char keys (per insert)");
	run_view(k32, "builder/insert_string_borrowed_name  32-char keys (per insert)");
	run_view(k128, "builder/insert_string_borrowed_name 128-char keys (per insert)");
}
// R0 — generic parse/dump drivers used for the new corpora.
void bench_parse_named(
	SV name,
	S const &corpus,
	SZ warmup = 5,
	SZ iters = 50) {
	auto s = measure([&] { (void)parse(corpus); }, warmup, iters, 1, corpus.size());
	print_row(name, s);
}
void bench_dump_named(
	SV name,
	S const &corpus,
	SZ warmup = 5,
	SZ iters = 50) {
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
// FI-1 — measures two components that together show the value of the sentinel:
//
//   (A) linear-only lookup (7-member, below kHashThreshold=48) — this is the
//       per-lookup cost WITH the sentinel cached (find_member short-circuits
//       straight to linear scan on every subsequent call after the first failure).
//
//   (B) hash-build overhead = (parse + first find_member) − (parse only), measured
//       on the 1024-member corpus. In the adversarial repeat-lookup scenario
//       WITHOUT the sentinel, (B) would be paid on every single call because
//       hash_idx_raw stays nullptr and each call retries alloc + build + free.
//       With the sentinel (FI-1), (B) is paid exactly once.
void bench_fi1_sentinel(
	S const &small_corpus,
	S const &lookup_corpus) {
	{
		auto doc = parse(small_corpus);
		if (!doc) {
			return;
		}
		auto obj = doc->root().as_object();
		if (!obj) {
			return;
		}
		auto s = measure(
			[&] {
				(void)obj->find_member("field_0");
				(void)obj->find_member("field_3");
				(void)obj->find_member("field_6");
			},
			200,
			1000,
			1000);
		s.ns_per_iter /= 3.0;
		print_row("FI-1/sentinel: (A) linear-only 7-member (failure path proxy)", s);
	}
	{
		auto parse_only = measure([&] { (void)parse(lookup_corpus); }, 10, 100);
		auto parse_find = measure(
			[&] {
				auto d = parse(lookup_corpus);
				if (!d) {
					return;
				}
				auto o = d->root().as_object();
				if (!o) {
					return;
				}
				(void)o->find_member("member_512");
			},
			10,
			100);
		double const build_ns = parse_find.ns_per_iter - parse_only.ns_per_iter;
		BenchStats diff{};
		diff.ns_per_iter = max(0.0, build_ns);
		print_row("FI-1/sentinel: (B) build+lookup overhead (parse+find − parse-only)", diff);
	}
}

} // namespace
// ---------------------------------------------------------------------------
// UnknownMemberPolicy::reject cost on wide objects
// ---------------------------------------------------------------------------

struct BenchModel5 {
	i64 id{};
	S name{};
	double score{};
	bool active{};
	S tag{};
};
template<>
struct JsonMembers<BenchModel5> {
	static constexpr auto members() {
		return Tup{
			json_member("id", &BenchModel5::id),
			json_member("name", &BenchModel5::name),
			json_member("score", &BenchModel5::score),
			json_member("active", &BenchModel5::active),
			json_member("tag", &BenchModel5::tag),
		};
	}
	static constexpr SV type_name() { return "BenchModel5"; }
};
namespace {

S make_reject_corpus(
	SZ extra_members) {
	S out;
	out.reserve(extra_members * 30 + 128);
	out += R"({"id":42,"name":"bench","score":3.14,"active":true,"tag":"x")";
	for (SZ i = 0; i < extra_members; ++i) {
		out += format(R"(,"extra_field_{}":{})", i, i);
	}
	out += '}';
	return out;
}
void bench_reject_policy() {
	for (SZ extra: A<SZ, 5>{0, 10, 50, 100, 200}) {
		S const corpus = make_reject_corpus(extra);
		auto doc_res = parse(corpus);
		if (!doc_res) {
			return;
		}

		// reject policy (default): O(N·M) scan after DOM decode
		auto s_reject = measure(
			[&] {
				auto d = parse(corpus);
				if (!d) {
					return;
				}
				JsonDecodeOptions opts;
				opts.unknown_members = UnknownMemberPolicy::reject;
				auto r = decode<BenchModel5>(d->root(), opts);
				(void)r;
			},
			20,
			500);
		print_row(format("decode/reject 5+{} members", extra), s_reject);

		// ignore policy: no extra scan
		auto s_ignore = measure(
			[&] {
				auto d = parse(corpus);
				if (!d) {
					return;
				}
				JsonDecodeOptions opts;
				opts.unknown_members = UnknownMemberPolicy::ignore;
				auto r = decode<BenchModel5>(d->root(), opts);
				(void)r;
			},
			20,
			500);
		print_row(format("decode/ignore 5+{} members", extra), s_ignore);
	}
}

} // namespace
// NOLINTNEXTLINE(bugprone-exception-escape)
int main(
	int argc,
	char **argv) {
	bench_info_if_requested(
		argc,
		argv,
		R"({"name":"json","parser":"standard","configs":[{"name":"default","extra":{},"args":[]}]})");
	auto const cfg = bench_parse_args(span{argv, static_cast<SZ>(argc)});
	g_csv = cfg.json_out;
	if (!g_csv) {
		println("[json-bench] building corpora…");
	}
	S const config_corpus = make_config_corpus();
	S const decode_corpus = make_decode_corpus();
	S const lookup_corpus = make_lookup_corpus();
	S const array_corpus = make_array_corpus();
	S const large_corpus = make_large_corpus();
	S const long_strings_corpus = make_long_strings_corpus();
	S const pretty_ws_corpus = make_pretty_ws_corpus();
	S const escape_heavy_corpus = make_escape_heavy_corpus();
	S const deep_nest_corpus = make_deep_nest_corpus();
	S const mixed_numbers_corpus = make_mixed_numbers_corpus();
	S const lookup_escaped_corpus = make_lookup_escaped_corpus();
	S const lookup_mixed_corpus = make_lookup_mixed_corpus();
	S const below_threshold_corpus = make_below_threshold_corpus();
	S const linear31_corpus = make_linear31_corpus();

	if (!g_csv) {
		println(
			"[json-bench] corpus sizes: config={}B decode={}B lookup={}B A={}B large={}B",
			config_corpus.size(),
			decode_corpus.size(),
			lookup_corpus.size(),
			array_corpus.size(),
			large_corpus.size());
		println(
			"[json-bench]                long_strings={}B pretty_ws={}B escape_heavy={}B deep_nest={}B "
			"mixed_numbers={}B",
			long_strings_corpus.size(),
			pretty_ws_corpus.size(),
			escape_heavy_corpus.size(),
			deep_nest_corpus.size(),
			mixed_numbers_corpus.size());
		println("[json-bench]");
		println("[json-bench] {:<40} {:>10}     {:>10}", "benchmark", "median", "throughput");
		println("[json-bench] {}", S(60, '-'));
	}

	bench_parse_small(config_corpus);
	bench_parse_large(large_corpus);
	bench_decode(decode_corpus);
	bench_find_member(lookup_corpus);
	bench_array_traversal(array_corpus);
	bench_builder();
	bench_dump_plain(config_corpus);
	bench_dump_sorted(config_corpus);
	bench_accumulate_chunked("accumulate/byte_span chunked (4KB config)", config_corpus, 4096);

	if (!g_csv) {
		println("[json-bench]");
		println("[json-bench] -- R0 corpora (added v16) --");
	}
	bench_parse_named("parse/long_strings (1MB / 32x32KiB)", long_strings_corpus);
	bench_dump_named("dump/long_strings", long_strings_corpus);
	bench_parse_named("parse/pretty_ws (1MB indented)", pretty_ws_corpus);
	bench_dump_named("dump/pretty_ws", pretty_ws_corpus);
	bench_parse_named("parse/escape_heavy (256KiB)", escape_heavy_corpus, 10, 100);
	bench_dump_named("dump/escape_heavy", escape_heavy_corpus, 10, 100);
	bench_parse_named("parse/deep_nest (256 levels)", deep_nest_corpus, 50, 500);
	bench_parse_named("parse/mixed_numbers (1MB)", mixed_numbers_corpus);
	bench_dump_named("dump/mixed_numbers", mixed_numbers_corpus);
	bench_accumulate_chunked("accumulate/byte_span chunked (4KB large)", large_corpus, 4096);

	if (!g_csv) {
		println("[json-bench]");
		println("[json-bench] -- v16 Item C/E: member_name dispatch + builder name-copy --");
		println("[json-bench]    Baseline (plain names, kStorageInputView) already shown above.");
	}
	bench_find_member_escaped(lookup_escaped_corpus);
	bench_find_member_mixed(lookup_mixed_corpus);
	bench_find_member_linear31(linear31_corpus);
	bench_builder_name_length();

	if (!g_csv) {
		println("[json-bench]");
		println("[json-bench] -- FI-1: sentinel prevents repeated hash-build on failure --");
		println("[json-bench]    (A) per-lookup cost after sentinel cached; (B) overhead saved per repeat call");
		println("[json-bench]    adversarial cost WITHOUT sentinel: (A)+(B) per lookup; WITH: (A) after first call");
	}
	bench_fi1_sentinel(below_threshold_corpus, lookup_corpus);

	if (!g_csv) {
		println("[json-bench]");
		println("[json-bench] -- UnknownMemberPolicy::reject O(N·M) cost --");
	}
	bench_reject_policy();

	if (!g_csv) {
		println("[json-bench]");
		println("[json-bench] Acceptance thresholds:");
		println("[json-bench]   parse >=500 MB/s on typical-config corpus");
		println("[json-bench]   find_member <=1000 ns median on 1024-member object");
		println("[json-bench]   dump >=1000 MB/s on plain path");
	}

	{
		namespace fs = std::filesystem;
		fs::path const corpus_dir = fs::path{__FILE__}.parent_path() / "corpus";
		auto load = [&](SV filename) -> S {
			auto p = corpus_dir / filename;
			std::ifstream f{p, std::ios::binary};
			if (!f) {
				return {};
			}
			return {std::istreambuf_iterator<char>{f}, {}};
		};
		struct {
			SV label;
			SV file;
		} const rw[] = {
			{          "canada.json (2.1MB geo)",        "canada.json"},
			{"citm_catalog.json (1.6MB catalog)",  "citm_catalog.json"},
			{      "twitter.json (617KB social)",       "twitter.json"},
			{    "apache_builds.json (406KB CI)", "apache_builds.json"},
			{"github_events.json (333KB events)", "github_events.json"},
		};
		bool printed_header = false;
		for (auto const &[label, file]: rw) {
			auto corpus = load(file);
			if (corpus.empty()) {
				continue;
			}
			if (!printed_header && !g_csv) {
				println("[json-bench]");
				println("[json-bench] -- real-world corpora --");
				printed_header = true;
			}
			bench_parse_named(format("parse/{}", label), corpus);
			bench_dump_named(format("dump/{}", label), corpus);
		}
	}
}
