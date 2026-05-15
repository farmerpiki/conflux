module;
#include <ctime>
export module conflux.net.router_match;

import std;
import conflux.types;
import conflux.net.http.types;
import conflux.utils;

export struct Segment {
	S value;
	bool is_param; // true -> {name} single-segment capture
	bool is_wildcard; // true -> {*name} greedy tail capture (must be last segment)
};

export V<Segment> parse_pattern(
	SV pattern) {
	V<Segment> segs;
	segs.reserve(ranges::count(pattern, '/') + 1);
	SZ pos = 0;
	while (true) {
		auto next = pattern.find('/', pos);
		auto part = (next == SV::npos) ? pattern.substr(pos) : pattern.substr(pos, next - pos);

		if (part.size() >= 3 && part.front() == '{' && part.back() == '}' && part[1] == '*') {
			segs.push_back({S{part.substr(2, part.size() - 3)}, false, true});
		} else if (part.size() >= 2 && part.front() == '{' && part.back() == '}') {
			segs.push_back({S{part.substr(1, part.size() - 2)}, true, false});
		} else {
			segs.push_back({S{part}, false, false});
		}

		if (next == SV::npos) {
			break;
		}
		pos = next + 1;
	}
	return segs;
}

export bool match_segments(
	V<Segment> const &pattern,
	SV path,
	HttpFieldsView &out_params) {
	// Wildcard tail: last segment {*name} matches everything remaining.
	if (!pattern.empty() && pattern.back().is_wildcard) {
		// Match all non-wildcard leading segments first.
		auto prefix_count = pattern.size() - 1;
		SZ pos = 0;
		HttpFieldsView tmp;
		for (SZ i = 0; i < prefix_count; ++i) {
			if (pos >= path.size()) {
				return false;
			}
			auto next = path.find('/', pos);
			auto part = (next == SV::npos) ? path.substr(pos) : path.substr(pos, next - pos);
			if (next == SV::npos && i + 1 < prefix_count) {
				return false;
			}
			if (pattern[i].is_param) {
				tmp.emplace_back_owned(S{pattern[i].value}, url_decode_path(part));
			} else if (pattern[i].value != part) {
				return false;
			}
			pos = (next == SV::npos) ? path.size() : next + 1;
		}
		// Capture the remainder (may be empty for trailing slash).
		tmp.emplace_back_owned(S{pattern.back().value}, url_decode_path(path.substr(pos)));
		out_params = move(tmp);
		return true;
	}

	SZ pos = 0;
	SZ i = 0;
	HttpFieldsView tmp;
	while (true) {
		if (i >= pattern.size()) {
			return false;
		}
		auto next = path.find('/', pos);
		auto part = (next == SV::npos) ? path.substr(pos) : path.substr(pos, next - pos);
		if (pattern[i].is_param) {
			tmp.emplace_back_owned(S{pattern[i].value}, url_decode_path(part));
		} else if (pattern[i].value != part) {
			return false;
		}
		++i;
		if (next == SV::npos) {
			break;
		}
		pos = next + 1;
	}

	if (i != pattern.size()) {
		return false;
	}
	out_params = move(tmp);
	return true;
}

export S segments_to_pattern(
	V<Segment> const &segs) {
	S out;
	bool first = true;
	for (auto const &seg: segs) {
		if (first && seg.value.empty() && !seg.is_param && !seg.is_wildcard) {
			first = false;
			continue;
		}
		first = false;
		out += '/';
		if (seg.is_wildcard || seg.is_param) {
			out += '{';
			out += seg.value;
			out += '}';
		} else {
			out += seg.value;
		}
	}
	if (out.empty()) {
		out = "/";
	}
	return out;
}
