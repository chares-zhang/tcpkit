#include "packet.h"
#include "tcpkit.h"
#include "util.h"
#include "local_addresses.h"
#include <stdlib.h>
#include <string.h>
#define NULL_HDRLEN 4

static void
push_params(const struct ip *ip, unsigned dlen,  const struct timeval *tv)
{
    int tcp_hdr_size, direct = 0;
    struct tcphdr *tcp;
    uint16_t sport, dport;

    tcp = (struct tcphdr *) ((unsigned char *) ip + sizeof(struct ip));
#if defined(__FAVOR_BSD) || defined(__APPLE__)
        sport = ntohs(tcp->th_sport);
        dport = ntohs(tcp->th_dport);
        tcp_hdr_size = tcp->th_off * 4;
#else
        sport = ntohs(tcp->source);
        dport = ntohs(tcp->dest);
        tcp_hdr_size = tcp->doff * 4;
#endif
        if (is_local_address(ip->ip_dst)) {
            // 0 means incoming packet
            direct = 1;
        }

        lua_State *L = get_lua_vm();
        lua_newtable(L);
        script_pushtableinteger(L, "tv_sec",  tv->tv_sec);
        script_pushtableinteger(L, "tv_usec", tv->tv_usec);
        script_pushtableinteger(L, "direct", direct);
        script_pushtableinteger(L, "len" , dlen);
        script_pushtablestring(L,  "src", inet_ntoa(ip->ip_src));
        script_pushtablestring(L,  "dst", inet_ntoa(ip->ip_dst));
        script_pushtableinteger(L, "sport", sport);
        script_pushtableinteger(L, "dport", dport);
        script_pushtableinteger(L, "is_client", is_client_mode());

        if (dlen > 0) {
            // -----------+-----------+----------+-----....-----+
            // | ETHER    |  IP       | TCP      | payload      |
            // -----------+-----------+----------+--------------+
            script_pushtablelstring(L, "payload", (char *)tcp + tcp_hdr_size, dlen);
        }
}

static void packet_callback(const struct ip *ip, const struct timeval *tv) {
    lua_State *L;
    struct tcphdr *tcp;
    unsigned len, datalen; 

    tcp = (struct tcphdr *) ((unsigned char *) ip + sizeof(struct ip));
    len = htons(ip->ip_len);
#if defined(__FAVOR_BSD) || defined(__APPLE__)
    datalen = len - sizeof(struct ip) - tcp->th_off * 4;    // 4 bits offset 
#else
    datalen = len - sizeof(struct ip) - tcp->doff * 4;
#endif

    L = get_lua_vm();
    if (!L) logger(ERROR, "Lua vm didn't initialed.");
    lua_getglobal(L, DEFAULT_CALLBACK);
    push_params(ip, datalen, tv);
    if (lua_pcall(L, 1, 1, 0) != 0) {
        logger(ERROR, "%s", lua_tostring(L, -1));
    }
    script_need_gc(L); // check whether need gc.
    lua_tonumber(L, -1);
    lua_pop(L,-1);
}

static void
calc_bandwidth(const struct ip *ip, const struct timeval *tv)
{ 
    struct bandwidth *bw;

    bw = get_global_bandwidth();
    need_report_bandwidth();
    if (is_local_address(ip->ip_dst)) {
        bw->in_bytes += htons(ip->ip_len);        
        bw->in_packets += 1;
    } else {
        bw->out_bytes += htons(ip->ip_len);       
        bw->out_packets += 1;
    }
}

static int
process_ip_packet(const struct ip *ip, const struct timeval *tv)
{
    struct tk_options *opts;
    switch (ip->ip_p) {
        case IPPROTO_TCP:
            opts = get_global_options();
            if (opts->is_calc_mode) {
                calc_bandwidth(ip, tv);
            } else {
                packet_callback(ip, tv);
            }
            break;
    }
    return 0;
}

void
process_packet(unsigned char *user, const struct pcap_pkthdr *header,
                const unsigned char *packet)
{
    const struct sll_header *sll;
    const struct ether_header *ether_header;
    const struct ip *ip;
    unsigned short packet_type;
    pcap_wrapper *pw;

    pw  = (pcap_wrapper *) user;
    switch (pcap_datalink(pw->pcap)) {
    case DLT_NULL:
        /* BSD loopback */
        ip = (struct ip *)(packet + NULL_HDRLEN);
        break;
    case DLT_LINUX_SLL:
        sll = (struct sll_header *) packet;
        packet_type = ntohs(sll->sll_protocol);
        ip = (const struct ip *) (packet + sizeof(struct sll_header));
        break;
    case DLT_EN10MB:
        ether_header = (struct ether_header *) packet;
        packet_type = ntohs(ether_header->ether_type);
        ip = (const struct ip *) (packet + sizeof(struct ether_header));
        break;
    case DLT_RAW:
        packet_type = ETHERTYPE_IP; //This is raw ip
        ip = (const struct ip *) packet;
        break;

     default: return; 
    }
    
    // prevent warning
    packet_type = 0;
    process_ip_packet(ip, &header->ts); 
}
