module;
#include "json_simd_backend.hxx"

module conflux.json;

import std;
import std.compat;
import conflux.types;

// ---------------------------------------------------------------------------
// Dump implementation
// ---------------------------------------------------------------------------

// NOLINTBEGIN(readability-magic-numbers)
// Fast-path dump for bytes already known to be a raw JSON std::string body
// (kRawJsonSlice set on parse-side unescaped strings/numbers): no scan,
// just bracket the slice with quotes. Caller must guarantee `flags &
// kRawJsonSlice` and !ascii_only (the latter would still need a
// std::byte-by-std::byte non-ASCII rewrite).
inline void dump_str_raw(
	std::string_view sv,
	std::string &out) {
	out += '"';
	out.append(sv.data(), sv.size());
	out += '"';
}
inline void append_u_escape(
	std::string &out,
	std::uint32_t cp) {
	static constexpr std::array<char, 16> kHex =
		{'0', '1', '2', '3', '4', '5', '6', '7', '8', '9', 'a', 'b', 'c', 'd', 'e', 'f'};
	out += "\\u";
	out += kHex[(cp >> 12U) & 0x0FU];
	out += kHex[(cp >> 8U) & 0x0FU];
	out += kHex[(cp >> 4U) & 0x0FU];
	out += kHex[cp & 0x0FU];
}
// R3 — find the next std::byte in [p, n) that needs escaping in a JSON string body.
// ascii_only=false: stops at '"', '\\', or ctrl chars [0x00,0x1F].
// ascii_only=true:  also stops at high-bit bytes [0x80,0xFF].
[[nodiscard]] inline std::size_t scan_dump_safe_run(
	char const *p,
	std::size_t n,
	bool ascii_only) noexcept {
	std::size_t i = 0;
#if defined(CONFLUX_JSON_HAS_STDSIMD) && CONFLUX_SIMD_SELECTION_DIRECT
	constexpr std::size_t kStdsimdThreshold = 32;
	if (n >= kStdsimdThreshold) {
		return conflux_json_scan_dump_safe_run_stdsimd(p, n, ascii_only ? 1 : 0);
	}
#elif defined(CONFLUX_JSON_HAS_STDSIMD) && CONFLUX_SIMD_SELECTION_RUNTIME && defined(CONFLUX_JSON_STDSIMD_IFUNC)
	constexpr std::size_t kStdsimdThreshold = 32;
	if (n >= kStdsimdThreshold) {
		return conflux_json_scan_dump_safe_run_stdsimd(p, n, ascii_only ? 1 : 0);
	}
#elif defined(CONFLUX_JSON_HAS_STDSIMD) && CONFLUX_SIMD_SELECTION_RUNTIME
	constexpr std::size_t kStdsimdThreshold = 32;
	if (n >= kStdsimdThreshold && conflux_cpu_supports_avx2()) {
		return conflux_json_scan_dump_safe_run_stdsimd(p, n, ascii_only ? 1 : 0);
	}
#elif defined(CONFLUX_JSON_HAS_AVX2)
	__m256i const v_quote = _mm256_set1_epi8('"');
	__m256i const v_back = _mm256_set1_epi8('\\');
	__m256i const v_lim = _mm256_set1_epi8(0x20);
	__m256i const v_1f = _mm256_set1_epi8(0x1F);
	while (i + 32 <= n) {
		__m256i const v = _mm256_loadu_si256(reinterpret_cast<__m256i const *>(p + i));
		__m256i const eq_q = _mm256_cmpeq_epi8(v, v_quote);
		__m256i const eq_b = _mm256_cmpeq_epi8(v, v_back);
		__m256i mix = _mm256_or_si256(eq_q, eq_b);
		if (ascii_only) {
			mix = _mm256_or_si256(mix, _mm256_cmpgt_epi8(v_lim, v));
		} else {
			mix = _mm256_or_si256(mix, _mm256_cmpeq_epi8(_mm256_min_epu8(v, v_1f), v));
		}
		auto const mask = static_cast<unsigned>(_mm256_movemask_epi8(mix));
		if (mask != 0U) {
			return i + static_cast<std::size_t>(__builtin_ctz(mask));
		}
		i += 32;
	}
	if (i + 16 <= n) {
		__m128i const v128 = _mm_loadu_si128(reinterpret_cast<__m128i const *>(p + i));
		__m128i const eq_q = _mm_cmpeq_epi8(v128, _mm256_castsi256_si128(v_quote));
		__m128i const eq_b = _mm_cmpeq_epi8(v128, _mm256_castsi256_si128(v_back));
		__m128i mix16 = _mm_or_si128(eq_q, eq_b);
		if (ascii_only) {
			mix16 = _mm_or_si128(mix16, _mm_cmplt_epi8(v128, _mm256_castsi256_si128(v_lim)));
		} else {
			mix16 = _mm_or_si128(mix16, _mm_cmpeq_epi8(_mm_min_epu8(v128, _mm256_castsi256_si128(v_1f)), v128));
		}
		auto const mask16 = static_cast<unsigned>(_mm_movemask_epi8(mix16));
		if (mask16 != 0U) {
			return i + static_cast<std::size_t>(__builtin_ctz(mask16));
		}
		i += 16;
	}
#elif defined(CONFLUX_JSON_HAS_SSE2)
	__m128i const v_quote = _mm_set1_epi8('"');
	__m128i const v_back = _mm_set1_epi8('\\');
	__m128i const v_lim = _mm_set1_epi8(0x20);
	__m128i const v_1f = _mm_set1_epi8(0x1F);
	while (i + 16 <= n) {
		__m128i const v = _mm_loadu_si128(reinterpret_cast<__m128i const *>(p + i));
		__m128i const eq_q = _mm_cmpeq_epi8(v, v_quote);
		__m128i const eq_b = _mm_cmpeq_epi8(v, v_back);
		__m128i mix = _mm_or_si128(eq_q, eq_b);
		if (ascii_only) {
			mix = _mm_or_si128(mix, _mm_cmplt_epi8(v, v_lim));
		} else {
			mix = _mm_or_si128(mix, _mm_cmpeq_epi8(_mm_min_epu8(v, v_1f), v));
		}
		auto const mask = static_cast<unsigned>(_mm_movemask_epi8(mix));
		if (mask != 0U) {
			return i + static_cast<std::size_t>(__builtin_ctz(mask));
		}
		i += 16;
	}
#endif
	for (; i < n; ++i) {
		auto const c = static_cast<unsigned char>(p[i]);
		if (c == '"' || c == '\\' || c < 0x20U) {
			return i;
		}
		if (ascii_only && c >= 0x80U) {
			return i;
		}
	}
	return n;
}

