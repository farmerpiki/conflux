// Work executor example: submit CPU work to a WorkPool, aggregate with join_all,
// and handle exceptions through sync_wait.
import conflux.work;
import conflux.types;
import std;

static bool is_prime(
	std::int64_t n) {
	if (n < 2) {
		return false;
	}
	for (std::int64_t d = 2; d * d <= n; ++d) {
		if (n % d == 0) {
			return false;
		}
	}
	return true;
}

static std::int64_t count_primes(
	std::int64_t first,
	std::int64_t last) {
	std::int64_t count = 0;
	for (std::int64_t n = first; n < last; ++n) {
		count += is_prime(n) ? 1 : 0;
	}
	return count;
}

int main() {
	WorkPool pool{
		WorkPoolOptions{.threads = 4, .max_inject_queue = 128, .worker_name_prefix = "cf-example"}
    };

	auto counts = join_all(
		async_run_on(pool, [] { return count_primes(2, 25'000); }),
		async_run_on(pool, [] { return count_primes(25'000, 50'000); }),
		async_run_on(pool, [] { return count_primes(50'000, 75'000); }),
		async_run_on(pool, [] { return count_primes(75'000, 100'000); }));

	auto [a, b, c, d] = sync_wait(std::move(counts));
	std::println("primes below 100000: {}", a + b + c + d);

	try {
		(void)sync_wait(async_run_on(pool, []() -> std::int64_t { throw std::runtime_error{"worker-side failure"}; }));
	} catch (std::exception const &e) { std::println("failure propagated through task outcome: {}", e.what()); }

	pool.drain_and_stop();
}
