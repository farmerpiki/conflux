import conflux.work;
import conflux.work.race;
import std;

namespace root = conflux::work::root;
namespace race = conflux::work::race;
using namespace std::chrono_literals;

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

static int fetch_simulated(
	int key,
	std::chrono::milliseconds latency,
	root::Cancellation cancel,
	SourceStats &stats) {
	auto const deadline = std::chrono::steady_clock::now() + latency;
	while (std::chrono::steady_clock::now() < deadline) {
		if (cancel.requested()) {
			++stats.cancel_requests;
			cancel.throw_if_requested();
		}
		std::this_thread::sleep_for(1ms);
	}
	return key;
}

int main() {
	WorkPool pool{
		WorkPoolOptions{.threads = 4, .max_inject_queue = 128, .worker_name_prefix = "race-example"}
    };
	std::unordered_map<std::string, SourceStats> stats;
	auto &db_stats = stats["db"];
	auto &network_stats = stats["network"];
	auto &disk_stats = stats["disk"];

	for (int key = 0; key != 9; ++key) {
		auto db = async_run_cancellable_on(pool, [key, &db_stats](root::Cancellation cancel) {
			return fetch_simulated(key, 3ms, cancel, db_stats);
		});
		auto network = async_run_cancellable_on(pool, [key, &network_stats](root::Cancellation cancel) {
			return fetch_simulated(key, 6ms, cancel, network_stats);
		});
		auto disk = async_run_cancellable_on(pool, [key, &disk_stats](root::Cancellation cancel) {
			return fetch_simulated(key, 9ms, cancel, disk_stats);
		});

		auto result_task = race::race<int>(
			race::race_options{
				.winner = race::winner_policy::first_success,
				.losers = race::loser_policy::request_cancel_and_wait,
				.preserve_winner_latency = true,
			},
			race::candidate("db", std::move(db)),
			race::candidate("network", std::move(network)),
			race::candidate("disk", std::move(disk)));

		record_result(stats, root::value(std::move(result_task)));
	}

	pool.drain_and_stop();

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
