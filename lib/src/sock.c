// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#include <chiaki/sock.h>
#include <fcntl.h>
#include <stdlib.h>

#ifdef _WIN32
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <string.h>
#include <wctype.h>

typedef struct ps5link_qos_version_t {
	USHORT MajorVersion;
	USHORT MinorVersion;
} Ps5LinkQosVersion;

typedef BOOL (WINAPI *FnQOSCreateHandle)(Ps5LinkQosVersion *Version, PHANDLE QOSHandle);
typedef BOOL (WINAPI *FnQOSAddSocketToFlow)(HANDLE QOSHandle, SOCKET Socket, struct sockaddr *DestAddr,
	DWORD TrafficType, DWORD Flags, PULONG FlowId);

static HANDLE ps5link_get_qwave_handle(FnQOSAddSocketToFlow *out_add_flow)
{
	static LONG init_state = 0; // 0=uninit, 1=initing, 2=done
	static HANDLE qos_handle = NULL;
	static FnQOSAddSocketToFlow add_flow_fn = NULL;

	if(InterlockedCompareExchange(&init_state, 1, 0) == 0)
	{
		HMODULE lib = LoadLibraryExW(L"qwave.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
		if(lib)
		{
			FnQOSCreateHandle create_fn = (FnQOSCreateHandle)(void *)GetProcAddress(lib, "QOSCreateHandle");
			add_flow_fn = (FnQOSAddSocketToFlow)(void *)GetProcAddress(lib, "QOSAddSocketToFlow");
			if(create_fn && add_flow_fn)
			{
				Ps5LinkQosVersion ver = { 1, 0 };
				if(!create_fn(&ver, &qos_handle))
					qos_handle = NULL;
			}
		}
		InterlockedExchange(&init_state, 2);
	}
	else
	{
		while(InterlockedCompareExchange(&init_state, 2, 2) != 2)
			Sleep(0);
	}
	if(out_add_flow)
		*out_add_flow = add_flow_fn;
	return qos_handle;
}

static bool wcs_contains_ci(const WCHAR *haystack, const WCHAR *needle)
{
	if(!haystack || !needle)
		return false;
	const size_t nlen = wcslen(needle);
	if(nlen == 0)
		return true;
	for(const WCHAR *p = haystack; *p; ++p)
	{
		size_t i = 0;
		while(i < nlen && p[i] && towlower(p[i]) == towlower(needle[i]))
			++i;
		if(i == nlen)
			return true;
	}
	return false;
}

static bool adapter_is_virtual_or_tun(const IP_ADAPTER_ADDRESSES *aa)
{
	if(aa->IfType == IF_TYPE_PROP_VIRTUAL || aa->IfType == IF_TYPE_TUNNEL || aa->IfType == IF_TYPE_SOFTWARE_LOOPBACK)
		return true;
	static const WCHAR *const kVirtualNames[] = {
		L"wintun", L"meta", L"clash", L"mihomo", L"sing-box", L"singbox",
		L"hyper-v", L"virtual", L"vmware", L"virtualbox", L"tap-", L"wireguard",
		L"zerotier", L"tailscale", L"radmin", L"hamachi", L"nordlynx", L"openvpn"
	};
	for(size_t i = 0; i < sizeof(kVirtualNames) / sizeof(kVirtualNames[0]); ++i)
	{
		if(wcs_contains_ci(aa->FriendlyName, kVirtualNames[i]) || wcs_contains_ci(aa->Description, kVirtualNames[i]))
			return true;
	}
	return false;
}

static bool adapter_is_fake_tun(const IP_ADAPTER_ADDRESSES *aa)
{
	if(aa->OperStatus != IfOperStatusUp)
		return false;
	if(aa->IfType == IF_TYPE_PROP_VIRTUAL || aa->IfType == IF_TYPE_TUNNEL)
	{
		if(wcs_contains_ci(aa->FriendlyName, L"meta") || wcs_contains_ci(aa->FriendlyName, L"clash")
			|| wcs_contains_ci(aa->FriendlyName, L"mihomo") || wcs_contains_ci(aa->FriendlyName, L"sing")
			|| wcs_contains_ci(aa->Description, L"wintun"))
			return true;
	}
	for(const IP_ADAPTER_UNICAST_ADDRESS *ua = aa->FirstUnicastAddress; ua; ua = ua->Next)
	{
		if(ua->Address.lpSockaddr && ua->Address.lpSockaddr->sa_family == AF_INET)
		{
			const uint32_t ip = ntohl(((const struct sockaddr_in *)ua->Address.lpSockaddr)->sin_addr.s_addr);
			// 198.18.0.0/15 is standard Clash/Mihomo/sing-box Fake-IP TUN range
			if((ip & 0xfffe0000u) == 0xc6120000u)
				return true;
		}
	}
	return false;
}

static ULONG find_physical_ipv4_interface_if_tun_active(WCHAR *out_name, size_t out_chars)
{
	ULONG buf_len = 32768;
	IP_ADAPTER_ADDRESSES *addrs = (IP_ADAPTER_ADDRESSES *)malloc(buf_len);
	if(!addrs)
		return 0;
	const ULONG flags = GAA_FLAG_INCLUDE_GATEWAYS | GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST;
	ULONG ret = GetAdaptersAddresses(AF_INET, flags, NULL, addrs, &buf_len);
	if(ret == ERROR_BUFFER_OVERFLOW)
	{
		free(addrs);
		addrs = (IP_ADAPTER_ADDRESSES *)malloc(buf_len);
		if(!addrs)
			return 0;
		ret = GetAdaptersAddresses(AF_INET, flags, NULL, addrs, &buf_len);
	}
	if(ret != NO_ERROR)
	{
		free(addrs);
		return 0;
	}

	bool tun_active = false;
	ULONG best_if = 0;
	ULONG best_metric = 0xffffffffu;
	for(const IP_ADAPTER_ADDRESSES *aa = addrs; aa; aa = aa->Next)
	{
		if(adapter_is_fake_tun(aa))
			tun_active = true;
		if(aa->OperStatus != IfOperStatusUp)
			continue;
		if(aa->IfType != IF_TYPE_ETHERNET_CSMACD && aa->IfType != IF_TYPE_IEEE80211)
			continue;
		if(adapter_is_virtual_or_tun(aa))
			continue;
		if(!aa->FirstGatewayAddress || !aa->IfIndex)
			continue;
		bool has_v4_gw = false;
		for(const IP_ADAPTER_GATEWAY_ADDRESS_LH *gw = aa->FirstGatewayAddress; gw; gw = gw->Next)
		{
			if(gw->Address.lpSockaddr && gw->Address.lpSockaddr->sa_family == AF_INET)
			{
				const struct sockaddr_in *sin = (const struct sockaddr_in *)gw->Address.lpSockaddr;
				if(sin->sin_addr.s_addr != 0)
				{
					has_v4_gw = true;
					break;
				}
			}
		}
		if(!has_v4_gw)
			continue;
		// Prefer wired Ethernet over Wi-Fi if metrics tie
		const ULONG score = aa->Ipv4Metric * 2u + (aa->IfType == IF_TYPE_IEEE80211 ? 1u : 0u);
		if(score < best_metric)
		{
			best_metric = score;
			best_if = aa->IfIndex;
			if(out_name && out_chars > 0 && aa->FriendlyName)
			{
				wcsncpy(out_name, aa->FriendlyName, out_chars - 1);
				out_name[out_chars - 1] = L'\0';
			}
		}
	}
	free(addrs);
	return tun_active ? best_if : 0;
}
#endif

CHIAKI_EXPORT void chiaki_socket_apply_qos(chiaki_socket_t sock, ChiakiLog *log)
{
#ifdef _WIN32
	if(CHIAKI_SOCKET_IS_INVALID(sock))
		return;
	struct sockaddr_storage peer;
	int peer_len = sizeof(peer);
	const bool has_peer = getpeername(sock, (struct sockaddr *)&peer, &peer_len) == 0;
	if(!has_peer || peer.ss_family == AF_INET)
	{
		// Best-effort DSCP CS4 (Video, WMM AC_VI) fallback when user TOS is permitted.
		const DWORD tos = 0x80;
		setsockopt(sock, IPPROTO_IP, IP_TOS, (const char *)&tos, sizeof(tos));
	}
	FnQOSAddSocketToFlow add_flow = NULL;
	HANDLE qos = ps5link_get_qwave_handle(&add_flow);
	if(qos && add_flow && has_peer)
	{
		// QOSTrafficTypeAudioVideo = 3, QOS_NON_ADAPTIVE_FLOW = 2
		ULONG flow_id = 0;
		if(add_flow(qos, (SOCKET)sock, NULL, 3, 2, &flow_id))
		{
			if(log)
				CHIAKI_LOGI(log, "PS5Link enabled Windows qWave AudioVideo QoS flow #%lu", (unsigned long)flow_id);
		}
	}
#else
	(void)sock;
	(void)log;
#endif
}

CHIAKI_EXPORT void chiaki_socket_bypass_tun(chiaki_socket_t sock, int family, ChiakiLog *log)
{
#ifdef _WIN32
	if(family != AF_INET || CHIAKI_SOCKET_IS_INVALID(sock))
		return;
	// Best-effort QoS DSCP CS4 (Video) marking for game/stream UDP packets
	const DWORD tos = 0x80;
	setsockopt(sock, IPPROTO_IP, IP_TOS, (const char *)&tos, sizeof(tos));

	// Strictly opt-in: forcing IP_UNICAST_IF breaks Clash/Mihomo Fake-IP (198.18.0.0/15)
	// DNS lookups (e.g. STUN) and explicit VPN/WireGuard routes.
	const char *enable = getenv("PS5LINK_BYPASS_TUN");
	if(!enable || enable[0] != '1')
		return;

	WCHAR if_name[128] = {0};
	const ULONG if_index = find_physical_ipv4_interface_if_tun_active(if_name, 128);
	if(!if_index)
		return;

	// Windows requires IP_UNICAST_IF value in network byte order for IPv4!
	const DWORD if_be = htonl(if_index);
	if(setsockopt(sock, IPPROTO_IP, IP_UNICAST_IF, (const char *)&if_be, sizeof(if_be)) == 0)
	{
		if(log)
		{
			char utf8_name[128] = {0};
			WideCharToMultiByte(CP_UTF8, 0, if_name, -1, utf8_name, sizeof(utf8_name), NULL, NULL);
			CHIAKI_LOGI(log, "PS5Link bypassed local TUN via direct physical interface #%lu (%s)",
				(unsigned long)if_index, utf8_name[0] ? utf8_name : "physical");
		}
	}
#else
	(void)sock;
	(void)family;
	(void)log;
#endif
}

CHIAKI_EXPORT ChiakiErrorCode chiaki_socket_set_nonblock(chiaki_socket_t sock, bool nonblock)
{
#ifdef _WIN32
	u_long nbio = nonblock ? 1 : 0;
	if(ioctlsocket(sock, FIONBIO, &nbio) != NO_ERROR)
		return CHIAKI_ERR_UNKNOWN;
#else
	int flags = fcntl(sock, F_GETFL, 0);
	if(flags == -1)
		return CHIAKI_ERR_UNKNOWN;
	flags = nonblock ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK);
	if(fcntl(sock, F_SETFL, flags) == -1)
		return CHIAKI_ERR_UNKNOWN;
#endif
	return CHIAKI_ERR_SUCCESS;
}