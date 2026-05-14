// Work executor example: submit CPU work to a WorkPool, aggregate with join_all,
// and handle exceptions through sync_wait.
import conflux.work;
import conflux.types;
import std;

static bool is_prime(i64 n) {
	if (n < 2) {
		return false;
	}
	for (i64 d = 2; d * d <= n; ++d) {
		if (n % d == 0) {
			return false;
		}
	}
	return true;
}

static i64 count_primes(i64 first, i64 last) {
	i64 count = 0;
	for (i64 n = first; n < last; ++n) {
		count += is_prime(n) ? 1 : 0;
	}
	return count;
}

int main() {
	WorkPool pool{WorkPoolOptions{.threads = 4, .max_inject_queue = 128, .worker_name_prefix = "cf-example"}};

	auto counts = join_all(
		run_on_task(pool, [] { return count_primes(2, 25'000); }),
		run_on_task(pool, [] { return count_primes(25'000, 50'000); }),
		run_on_task(pool, [] { return count_primes(50'000, 75'000); }),
		run_on_task(pool, [] { return count_primes(75'000, 100'000); }));

	auto [a, b, c, d] = sync_wait(move(counts));
	println("primes below 100000: {}", a + b + c + d);

	try {
		(void)sync_wait(run_on_task(pool, []() -> i64 { throw RE{"worker-side failure"}; }));
	} catch (exception const &e) {
		println("failure propagated through task outcome: {}", e.what());
	}

	pool.drain_and_stop();
}
