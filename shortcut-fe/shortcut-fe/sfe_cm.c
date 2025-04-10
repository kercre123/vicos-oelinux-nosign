/*
 * sfe-cm.c
 *	Shortcut forwarding engine connection manager.
 *
 * Copyright (c) 2013-2015 The Linux Foundation. All rights reserved.
 * Permission to use, copy, modify, and/or distribute this software for
 * any purpose with or without fee is hereby granted, provided that the
 * above copyright notice and this permission notice appear in all copies.
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT
 * OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <linux/module.h>
#include <linux/sysfs.h>
#include <linux/skbuff.h>
#include <net/route.h>
#include <net/ip6_route.h>
#include <net/addrconf.h>
#include <linux/inetdevice.h>
#include <linux/netfilter_bridge.h>
#include <linux/netfilter_ipv6.h>
#include <net/netfilter/nf_conntrack_acct.h>
#include <net/netfilter/nf_conntrack_helper.h>
#include <net/netfilter/nf_conntrack_zones.h>
#include <net/netfilter/nf_conntrack_core.h>
#include <linux/if_bridge.h>
#include <linux/netdevice.h>
#include "sfe.h"
#include "sfe_cm.h"
#include "sfe_backport.h"

typedef enum sfe_cm_exception {
	SFE_CM_EXCEPTION_PACKET_BROADCAST,
	SFE_CM_EXCEPTION_PACKET_MULTICAST,
	SFE_CM_EXCEPTION_NO_IIF,
	SFE_CM_EXCEPTION_NO_CT,
	SFE_CM_EXCEPTION_CT_NO_TRACK,
	SFE_CM_EXCEPTION_CT_NO_CONFIRM,
	SFE_CM_EXCEPTION_CT_IS_ALG,
	SFE_CM_EXCEPTION_IS_IPV4_MCAST,
	SFE_CM_EXCEPTION_IS_IPV6_MCAST,
	SFE_CM_EXCEPTION_TCP_NOT_ASSURED,
	SFE_CM_EXCEPTION_TCP_NOT_ESTABLISHED,
	SFE_CM_EXCEPTION_UNKNOW_PROTOCOL,
	SFE_CM_EXCEPTION_NO_SRC_DEV,
	SFE_CM_EXCEPTION_NO_SRC_XLATE_DEV,
	SFE_CM_EXCEPTION_NO_DEST_DEV,
	SFE_CM_EXCEPTION_NO_DEST_XLATE_DEV,
	SFE_CM_EXCEPTION_NO_BRIDGE,
	SFE_CM_EXCEPTION_LOCAL_OUT,
	SFE_CM_EXCEPTION_MAX
} sfe_cm_exception_t;

static char *sfe_cm_exception_events_string[SFE_CM_EXCEPTION_MAX] = {
	"PACKET_BROADCAST",
	"PACKET_MULTICAST",
	"NO_IIF",
	"NO_CT",
	"CT_NO_TRACK",
	"CT_NO_CONFIRM",
	"CT_IS_ALG",
	"IS_IPV4_MCAST",
	"IS_IPV6_MCAST",
	"TCP_NOT_ASSURED",
	"TCP_NOT_ESTABLISHED",
	"UNKNOW_PROTOCOL",
	"NO_SRC_DEV",
	"NO_SRC_XLATE_DEV",
	"NO_DEST_DEV",
	"NO_DEST_XLATE_DEV",
	"NO_BRIDGE",
	"LOCAL_OUT"
};

/*
 * Per-module structure.
 */
struct sfe_cm {
	spinlock_t lock;		/* Lock for SMP correctness */

	/*
	 * Control state.
	 */
	struct kobject *sys_sfe_cm;	/* sysfs linkage */

	/*
	 * Callback notifiers.
	 */
	struct notifier_block dev_notifier;
					/* Device notifier */
	struct notifier_block inet_notifier;
					/* IPv4 notifier */
	struct notifier_block inet6_notifier;
					/* IPv6 notifier */
	uint32_t exceptions[SFE_CM_EXCEPTION_MAX];
};

struct sfe_cm __sc;

/*
 * Expose the hook for the receive processing.
 */
extern int (*athrs_fast_nat_recv)(struct sk_buff *skb,struct packet_type *pt_tmp);
extern void (*delete_sfe_entry)(struct nf_conn *ct);

/*
 * Expose what should be a static flag in the TCP connection tracker.
 */
extern int nf_ct_tcp_no_window_check;
/*
 * sfe_cm_incr_exceptions()
 *	increase an exception counter.
 */
 /*sfe function */
static inline void sfe_cm_incr_exceptions(sfe_cm_exception_t except)
{
	struct sfe_cm *sc = &__sc;

	spin_lock_bh(&sc->lock);
	sc->exceptions[except]++;
	spin_unlock_bh(&sc->lock);
}

/*
 * sfe_cm_recv()
 *	Handle packet receives.
 *
 * Returns 1 if the packet is forwarded or 0 if it isn't.
 */
