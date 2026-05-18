// Plain TU — deterministic SEND_ZC CQE state-machine coverage; no live ring.
#include <catch2/catch_test_macros.hpp>

import conflux.types;
import conflux.net.http_server;

TEST_CASE(
	"send_zc lifecycle: data CQE waits for notification before resubmit",
	"[send_zc][http_server]") {
	SendZcCqeState state{};
	SendZcMetrics metrics{};
	bool enabled = true;

	auto first = observe_send_zc_cqe(
		state,
		metrics,
		SendZcCqeInput{
			.result = 512,
			.more = true,
			.written_before = 0,
			.response_total = 1024,
		},
		enabled);
	CHECK(first.action == SendZcCqeAction::none);
	CHECK(first.bytes_sent == 512);
	CHECK(metrics.bytes_sent == 512);
	CHECK(state.waiting_notification);
	CHECK(state.after_notification == SendZcPendingAction::resubmit_response);

	auto notif = observe_send_zc_cqe(
		state,
		metrics,
		SendZcCqeInput{
			.notification = true,
			.written_before = 512,
			.response_total = 1024,
		},
		enabled);
	CHECK(notif.action == SendZcCqeAction::resubmit_response);
	CHECK(!state.waiting_notification);
	CHECK(state.after_notification == SendZcPendingAction::none);
	CHECK(metrics.notifications == 1);
	CHECK(enabled);
}

TEST_CASE(
	"send_zc lifecycle: copied notification can disable adaptive SEND_ZC",
	"[send_zc][http_server]") {
	SendZcCqeState state{};
	SendZcMetrics metrics{};
	metrics.attempts = 1024;
	metrics.bytes_requested = std::size_t{16} * 1024 * 1024;
	metrics.notifications = 1023;
	metrics.copied_notifications = 921;
	bool enabled = true;

	auto data = observe_send_zc_cqe(
		state,
		metrics,
		SendZcCqeInput{
			.result = 1024,
			.more = true,
			.written_before = 0,
			.response_total = 1024,
		},
		enabled);
	CHECK(data.action == SendZcCqeAction::none);
	CHECK(state.after_notification == SendZcPendingAction::complete_response);

	auto notif = observe_send_zc_cqe(
		state,
		metrics,
		SendZcCqeInput{
			.notification = true,
			.copied = true,
			.written_before = 1024,
			.response_total = 1024,
		},
		enabled);
	CHECK(notif.action == SendZcCqeAction::complete_response);
	CHECK(notif.adaptive_disabled);
	CHECK(!enabled);
	CHECK(metrics.notifications == 1024);
	CHECK(metrics.copied_notifications == 922);
	CHECK(metrics.adaptive_disable_count == 1);
}

TEST_CASE(
	"send_zc lifecycle: no-notification data CQE completes directly",
	"[send_zc][http_server]") {
	SendZcCqeState state{};
	SendZcMetrics metrics{};
	bool enabled = true;

	auto partial = observe_send_zc_cqe(
		state,
		metrics,
		SendZcCqeInput{
			.result = 256,
			.written_before = 0,
			.response_total = 512,
		},
		enabled);
	CHECK(partial.action == SendZcCqeAction::resubmit_response);
	CHECK(partial.bytes_sent == 256);
	CHECK(metrics.sends_without_notification == 1);
	CHECK(!state.waiting_notification);

	auto done = observe_send_zc_cqe(
		state,
		metrics,
		SendZcCqeInput{
			.result = 256,
			.written_before = 256,
			.response_total = 512,
		},
		enabled);
	CHECK(done.action == SendZcCqeAction::complete_response);
	CHECK(done.bytes_sent == 256);
	CHECK(metrics.bytes_sent == 512);
	CHECK(metrics.sends_without_notification == 2);
}

TEST_CASE(
	"send_zc lifecycle: error paths account ENOMEM and wait for notification when required",
	"[send_zc][http_server]") {
	SendZcCqeState state{};
	SendZcMetrics metrics{};
	bool enabled = true;

	auto data_error = observe_send_zc_cqe(
		state,
		metrics,
		SendZcCqeInput{
			.result = -1,
			.more = true,
			.enomem_error = true,
			.written_before = 0,
			.response_total = 1024,
		},
		enabled);
	CHECK(data_error.action == SendZcCqeAction::none);
	CHECK(state.waiting_notification);
	CHECK(state.after_notification == SendZcPendingAction::close_after_error);
	CHECK(metrics.errors_enomem == 1);

	auto notif = observe_send_zc_cqe(
		state,
		metrics,
		SendZcCqeInput{
			.notification = true,
			.written_before = 0,
			.response_total = 1024,
		},
		enabled);
	CHECK(notif.action == SendZcCqeAction::close_after_error);
	CHECK(!state.waiting_notification);
	CHECK(state.after_notification == SendZcPendingAction::none);
}

TEST_CASE(
	"send_zc lifecycle: queued close waits until notification CQE arrives",
	"[send_zc][http_server]") {
	SendZcCqeState state{};
	state.waiting_notification = true;
	state.close_after_notification = true;
	state.after_notification = SendZcPendingAction::complete_response;
	SendZcMetrics metrics{};
	bool enabled = true;

	auto notif = observe_send_zc_cqe(
		state,
		metrics,
		SendZcCqeInput{
			.notification = true,
			.written_before = 1024,
			.response_total = 1024,
		},
		enabled);
	CHECK(notif.action == SendZcCqeAction::close_after_notification);
	CHECK(!state.waiting_notification);
	CHECK(!state.close_after_notification);
	CHECK(state.after_notification == SendZcPendingAction::none);
	CHECK(metrics.notifications == 1);
}
