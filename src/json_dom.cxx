module conflux.json;

import std;
import std.compat;
import conflux.types;

// ---------------------------------------------------------------------------
// JsonPath::from_pointer (after JsonError definition)
// ---------------------------------------------------------------------------

expected<JsonPath, JsonError> JsonPath::from_pointer(
	SV sv) {
	if (sv.empty()) {
		return JsonPath{};
	}
	if (sv[0] != '/') {
		return unexpected(
			JsonError{
				.stage = JsonStage::parse,
				.code = JsonIssueCode::invalid_pointer,
				.message = "JSON Pointer must start with '/' or be empty"});
	}
	JsonPath result;
	SZ pos = 1;
	while (pos <= sv.size()) {
		SZ slash = sv.find('/', pos);
		if (slash == SV::npos) {
			slash = sv.size();
		}
		S name;
		name.reserve(slash - pos);
		for (SZ i = pos; i < slash; ++i) {
			if (sv[i] == '~') {
				if (i + 1 >= slash) {
					return unexpected(
						JsonError{
							.stage = JsonStage::parse,
							.code = JsonIssueCode::invalid_pointer,
							.message = "invalid '~' escape in JSON Pointer"});
				}
				++i;
				if (sv[i] == '0') {
					name += '~';
				} else if (sv[i] == '1') {
					name += '/';
				} else {
					return unexpected(
						JsonError{
							.stage = JsonStage::parse,
							.code = JsonIssueCode::invalid_pointer,
							.message = "invalid '~' escape in JSON Pointer"});
				}
			} else {
				name += sv[i];
			}
		}
		result.push_member(name);
		pos = slash + 1;
	}
	return result;
}
// ---------------------------------------------------------------------------
// Implement NodeRef methods that need ObjectView/ArrayView
// ---------------------------------------------------------------------------

expected<ObjectView, JsonError> NodeRef::as_object() const {
	if (rec().kind != NodeKind::object) {
		return unexpected(
			JsonError{
				.stage = JsonStage::lookup,
				.code = JsonIssueCode::wrong_kind,
				.expected_kind = JsonKind::object,
				.actual_kind = kind(),
				.message = "expected object"});
	}
	return ObjectView{storage_, rec().off, rec().len, idx_};
}
expected<ArrayView, JsonError> NodeRef::as_array() const {
	if (rec().kind != NodeKind::array_) {
		return unexpected(
			JsonError{
				.stage = JsonStage::lookup,
				.code = JsonIssueCode::wrong_kind,
				.expected_kind = JsonKind::array,
				.actual_kind = kind(),
				.message = "expected array"});
	}
	return ArrayView{storage_, rec().off, rec().len};
}
void push_seg(
	JsonPath &p,
	JsonPathSegment const &s) {
	if (holds_alternative<JsonPathMember>(s)) {
		p.push_member(get<JsonPathMember>(s).name);
	} else {
		p.push_index(get<JsonPathIndex>(s).index);
	}
}
expected<NodeRef, JsonError> NodeRef::at_pointer(
	SV pointer) const {
	auto path = JsonPath::from_pointer(pointer);
	if (!path) {
		return unexpected(move(path).error());
	}
	return at(*path);
}
expected<NodeRef, JsonError> NodeRef::at(
	JsonPath const &path) const {
	NodeRef cur = *this;
	for (SZ i = 0; i < path.size(); ++i) {
		auto const &seg = path.segment(i);
		auto set_path = [&](JsonError err) {
			err.path = JsonPath{};
			for (SZ j = 0; j <= i; ++j) {
				push_seg(err.path, path.segment(j));
			}
			return unexpected(move(err));
		};
		if (holds_alternative<JsonPathMember>(seg)) {
			auto const &name = get<JsonPathMember>(seg).name;
			if (cur.kind() == JsonKind::array) {
				bool all_digits = !name.empty() && (name.size() == 1 || name[0] != '0');
				for (SZ k = 0; all_digits && k < name.size(); ++k) {
					all_digits = name[k] >= '0' && name[k] <= '9';
				}
				if (!all_digits) {
					return set_path(
						JsonError{
							.stage = JsonStage::lookup,
							.code = JsonIssueCode::wrong_kind,
							.expected_kind = JsonKind::object,
							.actual_kind = JsonKind::array,
							.message = "non-numeric JSON Pointer segment on array"});
				}
				SZ idx = 0;
				for (char const ch: name) {
					idx = idx * 10 + static_cast<SZ>(ch - '0');
				}
				auto arr = cur.as_array();
				if (!arr) {
					return set_path(move(arr).error());
				}
				auto child = arr->element(idx);
				if (!child) {
					return set_path(move(child).error());
				}
				cur = *child;
			} else {
				auto obj = cur.as_object();
				if (!obj) {
					return set_path(move(obj).error());
				}
				auto child = obj->member(name);
				if (!child) {
					return set_path(move(child).error());
				}
				cur = *child;
			}
		} else {
			auto arr = cur.as_array();
			if (!arr) {
				return set_path(move(arr).error());
			}
			auto child = arr->element(get<JsonPathIndex>(seg).index);
			if (!child) {
				return set_path(move(child).error());
			}
			cur = *child;
		}
	}
	return cur;
}
ObjectMemberRange ObjectView::members() const noexcept {
	return {storage_, mem_start_, mem_count_};
}
ArrayElementRange ArrayView::elements() const noexcept {
	return {storage_, child_start_, child_count_};
}