int sfe_cm_recv(struct sk_buff *skb,struct packet_type *pt_tmp)
{
	struct net_device *dev;

	/*
	 * We know that for the vast majority of packets we need the transport
	 * layer header so we may as well start to fetch it now!
	 */
	prefetch(skb->data + 32);
	barrier();

	dev = skb->dev;

	/*
	 * We're only interested in IPv4 and IPv6 packets.
	 */
	if (likely(htons(ETH_P_IP) == skb->protocol)) {
#if (SFE_HOOK_ABOVE_BRIDGE)
		struct in_device *in_dev;

		/*
		 * Does our input device support IP processing?
		 */
		in_dev = (struct in_device *)dev->ip_ptr;
		if (unlikely(!in_dev)) {
			DEBUG_TRACE("no IP processing for device: %s\n", dev->name);
			return 0;
		}

		/*
		 * Does it have an IP address?  If it doesn't then we can't do anything
		 * interesting here!
		 */
		if (unlikely(!in_dev->ifa_list)) {
			DEBUG_TRACE("no IP address for device: %s\n", dev->name);
			return 0;
		}
#endif

		return sfe_ipv4_recv(dev, skb, pt_tmp);
	}

	if (likely(htons(ETH_P_IPV6) == skb->protocol)) {
#if (SFE_HOOK_ABOVE_BRIDGE)
		struct inet6_dev *in_dev;

		/*
		 * Does our input device support IPv6 processing?
		 */
		in_dev = (struct inet6_dev *)dev->ip6_ptr;
		if (unlikely(!in_dev)) {
			DEBUG_TRACE("no IPv6 processing for device: %s\n", dev->name);
			return 0;
		}

		/*
		 * Does it have an IPv6 address?  If it doesn't then we can't do anything
		 * interesting here!
		 */
		if (unlikely(list_empty(&in_dev->addr_list))) {
			DEBUG_TRACE("no IPv6 address for device: %s\n", dev->name);
			return 0;
		}
#endif

		return sfe_ipv6_recv(dev, skb, pt_tmp);
	}

	DEBUG_TRACE("not IP packet\n");
	return 0;
}

/*
 * sfe_cm_delete_conntrack()
 *	Handle SFE entry deletion based on conntrack deletion.
 * Returns void
 */

static void sfe_cm_delete_conntrack (struct nf_conn *ct)
{
	struct sfe_connection_destroy sid;
	struct nf_conntrack_tuple orig_tuple;

	if (unlikely(!ct)) {
		DEBUG_WARN("no ct in conntrack event callback\n");
		return;
	}

	orig_tuple = ct->tuplehash[IP_CT_DIR_ORIGINAL].tuple;
	sid.protocol = (int32_t)orig_tuple.dst.protonum;

	/*
	 * Extract information from the conntrack connection.  We're only interested
	 * in nominal connection information (i.e. we're ignoring any NAT information).
	 */

	switch (sid.protocol) {
	case IPPROTO_TCP:
		sid.src_port = orig_tuple.src.u.tcp.port;
		sid.dest_port = orig_tuple.dst.u.tcp.port;
		break;

	case IPPROTO_UDP:
		sid.src_port = orig_tuple.src.u.udp.port;
		sid.dest_port = orig_tuple.dst.u.udp.port;
		break;

	default:
		DEBUG_TRACE("unhandled protocol: %d\n", sid.protocol);
		return;
	}

	if (likely(nf_ct_l3num(ct) == AF_INET)) {
		sid.src_ip.ip = (__be32)orig_tuple.src.u3.ip;
		sid.dest_ip.ip = (__be32)orig_tuple.dst.u3.ip;

	sfe_ipv4_destroy_rule(&sid);
	} else if (likely(nf_ct_l3num(ct) == AF_INET6)) {
		sid.src_ip.ip6[0] = *((struct sfe_ipv6_addr *)&orig_tuple.src.u3.in6);
		sid.dest_ip.ip6[0] = *((struct sfe_ipv6_addr *)&orig_tuple.dst.u3.in6);

		sfe_ipv6_destroy_rule(&sid);
	} else {
		DEBUG_TRACE("ignoring non-IPv4 and non-IPv6 connection\n");
	}
}

/*
 * sfe_cm_find_dev_and_mac_addr()
 *	Find the device and MAC address for a given IPv4/IPv6 address.
 *
 * Returns true if we find the device and MAC address, otherwise false.
 *
 * We look up the rtable entry for the address and, from its neighbour
 * structure, obtain the hardware address.  This means this function also
 * works if the neighbours are routers too.
 */
static bool sfe_cm_find_dev_and_mac_addr(sfe_ip_addr_t *addr, struct net_device **dev, uint8_t *mac_addr, int is_v4)
{
	struct neighbour *neigh;
	struct rtable *rt;
	struct rt6_info *rt6;
	struct dst_entry *dst;
	struct net_device *mac_dev;

	/*
	 * Look up the rtable entry for the IP address then get the hardware
	 * address from its neighbour structure.  This means this work when the
	 * neighbours are routers too.
	 */
	if (likely(is_v4)) {
		rt = ip_route_output(&init_net, addr->ip, 0, 0, 0);
		if (unlikely(IS_ERR(rt))) {
			goto ret_fail;
		}

		dst = (struct dst_entry *)rt;
	} else {
		rt6 = rt6_lookup(&init_net, (struct in6_addr *)addr->ip6, 0, 0, 0);
		if (!rt6) {
			goto ret_fail;
		}

		dst = (struct dst_entry *)rt6;
	}

	rcu_read_lock();
	neigh = dst_neigh_lookup(dst, addr);
	if (unlikely(!neigh)) {
		rcu_read_unlock();
		dst_release(dst);
		goto ret_fail;
	}

	if (unlikely(!(neigh->nud_state & NUD_VALID))) {
		rcu_read_unlock();
		neigh_release(neigh);
		dst_release(dst);
		goto ret_fail;
	}

	mac_dev = neigh->dev;
	if (!mac_dev) {
		rcu_read_unlock();
		neigh_release(neigh);
		dst_release(dst);
		goto ret_fail;
	}

	memcpy(mac_addr, neigh->ha, (size_t)mac_dev->addr_len);

	dev_hold(mac_dev);
	*dev = mac_dev;
	rcu_read_unlock();
	neigh_release(neigh);
	dst_release(dst);

	return true;

ret_fail:
	if (is_v4) {
		DEBUG_TRACE("failed to find MAC address for IP: %pI4\n", &addr->ip);

	} else {
		DEBUG_TRACE("failed to find MAC address for IP: %pI6\n", addr->ip6);
	}

	return false;
}

