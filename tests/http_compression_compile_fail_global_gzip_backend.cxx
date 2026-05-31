import conflux.net.compress;

auto probe() -> ::GzipBackend {
	return ::GzipBackend::auto_select;
}
