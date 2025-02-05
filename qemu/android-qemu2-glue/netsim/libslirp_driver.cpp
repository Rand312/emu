// Copyright (C) 2023 The Android Open Source Project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "android-qemu2-glue/netsim/libslirp_driver.h"

#include "aemu/base/ArraySize.h"
#include "aemu/base/network/IpAddress.h"
#include "aemu/base/network/Dns.h"
#include "android/base/system/System.h"
#include "android/utils/debug.h"
#include "android/utils/system.h"


#ifdef _WIN32
#include <winsock2.h>
#endif

extern "C" {
#include "libslirp.h"
#include "util.h"
#include "ip6.h"

#ifdef __linux__
#include <poll.h>
#endif
}

namespace android {
namespace qemu2 {

static slirp_rx_callback s_rx_callback = nullptr;
static Slirp* s_slirp = nullptr;
static GArray* s_gpollfds;

/* Transition function to convert a nanosecond timeout to ms
 * This is used where a system does not support ppoll
 */
static int libslirp_timeout_ns_to_ms(int64_t ns)
{
    int64_t ms;
    if (ns < 0) {
        return -1;
    }

    if (!ns) {
        return 0;
    }

    /* Always round up, because it's better to wait too long than to wait too
     * little and effectively busy-wait
     */
    ms = DIV_ROUND_UP(ns, SCALE_MS);

    /* To avoid overflow problems, limit this to 2^31, i.e. approx 25 days */
    if (ms > static_cast<int64_t>(INT32_MAX)) {
        ms = INT32_MAX;
    }

    return (int) ms;
}

#ifdef _WIN32
// Invariant: pollfds and fds always have the same length
static int pollfds_fill(GArray* pollfds, WSAPOLLFD* fds) {
    int i;
    int sockets_count = 0;
    for (i = 0; i < pollfds->len; i++) {
        GPollFD* pfd = &g_array_index(pollfds, GPollFD, i);
        int events = pfd->events;
        fds[i].events = 0;
        if (events & G_IO_IN) {
            fds[i].events |= POLLRDNORM;
        }
        if (events & G_IO_OUT) {
            fds[i].events |= POLLOUT;
        }
        if (events & G_IO_PRI) {
            fds[i].events |= POLLRDBAND;
        }

        if (fds[i].events == 0) {
            fds[i].fd = INVALID_SOCKET;  // ignore this one
        } else {
            ++sockets_count;
            fds[i].fd = pfd->fd;
        }
    }
    return sockets_count;
}

// Invariant: pollfds and fds always have the same length
static void pollfds_poll(GArray* pollfds, const WSAPOLLFD* fds) {
    int i;
    for (i = 0; i < pollfds->len; i++) {
        GPollFD* pfd = &g_array_index(pollfds, GPollFD, i);
        int revents = 0;
        if (fds[i].fd != INVALID_SOCKET) {
            if (fds[i].revents & (POLLRDNORM | POLLERR | POLLHUP)) {
                revents |= G_IO_IN;
            }
            if (fds[i].revents & POLLWRNORM) {
                revents |= G_IO_OUT;
            }
            if (fds[i].revents & (POLLERR | POLLHUP | POLLPRI | POLLRDBAND)) {
                revents |= G_IO_PRI;
            }
        }
        pfd->revents = revents & pfd->events;
    }
}
#endif

static int libslirp_poll_ns(GPollFD *fds, guint nfds, int64_t timeout)
{
#ifdef __linux__
    if (timeout < 0) {
        return ppoll((struct pollfd *)fds, nfds, NULL, NULL);
    } else {
        struct timespec ts;
        int64_t tvsec = timeout / 1000000000LL;
        /* Avoid possibly overflowing and specifying a negative number of
         * seconds, which would turn a very long timeout into a busy-wait.
         */
        if (tvsec > static_cast<int64_t>(INT32_MAX)) {
            tvsec = INT32_MAX;
        }
        ts.tv_sec = tvsec;
        ts.tv_nsec = timeout % 1000000000LL;
        return ppoll((struct pollfd *)fds, nfds, &ts, NULL);
    }
#elif defined(_WIN32)
    int poll_ret = 0;
    WSAPOLLFD* wsa_fds = static_cast<WSAPOLLFD*>(
            android_alloc0(sizeof(WSAPOLLFD) * s_gpollfds->len));
    int polled_count = pollfds_fill(s_gpollfds, wsa_fds);
    if (polled_count > 0) {
        poll_ret = WSAPoll(wsa_fds, s_gpollfds->len, 0);
        if (poll_ret != 0) {
            timeout = 0;
        }
        if (poll_ret > 0) {
            pollfds_poll(s_gpollfds, wsa_fds);
        }
    }
    return poll_ret;
#else
    return g_poll(fds, nfds, libslirp_timeout_ns_to_ms(timeout));
#endif
}

static int libslirp_poll_to_gio(int events)
{
    int ret = 0;

    if (events & SLIRP_POLL_IN) {
        ret |= G_IO_IN;
    }
    if (events & SLIRP_POLL_OUT) {
        ret |= G_IO_OUT;
    }
    if (events & SLIRP_POLL_PRI) {
        ret |= G_IO_PRI;
    }
    if (events & SLIRP_POLL_ERR) {
        ret |= G_IO_ERR;
    }
    if (events & SLIRP_POLL_HUP) {
        ret |= G_IO_HUP;
    }

    return ret;
}

static int libslirp_add_poll(int fd, int events, void *opaque)
{
    GArray *pollfds = static_cast<GArray *>(opaque);
    GPollFD pfd = {
        .fd = fd,
        .events = static_cast<gushort>(libslirp_poll_to_gio(events)),
    };
    int idx = pollfds->len;
    g_array_append_val(pollfds, pfd);
    return idx;
}

static int libslirp_gio_to_poll(int events)
{
    int ret = 0;

    if (events & G_IO_IN) {
        ret |= SLIRP_POLL_IN;
    }
    if (events & G_IO_OUT) {
        ret |= SLIRP_POLL_OUT;
    }
    if (events & G_IO_PRI) {
        ret |= SLIRP_POLL_PRI;
    }
    if (events & G_IO_ERR) {
        ret |= SLIRP_POLL_ERR;
    }
    if (events & G_IO_HUP) {
        ret |= SLIRP_POLL_HUP;
    }

    return ret;
}

static int libslirp_get_revents(int idx, void *opaque)
{
    GArray *pollfds = static_cast<GArray *>(opaque);

    return libslirp_gio_to_poll(g_array_index(pollfds, GPollFD, idx).revents);
}

static int get_str_sep(char* buf, int buf_size, const char** pp, int sep) {
    const char *p, *p1;
    int len;
    p = *pp;
    p1 = strchr(p, sep);
    if (!p1)
        return -1;
    len = p1 - p;
    p1++;
    if (buf_size > 0) {
        if (len > buf_size - 1)
            len = buf_size - 1;
        memcpy(buf, p, len);
        buf[len] = '\0';
    }
    *pp = p1;
    return 0;
}

static slirp_ssize_t libslirp_send_packet(const void* pkt,
                                          size_t pkt_len,
                                          void* opaque) {
    if (s_rx_callback) {
        return s_rx_callback((const uint8_t*)pkt, pkt_len);
    } else {
        dwarning("libslirp: receive callback is not registered.\n");
        return 0;
    }
}

static void libslirp_guest_error(const char* msg, void* opaque) {
    dinfo("libslirp: %s\n", msg);
}

static int64_t libslirp_clock_get_ns(void* opaque) {
    return android::base::System::getSystemTimeUs() * 1000;
}

static void libslirp_init_completed(Slirp* slirp, void* opaque) {
    dinfo("libslirp: initialization completed.\n");
}

static void libslirp_timer_cb(void* opaque) {
    /*TODO(wdu@): Un-implemented because the function callback is only used
     * by icmp. */
}

static void* libslirp_timer_new_opaque(SlirpTimerId id,
                                       void* cb_opaque,
                                       void* opaque) {
    /*TODO(wdu@): Un-implemented because the function callback is only used
     * by icmp. */
    return nullptr;
}

static void libslirp_timer_free(void* timer, void* opaque) {
    /*TODO(wdu@): Un-implemented because the function callback is only used
     * by icmp. */
}

static void libslirp_timer_mod(void* timer,
                               int64_t expire_timer,
                               void* opaque) {
    /*TODO(wdu@): Un-implemented because the function callback is only used
     * by icmp. */
}

static void libslirp_register_poll_fd(int fd, void* opaque) {
    /*TODO(wdu@): Need implementation for Windows*/
}

static void libslirp_unregister_poll_fd(int fd, void* opaque) {
    /*TODO(wdu@): Need implementation for Windows*/
}

static void libslirp_notify(void* opaque) {
    /*TODO(wdu@): Un-implemented.*/
}

static const SlirpCb slirp_cb = {
        .send_packet = libslirp_send_packet,
        .guest_error = libslirp_guest_error,
        .clock_get_ns = libslirp_clock_get_ns,
        .timer_free = libslirp_timer_free,
        .timer_mod = libslirp_timer_mod,
        .register_poll_fd = libslirp_register_poll_fd,
        .unregister_poll_fd = libslirp_unregister_poll_fd,
        .notify = libslirp_notify,
        .init_completed = libslirp_init_completed,
        .timer_new_opaque = libslirp_timer_new_opaque,
};

void libslirp_main_loop_wait(bool nonblocking) {
    int ret;
    uint32_t timeout = UINT32_MAX;
    int64_t timeout_ns;

    if (nonblocking) {
        timeout = 0;
    }
    /* Reset for new iteration */
    g_array_set_size(s_gpollfds, 0);
    /* Add fds to s_gpollfds for polling*/
    slirp_pollfds_fill(s_slirp, &timeout,
                        libslirp_add_poll, s_gpollfds);
    if (timeout == UINT32_MAX) {
        timeout_ns = -1;
    } else {
        timeout_ns =
                static_cast<uint64_t>(timeout) * static_cast<int64_t>(SCALE_MS);
    }
    ret = libslirp_poll_ns((GPollFD *)s_gpollfds->data, s_gpollfds->len, timeout_ns);
    /* Perform I/O when sockets are ready */
    slirp_pollfds_poll(s_slirp, ret < 0,
                           libslirp_get_revents, s_gpollfds);
}

static bool resolveHostNameToList(
        const char* hostName, std::vector<sockaddr_storage>* out) {
    int count = 0;
    android::base::Dns::AddressList list = android::base::Dns::resolveName(hostName);
    for (const auto& ip : list) {
        // Convert IpAddress instance |src| into a sockaddr_storage |dst|.
        sockaddr_storage addr = {};
        if (ip.isIpv4()) {
            auto sin = reinterpret_cast<sockaddr_in *>(&addr);
            sin->sin_family = AF_INET;
            sin->sin_addr.s_addr = htonl(ip.ipv4());
        } else if (ip.isIpv6()) {
            auto sin6 = reinterpret_cast<sockaddr_in6 *>(&addr);
            sin6->sin6_family = AF_INET6;
            memcpy(sin6->sin6_addr.s6_addr, ip.ipv6Addr(), 16);
            sin6->sin6_scope_id = htonl(ip.ipv6ScopeId());
            sin6->sin6_port = 0;
        }

        if (ip.isIpv4() || ip.isIpv6()) {
            out->emplace_back(std::move(addr));
            count++;
        }
    }
    return (count > 0);
}

static bool setup_custom_dns_server(std::vector<std::string>& host_dns, SlirpConfig* cfg) {
    std::vector<sockaddr_storage> server_addresses;
    for (int i = 0; i < host_dns.size(); i++) {
        if (!resolveHostNameToList(host_dns[i].c_str(), &server_addresses)) {
            dwarning("Netsim WiFi: Ignoring ivalid DNS address: [%s]\n", host_dns[i].c_str());
        }
    }
    size_t count = server_addresses.size();
    if (count == 0) {
        return false;
    }

    if (count > SLIRP_MAX_DNS_SERVERS) {
        dwarning("Too many DNS servers, only keeping the first %d ones\n",
                 SLIRP_MAX_DNS_SERVERS);
        count = SLIRP_MAX_DNS_SERVERS;
    }

    cfg->host_dns_count = count;
    memcpy(cfg->host_dns, &server_addresses[0],
           count * sizeof(server_addresses[0]));
    return true;
}

Slirp* libslirp_init(slirp_rx_callback rx_callback,
                     int restricted,
                     bool ipv4,
                     const char* vnetwork,
                     const char* vhost,
                     bool ipv6,
                     const char* vprefix6,
                     int vprefix6_len,
                     const char* vhost6,
                     const char* vhostname,
                     const char* tftp_export,
                     const char* bootfile,
                     const char* vdhcp_start,
                     const char* vnameserver,
                     const char* vnameserver6,
                     const char** dnssearch,
                     const char* vdomainname,
                     const char* tftp_server_name,
                     std::vector<std::string>& host_dns) {
    /* default settings according to historic slirp */
    struct in_addr net = {.s_addr = htonl(0x0a000200)};  /* 10.0.2.0 */
    struct in_addr mask = {.s_addr = htonl(0xffffff00)}; /* 255.255.255.0 */
    struct in_addr host = {.s_addr = htonl(0x0a000202)}; /* 10.0.2.2 */
    struct in_addr dhcp = {.s_addr = htonl(0x0a000210)}; /* 10.0.2.16 */
    struct in_addr dns = {.s_addr = htonl(0x0a000203)};  /* 10.0.2.3 */
    struct in6_addr ip6_prefix;
    struct in6_addr ip6_host;
    struct in6_addr ip6_dns;
    SlirpConfig cfg = {0};
    Slirp* slirp = nullptr;
    char buf[20];
    uint32_t addr;
    int shift;
    char* end;
    s_rx_callback = std::move(rx_callback);
#ifndef _WIN32
    signal(SIGPIPE, SIG_IGN);
#endif
    s_gpollfds = g_array_new(FALSE, FALSE, sizeof(GPollFD));

    if (!ipv4 && (vnetwork || vhost || vnameserver)) {
        derror("IPv4 disabled but netmask/host/dns provided");
        return nullptr;
    }

    if (!ipv6 && (vprefix6 || vhost6 || vnameserver6)) {
        derror("IPv6 disabled but prefix/host6/dns6 provided");
        return nullptr;
    }

    if (!ipv4 && !ipv6) {
        /* It doesn't make sense to disable both */
        derror("IPv4 and IPv6 disabled");
        return nullptr;
    }

    if (vnetwork) {
        if (get_str_sep(buf, sizeof(buf), &vnetwork, '/') < 0) {
            if (!inet_aton(vnetwork, &net)) {
                derror("Failed to parse netmask");
                return nullptr;
            }
            addr = ntohl(net.s_addr);
            if (!(addr & 0x80000000)) {
                mask.s_addr = htonl(0xff000000); /* class A */
            } else if ((addr & 0xfff00000) == 0xac100000) {
                mask.s_addr = htonl(0xfff00000); /* priv. 172.16.0.0/12 */
            } else if ((addr & 0xc0000000) == 0x80000000) {
                mask.s_addr = htonl(0xffff0000); /* class B */
            } else if ((addr & 0xffff0000) == 0xc0a80000) {
                mask.s_addr = htonl(0xffff0000); /* priv. 192.168.0.0/16 */
            } else if ((addr & 0xffff0000) == 0xc6120000) {
                mask.s_addr = htonl(0xfffe0000); /* tests 198.18.0.0/15 */
            } else if ((addr & 0xe0000000) == 0xe0000000) {
                mask.s_addr = htonl(0xffffff00); /* class C */
            } else {
                mask.s_addr = htonl(0xfffffff0); /* multicast/reserved */
            }
        } else {
            if (!inet_aton(buf, &net)) {
                derror("Failed to parse netmask");
                return nullptr;
            }
            shift = strtol(vnetwork, &end, 10);
            if (*end != '\0') {
                if (!inet_aton(vnetwork, &mask)) {
                    derror("Failed to parse netmask (trailing chars)");
                    return nullptr;
                }
            } else if (shift < 4 || shift > 32) {
                derror("Invalid netmask provided (must be in range 4-32)");
                return nullptr;
            } else {
                mask.s_addr = htonl(0xffffffff << (32 - shift));
            }
        }
        net.s_addr &= mask.s_addr;
        host.s_addr = net.s_addr | (htonl(0x0202) & ~mask.s_addr);
        dhcp.s_addr = net.s_addr | (htonl(0x020f) & ~mask.s_addr);
        dns.s_addr = net.s_addr | (htonl(0x0203) & ~mask.s_addr);
    }

    if (vhost && !inet_aton(vhost, &host)) {
        derror("Failed to parse host");
        return nullptr;
    }
    if ((host.s_addr & mask.s_addr) != net.s_addr) {
        derror("Host doesn't belong to network");
        return nullptr;
    }

    if (vnameserver && !inet_aton(vnameserver, &dns)) {
        derror("Failed to parse DNS");
        return nullptr;
    }
    if (restricted && (dns.s_addr & mask.s_addr) != net.s_addr) {
        derror("DNS doesn't belong to network");
        return nullptr;
    }
    if (dns.s_addr == host.s_addr) {
        derror("DNS must be different from host");
        return nullptr;
    }

    if (vdhcp_start && !inet_aton(vdhcp_start, &dhcp)) {
        derror("Failed to parse DHCP start address");
        return nullptr;
    }
    if ((dhcp.s_addr & mask.s_addr) != net.s_addr) {
        derror("DHCP doesn't belong to network");
        return nullptr;
    }
    if (dhcp.s_addr == host.s_addr || dhcp.s_addr == dns.s_addr) {
        derror("DHCP must be different from host and DNS");
        return nullptr;
    }

    if (!vprefix6) {
        vprefix6 = "fec0::";
    }
    if (!inet_pton(AF_INET6, vprefix6, &ip6_prefix)) {
        derror("Failed to parse IPv6 prefix");
        return nullptr;
    }

    if (!vprefix6_len) {
        vprefix6_len = 64;
    }
    if (vprefix6_len < 0 || vprefix6_len > 126) {
        derror("Invalid IPv6 prefix provided "
               "(IPv6 prefix length must be between 0 and 126)");
        return nullptr;
    }

    if (vhost6) {
        if (!inet_pton(AF_INET6, vhost6, &ip6_host)) {
            derror("Failed to parse IPv6 host");
            return nullptr;
        }
        if (!in6_equal_net(&ip6_prefix, &ip6_host, vprefix6_len)) {
            derror("IPv6 Host doesn't belong to network");
            return nullptr;
        }
    } else {
        ip6_host = ip6_prefix;
        ip6_host.s6_addr[15] |= 2;
    }

    if (vnameserver6) {
        if (!inet_pton(AF_INET6, vnameserver6, &ip6_dns)) {
            derror("Failed to parse IPv6 DNS");
            return nullptr;
        }
        if (restricted && !in6_equal_net(&ip6_prefix, &ip6_dns, vprefix6_len)) {
            derror("IPv6 DNS doesn't belong to network");
            return nullptr;
        }
    } else {
        ip6_dns = ip6_prefix;
        ip6_dns.s6_addr[15] |= 3;
    }

    if (vdomainname && !*vdomainname) {
        derror("'domainname' parameter cannot be empty");
        return nullptr;
    }

    if (vdomainname && strlen(vdomainname) > 255) {
        derror("'domainname' parameter cannot exceed 255 bytes");
        return nullptr;
    }

    if (vhostname && strlen(vhostname) > 255) {
        derror("'vhostname' parameter cannot exceed 255 bytes");
        return nullptr;
    }

    if (tftp_server_name && strlen(tftp_server_name) > 255) {
        derror("'tftp-server-name' parameter cannot exceed 255 bytes");
        return nullptr;
    }

    if (host_dns.size() > 0) {
        setup_custom_dns_server(host_dns, &cfg);
    }

    cfg.version = 5;
    cfg.restricted = restricted;
    cfg.in_enabled = ipv4;
    cfg.vnetwork = net;
    cfg.vnetmask = mask;
    cfg.vhost = host;
    cfg.in6_enabled = ipv6;
    cfg.vprefix_addr6 = ip6_prefix;
    cfg.vprefix_len = vprefix6_len;
    cfg.vhost6 = ip6_host;
    cfg.vhostname = vhostname;
    cfg.tftp_server_name = tftp_server_name;
    cfg.tftp_path = tftp_export;
    cfg.bootfile = bootfile;
    cfg.vdhcp_start = dhcp;
    cfg.vnameserver = dns;
    cfg.vnameserver6 = ip6_dns;
    cfg.vdnssearch = dnssearch;
    cfg.vdomainname = vdomainname;
    slirp = slirp_new(&cfg, &slirp_cb, nullptr);
    s_slirp = slirp;
    return slirp;
}

}  // namespace qemu2
}  // namespace android