extern "C" std::size_t conflux_json_scan_dump_safe_run_auto(
	char const *p,
	std::size_t n,
	int ascii_only) noexcept {
	return scan_dump_safe_run(p, n, ascii_only != 0);
}

void dump_str(
	std::string_view sv,
	std::string &out,
	bool ascii_only) {
	out += '"';
	std::size_t i = 0;
	while (i < sv.size()) {
		auto const c = static_cast<unsigned char>(sv[i]);
		// Scalar pre-check: when the very next std::byte already needs escaping,
		// skip the SIMD chunk setup entirely. Avoids paying SIMD cost on
		// escape-dense payloads where every other std::byte is an escape.
		bool const needs_escape = (c == '"' || c == '\\' || c < 0x20U || (ascii_only && c >= 0x80U));
		if (!needs_escape) {
			// R3 — fast-forward over the safe-ASCII run.
			std::size_t const run = scan_dump_safe_run(sv.data() + i, sv.size() - i, ascii_only);
			out.append(sv.data() + i, run);
			i += run;
			if (i >= sv.size()) {
				break;
			}
		}
		auto const cc = static_cast<unsigned char>(sv[i]);
		switch (cc) {
		case '"':
			out += "\\\"";
			++i;
			break;
		case '\\':
			out += "\\\\";
			++i;
			break;
		case '\b':
			out += "\\b";
			++i;
			break;
		case '\f':
			out += "\\f";
			++i;
			break;
		case '\n':
			out += "\\n";
			++i;
			break;
		case '\r':
			out += "\\r";
			++i;
			break;
		case '\t':
			out += "\\t";
			++i;
			break;
		default:
			if (cc < 0x20U) {
				append_u_escape(out, cc);
				++i;
			} else if (ascii_only && cc >= 0x80U) {
				// Decode UTF-8 to get code point, then emit \uXXXX or surrogate P.
				std::uint32_t cp = 0;
				std::size_t seq = 0;
				if (cc < 0xE0U) {
					cp = cc & 0x1FU;
					seq = 2;
				} else if (cc < 0xF0U) {
					cp = cc & 0x0FU;
					seq = 3;
				} else {
					cp = cc & 0x07U;
					seq = 4;
				}
				for (std::size_t k = 1; k < seq && i + k < sv.size(); ++k) {
					cp = (cp << 6U) | (static_cast<unsigned char>(sv[i + k]) & 0x3FU);
				}
				i += seq;
				if (cp < 0x10000U) {
					append_u_escape(out, cp);
				} else {
					cp -= 0x10000U;
					append_u_escape(out, 0xD800U | (cp >> 10U));
					append_u_escape(out, 0xDC00U | (cp & 0x3FFU));
				}
			} else {
				out += static_cast<char>(cc);
				++i;
			}
		}
	}
	out += '"';
}
// NOLINTEND(readability-magic-numbers)

