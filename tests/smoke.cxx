module;

#include <catch2/catch_test_macros.hpp>

export module conflux.tests.smoke;

import conflux;
TEST_CASE(
	"smoke") {
	SUCCEED();
}