/*
 * sfe_cm_post_routing()
 *	Called for packets about to leave the box - either locally generated or forwarded from another interface
 */
static unsigned int sfe_cm_post_routing(struct sk_buff *skb, int is_v4)
{
	struct sfe_connection_create sic;
	struct net_device *in;
	struct nf_conn *ct;
	enum ip_conntrack_info ctinfo;
	struct net_device *dev;
	struct net_device *src_dev;
	struct net_device *dest_dev;
	struct net_device *src_dev_use;
	struct net_device *dest_dev_use;
	struct net_device *src_br_dev = NULL;
	struct net_device *dest_br_dev = NULL;
	struct nf_conntrack_tuple orig_tuple;
	struct nf_conntrack_tuple reply_tuple;
	struct net *net = NULL;
	struct nf_tcp_net *tcp_net = NULL;
	struct nf_conn_acct *acct = NULL;
	struct nf_conn_counter *counter = NULL;
	u64 pkts;

	/*
	 * Don't process broadcast or multicast packets.
	 */
	if (unlikely(skb->pkt_type == PACKET_BROADCAST)) {
		sfe_cm_incr_exceptions(SFE_CM_EXCEPTION_PACKET_BROADCAST);
		DEBUG_TRACE("broadcast, ignoring\n");
		return NF_ACCEPT;
	}
	if (unlikely(skb->pkt_type == PACKET_MULTICAST)) {
		sfe_cm_incr_exceptions(SFE_CM_EXCEPTION_PACKET_MULTICAST);
		DEBUG_TRACE("multicast, ignoring\n");
		return NF_ACCEPT;
	}

#ifdef CONFIG_XFRM
	/*
	 * Packet to xfrm for encapsulation, we can't process it
	 */
	if (unlikely(skb_dst(skb)->xfrm)) {
		DEBUG_TRACE("packet to xfrm, ignoring\n");
		return NF_ACCEPT;
	}
#endif

	/*
	 * Don't process locally generated packets.
	 */
	if (skb->sk) {
		sfe_cm_incr_exceptions(SFE_CM_EXCEPTION_LOCAL_OUT);
		DEBUG_TRACE("skip local out packet\n");
		return NF_ACCEPT;
	}

	/*
	 * Don't process packets that are not being forwarded.
	 */
	in = dev_get_by_index(&init_net, skb->skb_iif);
	if (!in) {
		sfe_cm_incr_exceptions(SFE_CM_EXCEPTION_NO_IIF);
		DEBUG_TRACE("packet not forwarding\n");
		return NF_ACCEPT;
	}

	dev_put(in);

	net = dev_net(in);

	/*
	 * Don't process packets that aren't being tracked by conntrack.
	 */
	ct = nf_ct_get(skb, &ctinfo);
	if (unlikely(!ct)) {
		sfe_cm_incr_exceptions(SFE_CM_EXCEPTION_NO_CT);
		DEBUG_TRACE("no conntrack connection, ignoring\n");
		return NF_ACCEPT;
	}

	/*
	 * Don't process untracked connections.
	 */
	if (unlikely(ct == &nf_conntrack_untracked)) {
		sfe_cm_incr_exceptions(SFE_CM_EXCEPTION_CT_NO_TRACK);
		DEBUG_TRACE("untracked connection\n");
		return NF_ACCEPT;
	}

	/*
	 * Unconfirmed connection may be dropped by Linux at the final step,
	 * So we don't process unconfirmed connections.
	 */
	if (!nf_ct_is_confirmed(ct)) {
		sfe_cm_incr_exceptions(SFE_CM_EXCEPTION_CT_NO_CONFIRM);
		DEBUG_TRACE("unconfirmed connection\n");
		return NF_ACCEPT;
	}

	/*
	 * Don't process connections that require support from a 'helper' (typically a NAT ALG).
	 */
	if (unlikely(nfct_help(ct))) {
		sfe_cm_incr_exceptions(SFE_CM_EXCEPTION_CT_IS_ALG);
		DEBUG_TRACE("connection has helper\n");
		return NF_ACCEPT;
	}

	/*
	 * Do not create sfe entry until the initial packet threshold is met
	 */
	acct = nf_conn_acct_find(ct);
	if (acct) {
			counter = acct->counter;
			/*
			 * Packet threshold here is the aggregate of packets in both
			 * the directions.
			 */
			pkts = atomic64_read(&counter[CTINFO2DIR(ctinfo)].packets) +
			       atomic64_read(&counter[!CTINFO2DIR(ctinfo)].packets);
			/* Report if the packet threshold is reached. */
			if ((nf_conntrack_pkt_threshold > 0) &&
			    (pkts < nf_conntrack_pkt_threshold)) {
					DEBUG_TRACE("Initial packet threshold still not reached\n");
					return NF_ACCEPT;
			}
	}

	/*
	 * Look up the details of our connection in conntrack.
	 *
	 * Note that the data we get from conntrack is for the "ORIGINAL" direction
	 * but our packet may actually be in the "REPLY" direction.
	 */
	orig_tuple = ct->tuplehash[IP_CT_DIR_ORIGINAL].tuple;
	reply_tuple = ct->tuplehash[IP_CT_DIR_REPLY].tuple;
	sic.protocol = (int32_t)orig_tuple.dst.protonum;

	/*
	 * Get addressing information, non-NAT first
	 */
	if (likely(is_v4)) {
		sic.src_ip.ip = (__be32)orig_tuple.src.u3.ip;
		sic.dest_ip.ip = (__be32)orig_tuple.dst.u3.ip;

		if (ipv4_is_multicast(sic.src_ip.ip) || ipv4_is_multicast(sic.dest_ip.ip)) {
			sfe_cm_incr_exceptions(SFE_CM_EXCEPTION_IS_IPV4_MCAST);
			DEBUG_TRACE("multicast address\n");
			return NF_ACCEPT;
		}

		/*
		 * NAT'ed addresses - note these are as seen from the 'reply' direction
		 * When NAT does not apply to this connection these will be identical to the above.
		 */
		sic.src_ip_xlate.ip = (__be32)reply_tuple.dst.u3.ip;
		sic.dest_ip_xlate.ip = (__be32)reply_tuple.src.u3.ip;
	} else {
		sic.src_ip.ip6[0] = *((struct sfe_ipv6_addr *)&orig_tuple.src.u3.in6);
		sic.dest_ip.ip6[0] = *((struct sfe_ipv6_addr *)&orig_tuple.dst.u3.in6);

		if (ipv6_addr_is_multicast((struct in6_addr *)sic.src_ip.ip6) ||
		    ipv6_addr_is_multicast((struct in6_addr *)sic.dest_ip.ip6)) {
			sfe_cm_incr_exceptions(SFE_CM_EXCEPTION_IS_IPV6_MCAST);
			DEBUG_TRACE("multicast address\n");
			return NF_ACCEPT;
		}

		/*
		 * NAT'ed addresses - note these are as seen from the 'reply' direction
		 * When NAT does not apply to this connection these will be identical to the above.
		 */
		sic.src_ip_xlate.ip6[0] = *((struct sfe_ipv6_addr *)&reply_tuple.dst.u3.in6);
		sic.dest_ip_xlate.ip6[0] = *((struct sfe_ipv6_addr *)&reply_tuple.src.u3.in6);
	}

	sic.flags = 0;

	switch (sic.protocol) {
	case IPPROTO_TCP:
		sic.src_port = orig_tuple.src.u.tcp.port;
		sic.dest_port = orig_tuple.dst.u.tcp.port;
		sic.src_port_xlate = reply_tuple.dst.u.tcp.port;
		sic.dest_port_xlate = reply_tuple.src.u.tcp.port;
		sic.src_td_window_scale = ct->proto.tcp.seen[0].td_scale;
		sic.src_td_max_window = ct->proto.tcp.seen[0].td_maxwin;
		sic.src_td_end = ct->proto.tcp.seen[0].td_end;
		sic.src_td_max_end = ct->proto.tcp.seen[0].td_maxend;
		sic.dest_td_window_scale = ct->proto.tcp.seen[1].td_scale;
		sic.dest_td_max_window = ct->proto.tcp.seen[1].td_maxwin;
		sic.dest_td_end = ct->proto.tcp.seen[1].td_end;
		sic.dest_td_max_end = ct->proto.tcp.seen[1].td_maxend;
		tcp_net = &net->ct.nf_ct_proto.tcp;
		if (tcp_net->tcp_be_liberal
		    || (ct->proto.tcp.seen[0].flags & IP_CT_TCP_FLAG_BE_LIBERAL)
		    || (ct->proto.tcp.seen[1].flags & IP_CT_TCP_FLAG_BE_LIBERAL)) {
			sic.flags |= SFE_CREATE_FLAG_NO_SEQ_CHECK;
		}

		/*
		 * Don't try to manage a non-established connection.
		 */
		if (!test_bit(IPS_ASSURED_BIT, &ct->status)) {
			sfe_cm_incr_exceptions(SFE_CM_EXCEPTION_TCP_NOT_ASSURED);
			DEBUG_TRACE("non-established connection\n");
			return NF_ACCEPT;
		}

		/*
		 * If the connection is shutting down do not manage it.
		 * state can not be SYN_SENT, SYN_RECV because connection is assured
		 * Not managed states: FIN_WAIT, CLOSE_WAIT, LAST_ACK, TIME_WAIT, CLOSE.
		 */
		spin_lock_bh(&ct->lock);
		if (ct->proto.tcp.state != TCP_CONNTRACK_ESTABLISHED) {
			spin_unlock_bh(&ct->lock);
			sfe_cm_incr_exceptions(SFE_CM_EXCEPTION_TCP_NOT_ESTABLISHED);
			DEBUG_TRACE("connection in termination state: %#x, s: %pI4:%u, d: %pI4:%u\n",
				    ct->proto.tcp.state, &sic.src_ip, ntohs(sic.src_port),
				    &sic.dest_ip, ntohs(sic.dest_port));
			return NF_ACCEPT;
		}
		spin_unlock_bh(&ct->lock);
		break;

	case IPPROTO_UDP:
		sic.src_port = orig_tuple.src.u.udp.port;
		sic.dest_port = orig_tuple.dst.u.udp.port;
		sic.src_port_xlate = reply_tuple.dst.u.udp.port;
		sic.dest_port_xlate = reply_tuple.src.u.udp.port;
		break;

	default:
		sfe_cm_incr_exceptions(SFE_CM_EXCEPTION_UNKNOW_PROTOCOL);
		DEBUG_TRACE("unhandled protocol %d\n", sic.protocol);
		return NF_ACCEPT;
	}

#ifdef CONFIG_XFRM
	sic.original_accel = 1;
	sic.reply_accel = 1;

	/*
	 * For packets de-capsulated from xfrm, we still can accelerate it
	 * on the direction we just received the packet.
	 */
	if (unlikely(skb->sp)) {
		if (sic.protocol == IPPROTO_TCP &&
			!(sic.flags & SFE_CREATE_FLAG_NO_SEQ_CHECK)) {
			return NF_ACCEPT;
		}

		if (CTINFO2DIR(ctinfo) == IP_CT_DIR_ORIGINAL) {
			sic.reply_accel = 0;
		} else {
			sic.original_accel = 0;
		}
	}
#endif

	/*
	 * Get the net device and MAC addresses that correspond to the various source and
	 * destination host addresses.
	 */
	if (!sfe_cm_find_dev_and_mac_addr(&sic.src_ip, &src_dev, sic.src_mac, is_v4)) {
		sfe_cm_incr_exceptions(SFE_CM_EXCEPTION_NO_SRC_DEV);
		return NF_ACCEPT;
	}
	src_dev_use = src_dev;

	if (!sfe_cm_find_dev_and_mac_addr(&sic.src_ip_xlate, &dev, sic.src_mac_xlate, is_v4)) {
		sfe_cm_incr_exceptions(SFE_CM_EXCEPTION_NO_SRC_XLATE_DEV);
		goto done1;
	}

	dev_put(dev);

	if (!sfe_cm_find_dev_and_mac_addr(&sic.dest_ip, &dev, sic.dest_mac, is_v4)) {
		sfe_cm_incr_exceptions(SFE_CM_EXCEPTION_NO_DEST_DEV);
		goto done1;
	}

	dev_put(dev);

	if (!sfe_cm_find_dev_and_mac_addr(&sic.dest_ip_xlate, &dest_dev, sic.dest_mac_xlate, is_v4)) {
		sfe_cm_incr_exceptions(SFE_CM_EXCEPTION_NO_DEST_XLATE_DEV);
		goto done1;
	}
	dest_dev_use = dest_dev;

#if (!SFE_HOOK_ABOVE_BRIDGE)
	/*
	 * Now our devices may actually be a bridge interface.  If that's
	 * the case then we need to hunt down the underlying interface.
	 */
	if (src_dev->priv_flags & IFF_EBRIDGE) {
		src_br_dev = br_port_dev_get(src_dev, sic.src_mac);
		if (!src_br_dev) {
			sfe_cm_incr_exceptions(SFE_CM_EXCEPTION_NO_BRIDGE);
			DEBUG_TRACE("no port found on bridge\n");
			goto done2;
		}
		src_dev_use = src_br_dev;
	}

	if (dest_dev->priv_flags & IFF_EBRIDGE) {
		dest_br_dev = br_port_dev_get(dest_dev, sic.dest_mac_xlate);
		if (!dest_br_dev) {
			sfe_cm_incr_exceptions(SFE_CM_EXCEPTION_NO_BRIDGE);
			DEBUG_TRACE("no port found on bridge\n");
			goto done3;
		}
		dest_dev_use = dest_br_dev;
	}
#else
	/*
	 * Our devices may actually be part of a bridge interface.  If that's
	 * the case then find the bridge interface instead.
	 */
	if (src_dev->priv_flags & IFF_BRIDGE_PORT) {
		src_br_dev = sfe_dev_get_master(src_dev);
		if (!src_br_dev) {
			sfe_cm_incr_exceptions(SFE_CM_EXCEPTION_NO_BRIDGE);
			DEBUG_TRACE("no bridge found for: %s\n", src_dev->name);
			goto done2;
		}
		src_dev_use = src_br_dev;
	}

	if (dest_dev->priv_flags & IFF_BRIDGE_PORT) {
		dest_br_dev = sfe_dev_get_master(dest_dev);
		if (!dest_br_dev) {
			sfe_cm_incr_exceptions(SFE_CM_EXCEPTION_NO_BRIDGE);
			DEBUG_TRACE("no bridge found for: %s\n", dest_dev->name);
			goto done3;
		}

		dest_dev_use = dest_br_dev;
	}
#endif

	sic.src_dev = src_dev_use;
	sic.dest_dev = dest_dev_use;

	sic.src_mtu = src_dev_use->mtu;
	sic.dest_mtu = dest_dev_use->mtu;

	if (likely(is_v4)) {
		if (sfe_ipv4_create_rule(&sic) == 0) {
				ct->sfe_entry = (void *)(&sic);
			}
	} else {
		if (sfe_ipv6_create_rule(&sic) == 0) {
				ct->sfe_entry = (void *)(&sic);
			}
	}

	/*
	 * If we had bridge ports then release them too.
	 */
	if (dest_br_dev) {
		dev_put(dest_br_dev);
	}

done3:
	if (src_br_dev) {
		dev_put(src_br_dev);
	}

done2:
	dev_put(dest_dev);

done1:
	dev_put(src_dev);

	return NF_ACCEPT;
}

