import conflux.work;
import conflux.work.race;
import std;

namespace root = conflux::work::root;
namespace race = conflux::work::race;

struct SourceStats {
	std::uint64_t wins = 0;
	std::uint64_t failures = 0;
	std::uint64_t cancel_requests = 0;
	std::chrono::nanoseconds total_latency{};
};

static void record_result(
	std::unordered_map<std::string, SourceStats> &stats,
	race::race_result<int> const &result) {
	auto &entry = stats[std::string{result.winner.label}];
	if (result.outcome.is_success()) {
		++entry.wins;
		entry.total_latency += result.winner.latency;
	} else {
		++entry.failures;
	}
}

int main() {
	std::unordered_map<std::string, SourceStats> stats;
	auto &db_stats = stats["db"];
	auto &network_stats = stats["network"];
	auto &disk_stats = stats["disk"];

	for (int key = 0; key != 9; ++key) {
		auto [db, db_src] = root::make_cancellable_task_source<int>(
			[&db_stats](root::CancelReason) noexcept { ++db_stats.cancel_requests; });
		auto [network, network_src] = root::make_cancellable_task_source<int>(
			[&network_stats](root::CancelReason) noexcept { ++network_stats.cancel_requests; });
		auto [disk, disk_src] = root::make_cancellable_task_source<int>(
			[&disk_stats](root::CancelReason) noexcept { ++disk_stats.cancel_requests; });

		auto result_task = race::race<int>(
			race::race_options{
				.winner = race::winner_policy::first_success,
				.losers = race::loser_policy::request_cancel_and_wait,
				.preserve_winner_latency = true,
			},
			race::candidate("db", std::move(db)),
			race::candidate("network", std::move(network)),
			race::candidate("disk", std::move(disk)));

		switch (key % 3) {
		case 0:
			(void)db_src.try_set_value(root::Success<int>{key});
			(void)network_src.try_set_cancelled(root::CancelReason::requested);
			(void)disk_src.try_set_cancelled(root::CancelReason::requested);
			break;
		case 1:
			(void)network_src.try_set_value(root::Success<int>{key});
			(void)db_src.try_set_cancelled(root::CancelReason::requested);
			(void)disk_src.try_set_cancelled(root::CancelReason::requested);
			break;
		default:
			(void)disk_src.try_set_value(root::Success<int>{key});
			(void)db_src.try_set_cancelled(root::CancelReason::requested);
			(void)network_src.try_set_cancelled(root::CancelReason::requested);
			break;
		}

		record_result(stats, root::value(std::move(result_task)));
	}

	for (auto const &[name, source]: stats) {
		auto const avg_ns =
			source.wins == 0 ? 0 : source.total_latency.count() / static_cast<std::int64_t>(source.wins);
		std::println(
			"{} wins={} failures={} cancel_requests={} avg={}ns",
			name,
			source.wins,
			source.failures,
			source.cancel_requests,
			avg_ns);
	}
}
