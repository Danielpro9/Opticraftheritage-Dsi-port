#pragma once

#include <string>

// DSi-only: a from-scratch, minimal DNS SRV ("_minecraft._tcp.<host>")
// lookup. dswifi9's own resolver (netdb.h's gethostbyname()/getaddrinfo())
// only ever does A-record lookups -- there is no SRV support anywhere in
// the library. Needed because Minecraft Beta 1.2.5's connection flow
// (what every platform's NetworkManager in this project replicates, DSi
// included) predates SRV support in the official client entirely, but
// real-hardware testing against a budget host (HolyHosting) confirmed a
// non-default-port server there only works through its SRV record --
// connecting straight to the literal typed host:port reaches the wrong
// target and gets an active TCP refusal, not a timeout.
//
// Best-effort only: on any failure (no DNS server configured, the query
// times out, the response has no SRV answer, a malformed response), this
// leaves outHost/outPort untouched and returns false -- DsiNetworkSocket::
// connect() falls back to the original address exactly as it did before
// this existed, so a server that never needed SRV is unaffected beyond
// one extra UDP round trip before it connects as normal.
bool dsiResolveMinecraftSrv(const std::string &host, std::string &outHost, int &outPort);