/*
 * sfe_cm_ipv4_post_routing_hook()
 *	Called for packets about to leave the box - either locally generated or forwarded from another interface
 */
sfe_cm_ipv4_post_routing_hook(hooknum, ops, skb, in_unused, out, okfn)
{
	return sfe_cm_post_routing(skb, true);
}

/*
 * sfe_cm_ipv6_post_routing_hook()
 *	Called for packets about to leave the box - either locally generated or forwarded from another interface
 */
sfe_cm_ipv6_post_routing_hook(hooknum, ops, skb, in_unused, out, okfn)
{
	return sfe_cm_post_routing(skb, false);
}


#if 0
/*
 * sfe_cm_conntrack_event()
 *	Callback event invoked when a conntrack connection's state changes.
 */
#ifdef CONFIG_NF_CONNTRACK_CHAIN_EVENTS
static int sfe_cm_conntrack_event(struct notifier_block *this,
			unsigned long events, void *ptr)
#else
static int sfe_cm_conntrack_event(unsigned int events, struct nf_ct_event *item)
#endif
{
#ifdef CONFIG_NF_CONNTRACK_CHAIN_EVENTS
	struct nf_ct_event *item = ptr;
#endif
	struct sfe_connection_destroy sid;
	struct nf_conn *ct = item->ct;
	struct nf_conntrack_tuple orig_tuple;

	/*
	 * If we don't have a conntrack entry then we're done.
	 */
	if (unlikely(!ct)) {
		DEBUG_WARN("no ct in conntrack event callback\n");
		return NOTIFY_DONE;
	}

	/*
	 * If this is an untracked connection then we can't have any state either.
	 */
	if (unlikely(ct == &nf_conntrack_untracked)) {
		DEBUG_TRACE("ignoring untracked conn\n");
		return NOTIFY_DONE;
	}

	/*
	 * We're only interested in destroy events.
	 */
	if (unlikely(!(events & (1 << IPCT_DESTROY)))) {
		DEBUG_TRACE("ignoring non-destroy event\n");
		return NOTIFY_DONE;
	}

	orig_tuple = ct->tuplehash[IP_CT_DIR_ORIGINAL].tuple;
	sid.protocol = (int32_t)orig_tuple.dst.protonum;

	/*
	 * Extract information from the conntrack connection.  We're only interested
	 * in nominal connection information (i.e. we're ignoring any NAT information).
	 */
	switch (sid.protocol) {
	case IPPROTO_TCP:
		sid.src_port = orig_tuple.src.u.tcp.port;
		sid.dest_port = orig_tuple.dst.u.tcp.port;
		break;

	case IPPROTO_UDP:
		sid.src_port = orig_tuple.src.u.udp.port;
		sid.dest_port = orig_tuple.dst.u.udp.port;
		break;

	default:
		DEBUG_TRACE("unhandled protocol: %d\n", sid.protocol);
		return NOTIFY_DONE;
	}

	if (likely(nf_ct_l3num(ct) == AF_INET)) {
		sid.src_ip.ip = (__be32)orig_tuple.src.u3.ip;
		sid.dest_ip.ip = (__be32)orig_tuple.dst.u3.ip;

		sfe_ipv4_destroy_rule(&sid);
	} else if (likely(nf_ct_l3num(ct) == AF_INET6)) {
		sid.src_ip.ip6[0] = *((struct sfe_ipv6_addr *)&orig_tuple.src.u3.in6);
		sid.dest_ip.ip6[0] = *((struct sfe_ipv6_addr *)&orig_tuple.dst.u3.in6);

		sfe_ipv6_destroy_rule(&sid);
	} else {
		DEBUG_TRACE("ignoring non-IPv4 and non-IPv6 connection\n");
	}

	return NOTIFY_DONE;
}

