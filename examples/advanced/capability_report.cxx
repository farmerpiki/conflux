import std;
import conflux.types;
import conflux.uring;

int main() {
	std::println("{}", conflux::build_info_summary());

	auto caps = conflux::runtime::detect_capabilities();
	if (!caps) {
		auto const &issue = caps.error();
		std::println(
			"capability issue {} {}: {}",
			conflux::runtime::capability_issue_code_string(issue.code),
			issue.feature,
			issue.message);
		if (!issue.hint.empty()) {
			std::println("hint: {}", issue.hint);
		}
		return 1;
	}

	std::print("{}", conflux::runtime::capability_report(*caps));
}