// NOLINTNEXTLINE(misc-no-recursion)
void dump_node(
	DocumentStorage const &store,
	std::size_t node_idx,
	JsonDumpOptions const &opts,
	unsigned depth,
	std::string &out) {
	if (opts.truncate_depth.has_value() && static_cast<std::size_t>(depth) > *opts.truncate_depth) {
		out += "null";
		return;
	}
	// NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-constant-A-index)
	auto const &n = store.nodes[node_idx];
	auto indent = [&](unsigned d) {
		if (!opts.pretty) {
			return;
		}
		out += '\n';
		out.append(static_cast<std::size_t>(d) * opts.indent, opts.indent_char);
	};

	switch (n.kind) {
	case NodeKind::null_  : out += "null"; break;
	case NodeKind::boolean: out += n.bool_val ? "true" : "false"; break;
	case NodeKind::string_:
		{
			auto const bytes = store.bytes_at(n.off, n.len, n.flags);
			if ((n.flags & kRawJsonSlice) != 0 && !opts.ascii_only) {
				dump_str_raw(bytes, out);
			} else {
				dump_str(bytes, out, opts.ascii_only);
			}
			break;
		}
	case NodeKind::number: out += store.bytes_at(n.off, n.len, n.flags); break;
	case NodeKind::array_:
		{
			out += '[';
			if (n.len > 0) {
				for (std::size_t i = 0; i < n.len; ++i) {
					if (i > 0) {
						out += ',';
					}
					indent(depth + 1);
					// NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-constant-A-index)
					dump_node(store, store.array_children[n.off + i], opts, depth + 1, out);
				}
				indent(depth);
			}
			out += ']';
			break;
		}
	case NodeKind::object:
		{
			out += '{';
			if (n.len > 0) {
				auto dump_member = [&](auto const &m, std::string_view name) {
					if ((m.name_flags & kRawJsonSlice) != 0 && !opts.ascii_only) {
						dump_str_raw(name, out);
					} else {
						dump_str(name, out, opts.ascii_only);
					}
					out += opts.pretty ? ": " : ":";
					dump_node(store, m.val_node, opts, depth + 1, out);
				};
				// R3 — only allocate the order V when sorting; the
				// unsorted path iterates members in source order directly.
				if (opts.sort_object_keys) {
					std::vector<std::pair<std::string_view, std::size_t>> order;
					order.reserve(n.len);
					for (std::size_t i = 0; i < n.len; ++i) {
						// NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-constant-A-index)
						auto const &m = store.object_members[n.off + i];
						order.emplace_back(store.member_name(m), i);
					}
					std::ranges::sort(order, {}, &std::pair<std::string_view, std::size_t>::first);
					for (std::size_t i = 0; i < n.len; ++i) {
						if (i > 0) {
							out += ',';
						}
						indent(depth + 1);
						// NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-constant-A-index)
						auto const &m = store.object_members[n.off + order[i].second];
						dump_member(m, order[i].first);
					}
				} else {
					for (std::size_t i = 0; i < n.len; ++i) {
						if (i > 0) {
							out += ',';
						}
						indent(depth + 1);
						// NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-constant-A-index)
						auto const &m = store.object_members[n.off + i];
						dump_member(m, store.member_name(m));
					}
				}
				indent(depth);
			}
			out += '}';
			break;
		}
	}
}

std::expected<std::string, JsonError> Document::dump(
	JsonDumpOptions const &opts) const {
	std::string out;
	// R3 — skip the small-buffer doubling cycle. Empirically dump output
	// is roughly 1.05–1.2x the input size for compact corpora and within
	// 3x for pretty-printed; reserve from string_arena + nodes count.
	out.reserve(storage_->input_view.size() + storage_->string_arena.size() + 32);
	dump_node(*storage_, storage_->root_node, opts, 0, out);
	return out;
}
std::expected<std::string, JsonError> ArenaDocument::dump(
	JsonDumpOptions const &opts) const {
	check_live();
	std::string out;
	out.reserve(storage_->input_view.size() + storage_->string_arena.size() + 32);
	dump_node(*storage_, storage_->root_node, opts, 0, out);
	return out;
}