#endif

/*
 * Structure to establish a hook into the post routing netfilter point - this
 * will pick up local outbound and packets going from one interface to another.
 *
 * Note: see include/linux/netfilter_ipv4.h for info related to priority levels.
 * We want to examine packets after NAT translation and any ALG processing.
 */
static struct nf_hook_ops sfe_cm_ops_post_routing[] __read_mostly = {
	{
		.hook = __sfe_cm_ipv4_post_routing_hook,
		.owner = THIS_MODULE,
		.pf = NFPROTO_IPV4,
		.hooknum = NF_INET_POST_ROUTING,
		.priority = NF_IP_PRI_NAT_SRC + 1,
	},
#ifdef SFE_SUPPORT_IPV6
	{
		.hook = __sfe_cm_ipv6_post_routing_hook,
		.owner = THIS_MODULE,
		.pf = NFPROTO_IPV6,
		.hooknum = NF_INET_POST_ROUTING,
		.priority = NF_IP6_PRI_NAT_SRC + 1,
	},
#endif
};

/*
 * sfe_cm_sync_rule()
 *	Synchronize a connection's state.
 */
static void sfe_cm_sync_rule(struct sfe_connection_sync *sis)
{
	struct nf_conntrack_tuple_hash *h;
	struct nf_conntrack_tuple tuple;
	struct nf_conn *ct;
	SFE_NF_CONN_ACCT(acct);

	/*
	 * Create a tuple so as to be able to look up a connection
	 */
	memset(&tuple, 0, sizeof(tuple));
	tuple.src.u.all = (__be16)sis->src_port;
	tuple.dst.dir = IP_CT_DIR_ORIGINAL;
	tuple.dst.protonum = (uint8_t)sis->protocol;
	tuple.dst.u.all = (__be16)sis->dest_port;

	if (sis->is_v6) {
		tuple.src.u3.in6 = *((struct in6_addr *)sis->src_ip.ip6);
		tuple.dst.u3.in6 = *((struct in6_addr *)sis->dest_ip.ip6);
		tuple.src.l3num = AF_INET6;

		DEBUG_TRACE("update connection - p: %d, s: %pI6:%u, d: %pI6:%u\n",
			    (int)tuple.dst.protonum,
			    &tuple.src.u3.in6, (unsigned int)ntohs(tuple.src.u.all),
			    &tuple.dst.u3.in6, (unsigned int)ntohs(tuple.dst.u.all));
	} else {
		tuple.src.u3.ip = sis->src_ip.ip;
		tuple.dst.u3.ip = sis->dest_ip.ip;
		tuple.src.l3num = AF_INET;

		DEBUG_TRACE("update connection - p: %d, s: %pI4:%u, d: %pI4:%u\n",
			    (int)tuple.dst.protonum,
			    &tuple.src.u3.ip, (unsigned int)ntohs(tuple.src.u.all),
			    &tuple.dst.u3.ip, (unsigned int)ntohs(tuple.dst.u.all));
	}

	/*
	 * Look up conntrack connection
	 */
	h = nf_conntrack_find_get(&init_net, NF_CT_DEFAULT_ZONE, &tuple);
	if (unlikely(!h)) {
		DEBUG_TRACE("no connection found\n");
		return;
	}

	ct = nf_ct_tuplehash_to_ctrack(h);
	NF_CT_ASSERT(ct->timeout.data == (unsigned long)ct);

	/*
	 * Only update if this is not a fixed timeout
	 */
	if (!test_bit(IPS_FIXED_TIMEOUT_BIT, &ct->status)) {
		spin_lock_bh(&ct->lock);
		ct->timeout.expires += sis->delta_jiffies;
		spin_unlock_bh(&ct->lock);
	}

	acct = nf_conn_acct_find(ct);
	if (acct) {
		spin_lock_bh(&ct->lock);
		atomic64_set(&SFE_ACCT_COUNTER(acct)[IP_CT_DIR_ORIGINAL].packets, sis->src_packet_count);
		atomic64_set(&SFE_ACCT_COUNTER(acct)[IP_CT_DIR_ORIGINAL].bytes, sis->src_byte_count);
		atomic64_set(&SFE_ACCT_COUNTER(acct)[IP_CT_DIR_REPLY].packets, sis->dest_packet_count);
		atomic64_set(&SFE_ACCT_COUNTER(acct)[IP_CT_DIR_REPLY].bytes, sis->dest_byte_count);
		spin_unlock_bh(&ct->lock);
	}

	switch (sis->protocol) {
	case IPPROTO_TCP:
		spin_lock_bh(&ct->lock);
		if (ct->proto.tcp.seen[0].td_maxwin < sis->src_td_max_window) {
			ct->proto.tcp.seen[0].td_maxwin = sis->src_td_max_window;
		}
		if ((int32_t)(ct->proto.tcp.seen[0].td_end - sis->src_td_end) < 0) {
			ct->proto.tcp.seen[0].td_end = sis->src_td_end;
		}
		if ((int32_t)(ct->proto.tcp.seen[0].td_maxend - sis->src_td_max_end) < 0) {
			ct->proto.tcp.seen[0].td_maxend = sis->src_td_max_end;
		}
		if (ct->proto.tcp.seen[1].td_maxwin < sis->dest_td_max_window) {
			ct->proto.tcp.seen[1].td_maxwin = sis->dest_td_max_window;
		}
		if ((int32_t)(ct->proto.tcp.seen[1].td_end - sis->dest_td_end) < 0) {
			ct->proto.tcp.seen[1].td_end = sis->dest_td_end;
		}
		if ((int32_t)(ct->proto.tcp.seen[1].td_maxend - sis->dest_td_max_end) < 0) {
			ct->proto.tcp.seen[1].td_maxend = sis->dest_td_max_end;
		}
		spin_unlock_bh(&ct->lock);
		break;
	}

	/*
	 * Release connection
	 */
	nf_ct_put(ct);
}

