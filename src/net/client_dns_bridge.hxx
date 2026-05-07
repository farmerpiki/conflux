#pragma once

#include<stddef.h>
#include<stdint.h>

namespace conflux::http::client_dns_bridge{
struct Endpoint{
alignas(8) unsigned char addr[128]{};
unsigned int addr_len{};
int family{};
};

using EndpointSink=bool(*)(void*,Endpoint const&)noexcept;

bool resolve(
void*resolver,
char const*host_data,
size_t host_size,
uint16_t port,
long long timeout_ms,
void*sink_ctx,
EndpointSink sink,
char*error_data,
size_t error_size)noexcept;
}
