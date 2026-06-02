if(TARGET conflux)
    conflux_add_compile_fail_test(
        TARGET conflux_api_surface_curated_compile_fail_workpool
        SOURCE api_surface_curated_compile_fail_workpool.cxx
        TEST api-surface/curated-hides-workpool
        LINK conflux conflux_options
        LABELS build api-surface compile-fail
        EXPECT "conflux_api_surface_curated_unexpected_workpool_visible")

    conflux_add_compile_fail_test(
        TARGET conflux_api_surface_curated_compile_fail_iouring
        SOURCE api_surface_curated_compile_fail_iouring.cxx
        TEST api-surface/curated-hides-iouring
        LINK conflux conflux_options
        LABELS build api-surface compile-fail
        EXPECT "conflux::uring")

    conflux_add_compile_fail_test(
        TARGET conflux_api_surface_curated_compile_fail_file
        SOURCE api_surface_curated_compile_fail_file.cxx
        TEST api-surface/curated-hides-blocking-file
        LINK conflux conflux_options
        LABELS build api-surface compile-fail
        EXPECT "conflux::http" "file")

    conflux_add_compile_fail_test(
        TARGET conflux_api_surface_extended_compile_fail_iouring
        SOURCE api_surface_extended_compile_fail_iouring.cxx
        TEST api-surface/extended-hides-iouring
        LINK conflux conflux_options
        LABELS build api-surface compile-fail
        EXPECT "conflux::uring")

    conflux_add_compile_fail_test(
        TARGET conflux_api_surface_complete_compile_fail_direct_slot_pool
        SOURCE api_surface_complete_compile_fail_direct_slot_pool.cxx
        TEST api-surface/complete-hides-direct-slot-pool
        LINK conflux conflux_options
        LABELS build api-surface compile-fail
        EXPECT "DirectSlotPool")

    conflux_add_compile_fail_test(
        TARGET conflux_crypto_compile_fail_detail
        SOURCE crypto_compile_fail_detail.cxx
        TEST api-surface/crypto-hides-detail
        LINK conflux_crypto conflux_options
        LABELS build api-surface compile-fail
        EXPECT "conflux::crypto::detail")

    conflux_add_compile_fail_test(
        TARGET conflux_crypto_compile_fail_global_base64_encode
        SOURCE crypto_compile_fail_global_base64_encode.cxx
        TEST api-surface/crypto-hides-global-base64-encode
        LINK conflux_crypto conflux_options
        LABELS build api-surface compile-fail
        EXPECT "base64_encode")

    conflux_add_compile_fail_test(
        TARGET conflux_crypto_compile_fail_global_base64_decode
        SOURCE crypto_compile_fail_global_base64_decode.cxx
        TEST api-surface/crypto-hides-global-base64-decode
        LINK conflux_crypto conflux_options
        LABELS build api-surface compile-fail
        EXPECT "base64_decode")

    conflux_add_compile_fail_test(
        TARGET conflux_crypto_compile_fail_global_base64url_encode
        SOURCE crypto_compile_fail_global_base64url_encode.cxx
        TEST api-surface/crypto-hides-global-base64url-encode
        LINK conflux_crypto conflux_options
        LABELS build api-surface compile-fail
        EXPECT "base64url_encode")

    conflux_add_compile_fail_test(
        TARGET conflux_crypto_compile_fail_global_base64url_decode
        SOURCE crypto_compile_fail_global_base64url_decode.cxx
        TEST api-surface/crypto-hides-global-base64url-decode
        LINK conflux_crypto conflux_options
        LABELS build api-surface compile-fail
        EXPECT "base64url_decode")

    conflux_add_compile_fail_test(
        TARGET conflux_crypto_compile_fail_global_hmac_sha256_key
        SOURCE crypto_compile_fail_global_hmac_sha256_key.cxx
        TEST api-surface/crypto-hides-global-hmac-sha256-key
        LINK conflux_crypto conflux_options
        LABELS build api-surface compile-fail
        EXPECT "HmacSha256Key")

    conflux_add_compile_fail_test(
        TARGET conflux_crypto_compile_fail_global_hmac_sha256_precomputed
        SOURCE crypto_compile_fail_global_hmac_sha256_precomputed.cxx
        TEST api-surface/crypto-hides-global-hmac-sha256-precomputed
        LINK conflux_crypto conflux_options
        LABELS build api-surface compile-fail
        EXPECT "hmac_sha256_precomputed")

    conflux_add_compile_fail_test(
        TARGET conflux_crypto_compile_fail_global_sha1
        SOURCE crypto_compile_fail_global_sha1.cxx
        TEST api-surface/crypto-hides-global-sha1
        LINK conflux_crypto conflux_options
        LABELS build api-surface compile-fail
        EXPECT "sha1")

    conflux_add_compile_fail_test(
        TARGET conflux_crypto_compile_fail_global_sha256
        SOURCE crypto_compile_fail_global_sha256.cxx
        TEST api-surface/crypto-hides-global-sha256
        LINK conflux_crypto conflux_options
        LABELS build api-surface compile-fail
        EXPECT "sha256")

    conflux_add_compile_fail_test(
        TARGET conflux_crypto_compile_fail_global_hmac_sha1
        SOURCE crypto_compile_fail_global_hmac_sha1.cxx
        TEST api-surface/crypto-hides-global-hmac-sha1
        LINK conflux_crypto conflux_options
        LABELS build api-surface compile-fail
        EXPECT "hmac_sha1")

    conflux_add_compile_fail_test(
        TARGET conflux_crypto_compile_fail_global_hmac_sha256
        SOURCE crypto_compile_fail_global_hmac_sha256.cxx
        TEST api-surface/crypto-hides-global-hmac-sha256
        LINK conflux_crypto conflux_options
        LABELS build api-surface compile-fail
        EXPECT "hmac_sha256")

    conflux_add_compile_fail_test(
        TARGET conflux_crypto_compile_fail_global_aes_gcm_encrypt
        SOURCE crypto_compile_fail_global_aes_gcm_encrypt.cxx
        TEST api-surface/crypto-hides-global-aes-gcm-encrypt
        LINK conflux_crypto conflux_options
        LABELS build api-surface compile-fail
        EXPECT "aes_gcm_encrypt")

    conflux_add_compile_fail_test(
        TARGET conflux_crypto_compile_fail_global_aes_gcm_decrypt
        SOURCE crypto_compile_fail_global_aes_gcm_decrypt.cxx
        TEST api-surface/crypto-hides-global-aes-gcm-decrypt
        LINK conflux_crypto conflux_options
        LABELS build api-surface compile-fail
        EXPECT "aes_gcm_decrypt")

    conflux_add_compile_fail_test(
        TARGET conflux_utils_compile_fail_global_url_decode
        SOURCE utils_compile_fail_global_url_decode.cxx
        TEST api-surface/utils-hides-global-url-decode
        LINK conflux_utils conflux_options
        LABELS build api-surface compile-fail
        EXPECT "url_decode")

    conflux_add_compile_fail_test(
        TARGET conflux_utils_compile_fail_global_http_status
        SOURCE utils_compile_fail_global_http_status.cxx
        TEST api-surface/utils-hides-global-http-status
        LINK conflux_utils conflux_options
        LABELS build api-surface compile-fail
        EXPECT "kHttpOk")

    conflux_add_compile_fail_test(
        TARGET conflux_utils_compile_fail_global_hex_char_to_int
        SOURCE utils_compile_fail_global_hex_char_to_int.cxx
        TEST api-surface/utils-hides-global-hex-char-to-int
        LINK conflux_utils conflux_options
        LABELS build api-surface compile-fail
        EXPECT "hex_char_to_int")

    conflux_add_compile_fail_test(
        TARGET conflux_utils_compile_fail_global_json_string_fallback
        SOURCE utils_compile_fail_global_json_string_fallback.cxx
        TEST api-surface/utils-hides-global-json-string-fallback
        LINK conflux_utils conflux_options
        LABELS build api-surface compile-fail
        EXPECT "json_string_fallback")

    conflux_add_compile_fail_test(
        TARGET conflux_utils_compile_fail_global_json_string_content_fallback
        SOURCE utils_compile_fail_global_json_string_content_fallback.cxx
        TEST api-surface/utils-hides-global-json-string-content-fallback
        LINK conflux_utils conflux_options
        LABELS build api-surface compile-fail
        EXPECT "json_string_content_fallback")

    conflux_add_compile_fail_test(
        TARGET conflux_utils_compile_fail_global_ip_addr
        SOURCE utils_compile_fail_global_ip_addr.cxx
        TEST api-surface/utils-hides-global-ip-addr
        LINK conflux_utils conflux_options
        LABELS build api-surface compile-fail
        EXPECT "IpAddr")

    conflux_add_compile_fail_test(
        TARGET conflux_utils_compile_fail_global_ip_cidr
        SOURCE utils_compile_fail_global_ip_cidr.cxx
        TEST api-surface/utils-hides-global-ip-cidr
        LINK conflux_utils conflux_options
        LABELS build api-surface compile-fail
        EXPECT "IpCidr")

    conflux_add_compile_fail_test(
        TARGET conflux_utils_compile_fail_global_parse_ip
        SOURCE utils_compile_fail_global_parse_ip.cxx
        TEST api-surface/utils-hides-global-parse-ip
        LINK conflux_utils conflux_options
        LABELS build api-surface compile-fail
        EXPECT "parse_ip")

    conflux_add_compile_fail_test(
        TARGET conflux_utils_compile_fail_global_parse_cidr
        SOURCE utils_compile_fail_global_parse_cidr.cxx
        TEST api-surface/utils-hides-global-parse-cidr
        LINK conflux_utils conflux_options
        LABELS build api-surface compile-fail
        EXPECT "parse_cidr")

    conflux_add_compile_fail_test(
        TARGET conflux_utils_compile_fail_global_line_view
        SOURCE utils_compile_fail_global_line_view.cxx
        TEST api-surface/utils-hides-global-line-view
        LINK conflux_utils conflux_options
        LABELS build api-surface compile-fail
        EXPECT "LineView")

    conflux_add_compile_fail_test(
        TARGET conflux_utils_compile_fail_global_line_range
        SOURCE utils_compile_fail_global_line_range.cxx
        TEST api-surface/utils-hides-global-line-range
        LINK conflux_utils conflux_options
        LABELS build api-surface compile-fail
        EXPECT "LineRange")

    conflux_add_compile_fail_test(
        TARGET conflux_utils_compile_fail_global_ascii_lower
        SOURCE utils_compile_fail_global_ascii_lower.cxx
        TEST api-surface/utils-hides-global-ascii-lower
        LINK conflux_utils conflux_options
        LABELS build api-surface compile-fail
        EXPECT "ascii_lower")

    conflux_add_compile_fail_test(
        TARGET conflux_utils_compile_fail_global_trim
        SOURCE utils_compile_fail_global_trim.cxx
        TEST api-surface/utils-hides-global-trim
        LINK conflux_utils conflux_options
        LABELS build api-surface compile-fail
        EXPECT "trim")

    conflux_add_compile_fail_test(
        TARGET conflux_smtp_compile_fail_global_client
        SOURCE smtp_compile_fail_global_client.cxx
        TEST api-surface/smtp-hides-global-client
        LINK conflux_net_smtp conflux_options
        LABELS build api-surface compile-fail
        EXPECT "SmtpClient")

    conflux_add_compile_fail_test(
        TARGET conflux_tls_compile_fail_global_context
        SOURCE tls_compile_fail_global_context.cxx
        TEST api-surface/tls-hides-global-context
        LINK conflux_net_tls conflux_options
        LABELS build api-surface compile-fail
        EXPECT "TlsContext")

    conflux_add_compile_fail_test(
        TARGET conflux_process_compile_fail_global_run
        SOURCE process_compile_fail_global_run.cxx
        TEST api-surface/process-hides-global-run
        LINK conflux_process conflux_options
        LABELS build api-surface compile-fail
        EXPECT "run")

    conflux_add_compile_fail_test(
        TARGET conflux_process_compile_fail_global_stdio
        SOURCE process_compile_fail_global_stdio.cxx
        TEST api-surface/process-hides-global-stdio
        LINK conflux_process conflux_options
        LABELS build api-surface compile-fail
        EXPECT "Stdio")

    conflux_add_compile_fail_test(
        TARGET conflux_process_compile_fail_global_spawn_options
        SOURCE process_compile_fail_global_spawn_options.cxx
        TEST api-surface/process-hides-global-spawn-options
        LINK conflux_process conflux_options
        LABELS build api-surface compile-fail
        EXPECT "SpawnOptions")

    conflux_add_compile_fail_test(
        TARGET conflux_process_compile_fail_global_process
        SOURCE process_compile_fail_global_process.cxx
        TEST api-surface/process-hides-global-process
        LINK conflux_process conflux_options
        LABELS build api-surface compile-fail
        EXPECT "Process")

    conflux_add_compile_fail_test(
        TARGET conflux_process_compile_fail_global_run_result
        SOURCE process_compile_fail_global_run_result.cxx
        TEST api-surface/process-hides-global-run-result
        LINK conflux_process conflux_options
        LABELS build api-surface compile-fail
        EXPECT "RunResult")

    conflux_add_compile_fail_test(
        TARGET conflux_process_compile_fail_global_spawn
        SOURCE process_compile_fail_global_spawn.cxx
        TEST api-surface/process-hides-global-spawn
        LINK conflux_process conflux_options
        LABELS build api-surface compile-fail
        EXPECT "spawn")

    conflux_add_compile_fail_test(
        TARGET conflux_uring_handle_compile_fail_global_io_handle
        SOURCE uring_handle_compile_fail_global_io_handle.cxx
        TEST api-surface/uring-handle-hides-global-io-handle
        LINK conflux_uring conflux_options
        LABELS build api-surface compile-fail
        EXPECT "IoHandle")

    conflux_add_compile_fail_test(
        TARGET conflux_uring_completion_compile_fail_global_table
        SOURCE uring_completion_compile_fail_global_table.cxx
        TEST api-surface/uring-completion-hides-global-table
        LINK conflux_uring conflux_options
        LABELS build api-surface compile-fail
        EXPECT "CompletionTable")

    conflux_add_compile_fail_test(
        TARGET conflux_uring_compile_fail_legacy_fd_module
        SOURCE uring_compile_fail_legacy_fd_module.cxx
        TEST api-surface/uring-hides-legacy-fd-module
        LINK conflux_uring conflux_options
        LABELS build api-surface compile-fail
        EXPECT "conflux_uring_fd")

    conflux_add_compile_fail_test(
        TARGET conflux_uring_compile_fail_legacy_sqe_module
        SOURCE uring_compile_fail_legacy_sqe_module.cxx
        TEST api-surface/uring-hides-legacy-sqe-module
        LINK conflux_uring conflux_options
        LABELS build api-surface compile-fail
        EXPECT "conflux_uring_sqe")

    conflux_add_compile_fail_test(
        TARGET conflux_static_async_compile_fail_global_handler
        SOURCE static_async_compile_fail_global_handler.cxx
        TEST api-surface/static-async-hides-global-handler
        LINK conflux_http_static_async conflux_options
        LABELS build api-surface compile-fail
        EXPECT "handle_static_get_request")

    conflux_add_compile_fail_test(
        TARGET conflux_file_watch_compile_fail_global_watcher
        SOURCE file_watch_compile_fail_global_watcher.cxx
        TEST api-surface/file-watch-hides-global-watcher
        LINK conflux_file_watch conflux_options
        LABELS build api-surface compile-fail
        EXPECT "FileWatcher")

    conflux_add_compile_fail_test(
        TARGET conflux_file_watch_compile_fail_global_event_kind
        SOURCE file_watch_compile_fail_global_event_kind.cxx
        TEST api-surface/file-watch-hides-global-event-kind
        LINK conflux_file_watch conflux_options
        LABELS build api-surface compile-fail
        EXPECT "FileEventKind")

    conflux_add_compile_fail_test(
        TARGET conflux_file_watch_compile_fail_global_event
        SOURCE file_watch_compile_fail_global_event.cxx
        TEST api-surface/file-watch-hides-global-event
        LINK conflux_file_watch conflux_options
        LABELS build api-surface compile-fail
        EXPECT "FileEvent")

    conflux_add_compile_fail_test(
        TARGET conflux_file_watch_compile_fail_global_watch_options
        SOURCE file_watch_compile_fail_global_watch_options.cxx
        TEST api-surface/file-watch-hides-global-watch-options
        LINK conflux_file_watch conflux_options
        LABELS build api-surface compile-fail
        EXPECT "WatchOptions")

    conflux_add_compile_fail_test(
        TARGET conflux_template_compile_fail_global_environment
        SOURCE template_compile_fail_global_environment.cxx
        TEST api-surface/template-hides-global-environment
        LINK conflux_template conflux_options
        LABELS build api-surface compile-fail
        EXPECT "Environment")

    conflux_add_compile_fail_test(
        TARGET conflux_template_compile_fail_global_tmpl_value
        SOURCE template_compile_fail_global_tmpl_value.cxx
        TEST api-surface/template-hides-global-tmpl-value
        LINK conflux_template conflux_options
        LABELS build api-surface compile-fail
        EXPECT "TmplValue")

    conflux_add_compile_fail_test(
        TARGET conflux_template_compile_fail_global_build_report
        SOURCE template_compile_fail_global_build_report.cxx
        TEST api-surface/template-hides-global-build-report
        LINK conflux_template conflux_options
        LABELS build api-surface compile-fail
        EXPECT "TemplateBuildReport")

    conflux_add_compile_fail_test(
        TARGET conflux_template_watch_compile_fail_global_template_watcher
        SOURCE template_watch_compile_fail_global_template_watcher.cxx
        TEST api-surface/template-watch-hides-global-template-watcher
        LINK conflux_template_watch conflux_options
        LABELS build api-surface compile-fail
        EXPECT "TemplateWatcher")

    conflux_add_compile_fail_test(
        TARGET conflux_template_watch_compile_fail_global_watch_options
        SOURCE template_watch_compile_fail_global_watch_options.cxx
        TEST api-surface/template-watch-hides-global-watch-options
        LINK conflux_template_watch conflux_options
        LABELS build api-surface compile-fail
        EXPECT "TemplateWatchOptions")

    conflux_add_compile_fail_test(
        TARGET conflux_file_map_compile_fail_global_mapped_body
        SOURCE file_map_compile_fail_global_mapped_body.cxx
        TEST api-surface/file-map-hides-global-mapped-body
        LINK conflux_file_map conflux_options
        LABELS build api-surface compile-fail
        EXPECT "MappedBody")

    conflux_add_compile_fail_test(
        TARGET conflux_file_map_compile_fail_global_mapped_file_lease
        SOURCE file_map_compile_fail_global_mapped_file_lease.cxx
        TEST api-surface/file-map-hides-global-mapped-file-lease
        LINK conflux_file_map conflux_options
        LABELS build api-surface compile-fail
        EXPECT "MappedFileLease")

    conflux_add_compile_fail_test(
        TARGET conflux_file_map_compile_fail_global_make_mapped_file_lease
        SOURCE file_map_compile_fail_global_make_mapped_file_lease.cxx
        TEST api-surface/file-map-hides-global-make-mapped-file-lease
        LINK conflux_file_map conflux_options
        LABELS build api-surface compile-fail
        EXPECT "make_mapped_file_lease")

    conflux_add_compile_fail_test(
        TARGET conflux_file_map_compile_fail_global_blocking_map_fd_readonly
        SOURCE file_map_compile_fail_global_blocking_map_fd_readonly.cxx
        TEST api-surface/file-map-hides-global-blocking-map-fd-readonly
        LINK conflux_file_map conflux_options
        LABELS build api-surface compile-fail
        EXPECT "blocking_map_fd_readonly")

    conflux_add_compile_fail_test(
        TARGET conflux_file_map_compile_fail_global_blocking_map_file_readonly
        SOURCE file_map_compile_fail_global_blocking_map_file_readonly.cxx
        TEST api-surface/file-map-hides-global-blocking-map-file-readonly
        LINK conflux_file_map conflux_options
        LABELS build api-surface compile-fail
        EXPECT "blocking_map_file_readonly")

    conflux_add_compile_fail_test(
        TARGET conflux_file_io_compile_fail_global_pipe_pool
        SOURCE file_io_compile_fail_global_pipe_pool.cxx
        TEST api-surface/file-io-hides-global-pipe-pool
        LINK conflux_file_io conflux_options
        LABELS build api-surface compile-fail
        EXPECT "PipePool")

    conflux_add_compile_fail_test(
        TARGET conflux_file_io_compile_fail_global_fixed_buffer
        SOURCE file_io_compile_fail_global_fixed_buffer.cxx
        TEST api-surface/file-io-hides-global-fixed-buffer
        LINK conflux_file_io conflux_options
        LABELS build api-surface compile-fail
        EXPECT "FixedBuffer")

    conflux_add_compile_fail_test(
        TARGET conflux_file_io_compile_fail_global_reader
        SOURCE file_io_compile_fail_global_reader.cxx
        TEST api-surface/file-io-hides-global-reader
        LINK conflux_file_io conflux_options
        LABELS build api-surface compile-fail
        EXPECT "FileReader")

    conflux_add_compile_fail_test(
        TARGET conflux_file_io_compile_fail_global_iopoll
        SOURCE file_io_compile_fail_global_iopoll.cxx
        TEST api-surface/file-io-hides-global-iopoll
        LINK conflux_file_io conflux_options
        LABELS build api-surface compile-fail
        EXPECT "IopollStorageRingOptions")

    conflux_add_compile_fail_test(
        TARGET conflux_file_io_compile_fail_legacy_error_alias
        SOURCE file_io_compile_fail_legacy_error_alias.cxx
        TEST api-surface/file-io-hides-legacy-error-alias
        LINK conflux_file_io conflux_options
        LABELS build api-surface compile-fail
        EXPECT "FileIoError")

    conflux_add_compile_fail_test(
        TARGET conflux_file_io_sync_compile_fail_global_unique_fd
        SOURCE file_io_sync_compile_fail_global_unique_fd.cxx
        TEST api-surface/file-io-sync-hides-global-unique-fd
        LINK conflux_file_io_sync conflux_options
        LABELS build api-surface compile-fail
        EXPECT "UniqueFd")

    conflux_add_compile_fail_test(
        TARGET conflux_file_io_sync_compile_fail_legacy_error_alias
        SOURCE file_io_sync_compile_fail_legacy_error_alias.cxx
        TEST api-surface/file-io-sync-hides-legacy-error-alias
        LINK conflux_file_io_sync conflux_options
        LABELS build api-surface compile-fail
        EXPECT "FileIoSyncError")

    conflux_add_compile_fail_test(
        TARGET conflux_file_io_sync_compile_fail_legacy_tmpfile_alias
        SOURCE file_io_sync_compile_fail_legacy_tmpfile_alias.cxx
        TEST api-surface/file-io-sync-hides-legacy-tmpfile-alias
        LINK conflux_file_io_sync conflux_options
        LABELS build api-surface compile-fail
        EXPECT "TemporaryFileSync")

    conflux_add_compile_fail_test(
        TARGET conflux_socket_io_compile_fail_global_task_ring
        SOURCE socket_io_compile_fail_global_task_ring.cxx
        TEST api-surface/socket-io-hides-global-task-ring
        LINK conflux_socket_io conflux_options
        LABELS build api-surface compile-fail
        EXPECT "SocketTaskRingOptions")

    conflux_add_compile_fail_test(
        TARGET conflux_socket_io_compile_fail_global_tcp_stream
        SOURCE socket_io_compile_fail_global_tcp_stream.cxx
        TEST api-surface/socket-io-hides-global-tcp-stream
        LINK conflux_socket_io conflux_options
        LABELS build api-surface compile-fail
        EXPECT "TcpStream")

    conflux_add_compile_fail_test(
        TARGET conflux_socket_io_compile_fail_global_blocking
        SOURCE socket_io_compile_fail_global_blocking.cxx
        TEST api-surface/socket-io-hides-global-blocking
        LINK conflux_socket_io conflux_options
        LABELS build api-surface compile-fail
        EXPECT "SyncWaitSocketTaskTimeout")

    conflux_add_compile_fail_test(
        TARGET conflux_work_compile_fail_global_work_pool
        SOURCE work_compile_fail_global_work_pool.cxx
        TEST api-surface/work-hides-global-work-pool
        LINK conflux_work conflux_options
        LABELS build api-surface compile-fail
        EXPECT "WorkPool")

    conflux_add_compile_fail_test(
        TARGET conflux_work_compile_fail_global_work_pool_options
        SOURCE work_compile_fail_global_work_pool_options.cxx
        TEST api-surface/work-hides-global-work-pool-options
        LINK conflux_work conflux_options
        LABELS build api-surface compile-fail
        EXPECT "WorkPoolOptions")

    conflux_add_compile_fail_test(
        TARGET conflux_work_compile_fail_global_ring_lane
        SOURCE work_compile_fail_global_ring_lane.cxx
        TEST api-surface/work-hides-global-ring-lane
        LINK conflux_work conflux_options
        LABELS build api-surface compile-fail
        EXPECT "RingLane")

    conflux_add_compile_fail_test(
        TARGET conflux_work_compile_fail_global_ring_lane_options
        SOURCE work_compile_fail_global_ring_lane_options.cxx
        TEST api-surface/work-hides-global-ring-lane-options
        LINK conflux_work conflux_options
        LABELS build api-surface compile-fail
        EXPECT "RingLaneOptions")

    conflux_add_compile_fail_test(
        TARGET conflux_work_compile_fail_global_cancelled
        SOURCE work_compile_fail_global_cancelled.cxx
        TEST api-surface/work-hides-global-cancelled
        LINK conflux_work conflux_options
        LABELS build api-surface compile-fail
        EXPECT "Cancelled")

    conflux_add_compile_fail_test(
        TARGET conflux_work_compile_fail_global_task
        SOURCE work_compile_fail_global_task.cxx
        TEST api-surface/work-hides-global-task
        LINK conflux_work conflux_options
        LABELS build api-surface compile-fail
        EXPECT "Task")

    conflux_add_compile_fail_test(
        TARGET conflux_work_compile_fail_global_task_source
        SOURCE work_compile_fail_global_task_source.cxx
        TEST api-surface/work-hides-global-task-source
        LINK conflux_work conflux_options
        LABELS build api-surface compile-fail
        EXPECT "TaskSource")

    conflux_add_compile_fail_test(
        TARGET conflux_types_compile_fail_global_io_error
        SOURCE types_compile_fail_global_io_error.cxx
        TEST api-surface/types-hides-global-io-error
        LINK conflux_types conflux_options
        LABELS build api-surface compile-fail
        EXPECT "IoError")
endif()