/*
 * sfe_cm_device_event()
 */
int sfe_cm_device_event(struct notifier_block *this, unsigned long event, void *ptr)
{
	struct net_device *dev = SFE_DEV_EVENT_PTR(ptr);

	switch (event) {
	case NETDEV_DOWN:
		if (dev) {
			sfe_ipv4_destroy_all_rules_for_dev(dev);
			sfe_ipv6_destroy_all_rules_for_dev(dev);
		}
		break;
	}

	return NOTIFY_DONE;
}

/*
 * sfe_cm_inet_event()
 */
static int sfe_cm_inet_event(struct notifier_block *this, unsigned long event, void *ptr)
{
	struct net_device *dev = ((struct in_ifaddr *)ptr)->ifa_dev->dev;
	return sfe_cm_propagate_event(this, event, dev);
}

/*
 * sfe_cm_inet6_event()
 */
static int sfe_cm_inet6_event(struct notifier_block *this, unsigned long event, void *ptr)
{
	struct net_device *dev = ((struct inet6_ifaddr *)ptr)->idev->dev;
	return sfe_cm_propagate_event(this, event, dev);
}

/*
 * sfe_cm_get_exceptions
 * 	dump exception counters
 */
static ssize_t sfe_cm_get_exceptions(struct device *dev,
				     struct device_attribute *attr,
				     char *buf)
{
	int idx, len;
	struct sfe_cm *sc = &__sc;

	spin_lock_bh(&sc->lock);
	for (len = 0, idx = 0; idx < SFE_CM_EXCEPTION_MAX; idx++) {
		if (sc->exceptions[idx]) {
			len += snprintf(buf + len, SFE_MAX_CMD_LEN,
					 "%s = %d\n", sfe_cm_exception_events_string[idx], sc->exceptions[idx]);
		}
	}
	spin_unlock_bh(&sc->lock);

	return len;
}

/*
 * sysfs attributes.
 */
static const struct device_attribute sfe_cm_exceptions_attr =
	__ATTR(exceptions, S_IRUGO, sfe_cm_get_exceptions, NULL);

/*
 * sfe_cm_init()
 */
static int __init sfe_cm_init(void)
{
	struct sfe_cm *sc = &__sc;
	int result = -1;

	DEBUG_INFO("SFE CM init\n");

	/*
	 * Create sys/sfe_cm
	 */
	sc->sys_sfe_cm = kobject_create_and_add("sfe_cm", NULL);
	if (!sc->sys_sfe_cm) {
		DEBUG_ERROR("failed to register sfe_cm\n");
		goto exit1;
	}

	/*
	 * Create sys/sfe_cm/exceptions
	 */
	result = sysfs_create_file(sc->sys_sfe_cm, &sfe_cm_exceptions_attr.attr);
	if (result) {
		DEBUG_ERROR("failed to register exceptions file: %d\n", result);
		goto exit2;
	}

	sc->dev_notifier.notifier_call = sfe_cm_device_event;
	sc->dev_notifier.priority = 1;
	register_netdevice_notifier(&sc->dev_notifier);

	sc->inet_notifier.notifier_call = sfe_cm_inet_event;
	sc->inet_notifier.priority = 1;
	register_inetaddr_notifier(&sc->inet_notifier);

	sc->inet6_notifier.notifier_call = sfe_cm_inet6_event;
	sc->inet6_notifier.priority = 1;
	register_inet6addr_notifier(&sc->inet6_notifier);
	/*
	 * Register our netfilter hooks.
	 */
	result = nf_register_hooks(sfe_cm_ops_post_routing, ARRAY_SIZE(sfe_cm_ops_post_routing));
	if (result < 0) {
		DEBUG_ERROR("can't register nf post routing hook: %d\n", result);
		goto exit3;
	}

	spin_lock_init(&sc->lock);

	/*
	 * Hook the receive path in the network stack.
	 */
	BUG_ON(athrs_fast_nat_recv != NULL);
	RCU_INIT_POINTER(athrs_fast_nat_recv, sfe_cm_recv);

	/*
	 * Register the delete conntrack callback.
	 */
	BUG_ON(delete_sfe_entry != NULL);
	RCU_INIT_POINTER(delete_sfe_entry, sfe_cm_delete_conntrack);

	/*
	 * Hook the shortcut sync callback.
	 */
	sfe_ipv4_register_sync_rule_callback(sfe_cm_sync_rule);
	sfe_ipv6_register_sync_rule_callback(sfe_cm_sync_rule);
	return 0;


exit3:
	unregister_inet6addr_notifier(&sc->inet6_notifier);
	unregister_inetaddr_notifier(&sc->inet_notifier);
	unregister_netdevice_notifier(&sc->dev_notifier);
exit2:
	kobject_put(sc->sys_sfe_cm);

exit1:
	return result;
}

/*
 * sfe_cm_exit()
 */
static void __exit sfe_cm_exit(void)
{
	struct sfe_cm *sc = &__sc;

	DEBUG_INFO("SFE CM exit\n");

	/*
	 * Unregister our sync callback.
	 */
	sfe_ipv4_register_sync_rule_callback(NULL);
	sfe_ipv6_register_sync_rule_callback(NULL);

	/*
	 * Unregister our receive callback.
	 */
	RCU_INIT_POINTER(athrs_fast_nat_recv, NULL);

	/*
	 * Unregister the delete conntrack callback.
	 */
	RCU_INIT_POINTER(delete_sfe_entry, NULL);

	/*
	 * Wait for all callbacks to complete.
	 */
	rcu_barrier();

	/*
	 * Destroy all connections.
	 */
	sfe_ipv4_destroy_all_rules_for_dev(NULL);
	sfe_ipv6_destroy_all_rules_for_dev(NULL);

	nf_unregister_hooks(sfe_cm_ops_post_routing, ARRAY_SIZE(sfe_cm_ops_post_routing));

	unregister_inet6addr_notifier(&sc->inet6_notifier);
	unregister_inetaddr_notifier(&sc->inet_notifier);
	unregister_netdevice_notifier(&sc->dev_notifier);

	kobject_put(sc->sys_sfe_cm);
}

module_init(sfe_cm_init)
module_exit(sfe_cm_exit)
MODULE_AUTHOR("Qualcomm Atheros Inc.");
MODULE_DESCRIPTION("Shortcut Forwarding Engine - Connection Manager");
MODULE_LICENSE("Dual BSD/GPL");
