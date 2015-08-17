/*
 * Copyright (c) 2007-2014 Nicira, Inc.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of version 2 of the GNU General Public
 * License as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301, USA
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/skbuff.h>
#include <linux/in.h>
#include <linux/ip.h>
#include <linux/openvswitch.h>
#include <linux/sctp.h>
#include <linux/tcp.h>
#include <linux/udp.h>
#include <linux/in6.h>
#include <linux/if_arp.h>
#include <linux/if_vlan.h>

#include <net/ip.h>
#include <net/ipv6.h>
#include <net/checksum.h>
#include <net/dsfield.h>
#include <net/mpls.h>
#include <net/sctp/checksum.h>

#include "datapath.h"
#include "flow.h"
#include "vport.h"

static int do_execute_actions(struct datapath *dp, struct sk_buff *skb,
			      struct sw_flow_key *key,
			      const struct nlattr *attr, int len);

struct deferred_action {
	struct sk_buff *skb;
	const struct nlattr *actions;

	/* Store pkt_key clone when creating deferred action. */
	struct sw_flow_key pkt_key;
};

#define DEFERRED_ACTION_FIFO_SIZE 10
struct action_fifo {
	int head;
	int tail;
	/* Deferred action fifo queue storage. */
	struct deferred_action fifo[DEFERRED_ACTION_FIFO_SIZE];
};

static struct action_fifo __percpu *action_fifos;
static DEFINE_PER_CPU(int, exec_actions_level);

static void action_fifo_init(struct action_fifo *fifo)
{
	fifo->head = 0;
	fifo->tail = 0;
}

static bool action_fifo_is_empty(const struct action_fifo *fifo)
{
	return (fifo->head == fifo->tail);
}

static struct deferred_action *action_fifo_get(struct action_fifo *fifo)
{
	if (action_fifo_is_empty(fifo))
		return NULL;

	return &fifo->fifo[fifo->tail++];
}

static struct deferred_action *action_fifo_put(struct action_fifo *fifo)
{
	if (fifo->head >= DEFERRED_ACTION_FIFO_SIZE - 1)
		return NULL;

	return &fifo->fifo[fifo->head++];
}

/* Return true if fifo is not full */
static struct deferred_action *add_deferred_actions(struct sk_buff *skb,
						    const struct sw_flow_key *key,
						    const struct nlattr *attr)
{
	struct action_fifo *fifo;
	struct deferred_action *da;

	fifo = this_cpu_ptr(action_fifos);
	da = action_fifo_put(fifo);
	if (da) {
		da->skb = skb;
		da->actions = attr;
		da->pkt_key = *key;
	}

	return da;
}

static void invalidate_flow_key(struct sw_flow_key *key)
{
	key->eth.type = htons(0);
}

static bool is_flow_key_valid(const struct sw_flow_key *key)
{
	return !!key->eth.type;
}

static int push_mpls(struct sk_buff *skb, struct sw_flow_key *key,
		     const struct ovs_action_push_mpls *mpls)
{
	__be32 *new_mpls_lse;
	struct ethhdr *hdr;

	/* Networking stack do not allow simultaneous Tunnel and MPLS GSO. */
	if (skb->encapsulation)
		return -ENOTSUPP;

	if (skb_cow_head(skb, MPLS_HLEN) < 0)
		return -ENOMEM;

	skb_push(skb, MPLS_HLEN);
	memmove(skb_mac_header(skb) - MPLS_HLEN, skb_mac_header(skb),
		skb->mac_len);
	skb_reset_mac_header(skb);

	new_mpls_lse = (__be32 *)skb_mpls_header(skb);
	*new_mpls_lse = mpls->mpls_lse;

	if (skb->ip_summed == CHECKSUM_COMPLETE)
		skb->csum = csum_add(skb->csum, csum_partial(new_mpls_lse,
							     MPLS_HLEN, 0));

	hdr = eth_hdr(skb);
	hdr->h_proto = mpls->mpls_ethertype;

	skb_set_inner_protocol(skb, skb->protocol);
	skb->protocol = mpls->mpls_ethertype;

	invalidate_flow_key(key);
	return 0;
}

static int pop_mpls(struct sk_buff *skb, struct sw_flow_key *key,
		    const __be16 ethertype)
{
	struct ethhdr *hdr;
	int err;

	err = skb_ensure_writable(skb, skb->mac_len + MPLS_HLEN);
	if (unlikely(err))
		return err;

	skb_postpull_rcsum(skb, skb_mpls_header(skb), MPLS_HLEN);

	memmove(skb_mac_header(skb) + MPLS_HLEN, skb_mac_header(skb),
		skb->mac_len);

	__skb_pull(skb, MPLS_HLEN);
	skb_reset_mac_header(skb);

	/* skb_mpls_header() is used to locate the ethertype
	 * field correctly in the presence of VLAN tags.
	 */
	hdr = (struct ethhdr *)(skb_mpls_header(skb) - ETH_HLEN);
	hdr->h_proto = ethertype;
	if (eth_p_mpls(skb->protocol))
		skb->protocol = ethertype;

	invalidate_flow_key(key);
	return 0;
}

static int set_mpls(struct sk_buff *skb, struct sw_flow_key *key,
		    const __be32 *mpls_lse)
{
	__be32 *stack;
	int err;

	err = skb_ensure_writable(skb, skb->mac_len + MPLS_HLEN);
	if (unlikely(err))
		return err;

	stack = (__be32 *)skb_mpls_header(skb);
	if (skb->ip_summed == CHECKSUM_COMPLETE) {
		__be32 diff[] = { ~(*stack), *mpls_lse };
		skb->csum = ~csum_partial((char *)diff, sizeof(diff),
					  ~skb->csum);
	}

	*stack = *mpls_lse;
	key->mpls.top_lse = *mpls_lse;
	return 0;
}

static int pop_vlan(struct sk_buff *skb, struct sw_flow_key *key)
{
	int err;

	err = skb_vlan_pop(skb);
	if (vlan_tx_tag_present(skb))
		invalidate_flow_key(key);
	else
		key->eth.tci = 0;
	return err;
}

static int push_vlan(struct sk_buff *skb, struct sw_flow_key *key,
		     const struct ovs_action_push_vlan *vlan)
{
	if (vlan_tx_tag_present(skb))
		invalidate_flow_key(key);
	else
		key->eth.tci = vlan->vlan_tci;
	return skb_vlan_push(skb, vlan->vlan_tpid,
			     ntohs(vlan->vlan_tci) & ~VLAN_TAG_PRESENT);
}

static int set_eth_addr(struct sk_buff *skb, struct sw_flow_key *key,
			const struct ovs_key_ethernet *eth_key)
{
	int err;
	err = skb_ensure_writable(skb, ETH_HLEN);
	if (unlikely(err))
		return err;

	skb_postpull_rcsum(skb, eth_hdr(skb), ETH_ALEN * 2);

	ether_addr_copy(eth_hdr(skb)->h_source, eth_key->eth_src);
	ether_addr_copy(eth_hdr(skb)->h_dest, eth_key->eth_dst);

	ovs_skb_postpush_rcsum(skb, eth_hdr(skb), ETH_ALEN * 2);

	ether_addr_copy(key->eth.src, eth_key->eth_src);
	ether_addr_copy(key->eth.dst, eth_key->eth_dst);
	return 0;
}

static void set_ip_addr(struct sk_buff *skb, struct iphdr *nh,
			__be32 *addr, __be32 new_addr)
{
	int transport_len = skb->len - skb_transport_offset(skb);

	if (nh->protocol == IPPROTO_TCP) {
		if (likely(transport_len >= sizeof(struct tcphdr)))
			inet_proto_csum_replace4(&tcp_hdr(skb)->check, skb,
						 *addr, new_addr, true);
	} else if (nh->protocol == IPPROTO_UDP) {
		if (likely(transport_len >= sizeof(struct udphdr))) {
			struct udphdr *uh = udp_hdr(skb);

			if (uh->check || skb->ip_summed == CHECKSUM_PARTIAL) {
				inet_proto_csum_replace4(&uh->check, skb,
							 *addr, new_addr, true);
				if (!uh->check)
					uh->check = CSUM_MANGLED_0;
			}
		}
	}

	csum_replace4(&nh->check, *addr, new_addr);
	skb_clear_hash(skb);
	*addr = new_addr;
}

static void update_ipv6_checksum(struct sk_buff *skb, u8 l4_proto,
				 __be32 addr[4], const __be32 new_addr[4])
{
	int transport_len = skb->len - skb_transport_offset(skb);

	if (l4_proto == NEXTHDR_TCP) {
		if (likely(transport_len >= sizeof(struct tcphdr)))
			inet_proto_csum_replace16(&tcp_hdr(skb)->check, skb,
						  addr, new_addr, true);
	} else if (l4_proto == NEXTHDR_UDP) {
		if (likely(transport_len >= sizeof(struct udphdr))) {
			struct udphdr *uh = udp_hdr(skb);

			if (uh->check || skb->ip_summed == CHECKSUM_PARTIAL) {
				inet_proto_csum_replace16(&uh->check, skb,
							  addr, new_addr, true);
				if (!uh->check)
					uh->check = CSUM_MANGLED_0;
			}
		}
	} else if (l4_proto == NEXTHDR_ICMP) {
		if (likely(transport_len >= sizeof(struct icmp6hdr)))
			inet_proto_csum_replace16(&icmp6_hdr(skb)->icmp6_cksum,
						  skb, addr, new_addr, true);
	}
}

static void set_ipv6_addr(struct sk_buff *skb, u8 l4_proto,
			  __be32 addr[4], const __be32 new_addr[4],
			  bool recalculate_csum)
{
	if (recalculate_csum)
		update_ipv6_checksum(skb, l4_proto, addr, new_addr);

	skb_clear_hash(skb);
	memcpy(addr, new_addr, sizeof(__be32[4]));
}

static void set_ipv6_tc(struct ipv6hdr *nh, u8 tc)
{
	nh->priority = tc >> 4;
	nh->flow_lbl[0] = (nh->flow_lbl[0] & 0x0F) | ((tc & 0x0F) << 4);
}

static void set_ipv6_fl(struct ipv6hdr *nh, u32 fl)
{
	nh->flow_lbl[0] = (nh->flow_lbl[0] & 0xF0) | (fl & 0x000F0000) >> 16;
	nh->flow_lbl[1] = (fl & 0x0000FF00) >> 8;
	nh->flow_lbl[2] = fl & 0x000000FF;
}

static void set_ip_ttl(struct sk_buff *skb, struct iphdr *nh, u8 new_ttl)
{
	csum_replace2(&nh->check, htons(nh->ttl << 8), htons(new_ttl << 8));
	nh->ttl = new_ttl;
}

static int set_ipv4(struct sk_buff *skb, struct sw_flow_key *key,
		    const struct ovs_key_ipv4 *ipv4_key)
{
	struct iphdr *nh;
	int err;

	err = skb_ensure_writable(skb, skb_network_offset(skb) +
				  sizeof(struct iphdr));
	if (unlikely(err))
		return err;

	nh = ip_hdr(skb);

	if (ipv4_key->ipv4_src != nh->saddr) {
		set_ip_addr(skb, nh, &nh->saddr, ipv4_key->ipv4_src);
		key->ipv4.addr.src = ipv4_key->ipv4_src;
	}

	if (ipv4_key->ipv4_dst != nh->daddr) {
		set_ip_addr(skb, nh, &nh->daddr, ipv4_key->ipv4_dst);
		key->ipv4.addr.dst = ipv4_key->ipv4_dst;
	}

	if (ipv4_key->ipv4_tos != nh->tos) {
		ipv4_change_dsfield(nh, 0, ipv4_key->ipv4_tos);
		key->ip.tos = nh->tos;
	}

	if (ipv4_key->ipv4_ttl != nh->ttl) {
		set_ip_ttl(skb, nh, ipv4_key->ipv4_ttl);
		key->ip.ttl = ipv4_key->ipv4_ttl;
	}

	return 0;
}

static int set_ipv6(struct sk_buff *skb, struct sw_flow_key *key,
		    const struct ovs_key_ipv6 *ipv6_key)
{
	struct ipv6hdr *nh;
	int err;
	__be32 *saddr;
	__be32 *daddr;

	err = skb_ensure_writable(skb, skb_network_offset(skb) +
				  sizeof(struct ipv6hdr));
	if (unlikely(err))
		return err;

	nh = ipv6_hdr(skb);
	saddr = (__be32 *)&nh->saddr;
	daddr = (__be32 *)&nh->daddr;

	if (memcmp(ipv6_key->ipv6_src, saddr, sizeof(ipv6_key->ipv6_src))) {
		set_ipv6_addr(skb, ipv6_key->ipv6_proto, saddr,
			      ipv6_key->ipv6_src, true);
		memcpy(&key->ipv6.addr.src, ipv6_key->ipv6_src,
		       sizeof(ipv6_key->ipv6_src));
	}

	if (memcmp(ipv6_key->ipv6_dst, daddr, sizeof(ipv6_key->ipv6_dst))) {
		unsigned int offset = 0;
		int flags = IP6_FH_F_SKIP_RH;
		bool recalc_csum = true;

		if (ipv6_ext_hdr(nh->nexthdr))
			recalc_csum = ipv6_find_hdr(skb, &offset,
						    NEXTHDR_ROUTING, NULL,
						    &flags) != NEXTHDR_ROUTING;

		set_ipv6_addr(skb, ipv6_key->ipv6_proto, daddr,
			      ipv6_key->ipv6_dst, recalc_csum);
		memcpy(&key->ipv6.addr.dst, ipv6_key->ipv6_dst,
		       sizeof(ipv6_key->ipv6_dst));
	}

	set_ipv6_tc(nh, ipv6_key->ipv6_tclass);
	key->ip.tos = ipv6_get_dsfield(nh);

	set_ipv6_fl(nh, ntohl(ipv6_key->ipv6_label));
	key->ipv6.label = *(__be32 *)nh & htonl(IPV6_FLOWINFO_FLOWLABEL);

	nh->hop_limit = ipv6_key->ipv6_hlimit;
	key->ip.ttl = ipv6_key->ipv6_hlimit;
	return 0;
}

/* Must follow skb_ensure_writable() since that can move the skb data. */
static void set_tp_port(struct sk_buff *skb, __be16 *port,
			 __be16 new_port, __sum16 *check)
{
	inet_proto_csum_replace2(check, skb, *port, new_port, false);
	*port = new_port;
	skb_clear_hash(skb);
}

static void set_udp_port(struct sk_buff *skb, __be16 *port, __be16 new_port)
{
	struct udphdr *uh = udp_hdr(skb);

	if (uh->check && skb->ip_summed != CHECKSUM_PARTIAL) {
		set_tp_port(skb, port, new_port, &uh->check);

		if (!uh->check)
			uh->check = CSUM_MANGLED_0;
	} else {
		*port = new_port;
		skb_clear_hash(skb);
	}
}

static int set_udp(struct sk_buff *skb, struct sw_flow_key *key,
		   const struct ovs_key_udp *udp_port_key)
{
	struct udphdr *uh;
	int err;

	err = skb_ensure_writable(skb, skb_transport_offset(skb) +
				  sizeof(struct udphdr));
	if (unlikely(err))
		return err;

	uh = udp_hdr(skb);
	if (udp_port_key->udp_src != uh->source) {
		set_udp_port(skb, &uh->source, udp_port_key->udp_src);
		key->tp.src = udp_port_key->udp_src;
	}

	if (udp_port_key->udp_dst != uh->dest) {
		set_udp_port(skb, &uh->dest, udp_port_key->udp_dst);
		key->tp.dst = udp_port_key->udp_dst;
	}

	return 0;
}

static int set_tcp(struct sk_buff *skb, struct sw_flow_key *key,
		   const struct ovs_key_tcp *tcp_port_key)
{
	struct tcphdr *th;
	int err;

	err = skb_ensure_writable(skb, skb_transport_offset(skb) +
				  sizeof(struct tcphdr));
	if (unlikely(err))
		return err;

	th = tcp_hdr(skb);
	if (tcp_port_key->tcp_src != th->source) {
		set_tp_port(skb, &th->source, tcp_port_key->tcp_src, &th->check);
		key->tp.src = tcp_port_key->tcp_src;
	}

	if (tcp_port_key->tcp_dst != th->dest) {
		set_tp_port(skb, &th->dest, tcp_port_key->tcp_dst, &th->check);
		key->tp.dst = tcp_port_key->tcp_dst;
	}

	return 0;
}

static int set_sctp(struct sk_buff *skb, struct sw_flow_key *key,
		    const struct ovs_key_sctp *sctp_port_key)
{
	struct sctphdr *sh;
	int err;
	unsigned int sctphoff = skb_transport_offset(skb);

	err = skb_ensure_writable(skb, sctphoff + sizeof(struct sctphdr));
	if (unlikely(err))
		return err;

	sh = sctp_hdr(skb);
	if (sctp_port_key->sctp_src != sh->source ||
	    sctp_port_key->sctp_dst != sh->dest) {
		__le32 old_correct_csum, new_csum, old_csum;

		old_csum = sh->checksum;
		old_correct_csum = sctp_compute_cksum(skb, sctphoff);

		sh->source = sctp_port_key->sctp_src;
		sh->dest = sctp_port_key->sctp_dst;

		new_csum = sctp_compute_cksum(skb, sctphoff);

		/* Carry any checksum errors through. */
		sh->checksum = old_csum ^ old_correct_csum ^ new_csum;

		skb_clear_hash(skb);
		key->tp.src = sctp_port_key->sctp_src;
		key->tp.dst = sctp_port_key->sctp_dst;
	}

	return 0;
}

static void do_output(struct datapath *dp, struct sk_buff *skb, int out_port)
{
	struct vport *vport = ovs_vport_rcu(dp, out_port);

	if (likely(vport))
		ovs_vport_send(vport, skb);
	else
		kfree_skb(skb);
}

static int output_userspace(struct datapath *dp, struct sk_buff *skb,
			    struct sw_flow_key *key, const struct nlattr *attr)
{
	struct ovs_tunnel_info info;
	struct dp_upcall_info upcall;
	const struct nlattr *a;
	int rem;

	upcall.cmd = OVS_PACKET_CMD_ACTION;
	upcall.userdata = NULL;
	upcall.portid = 0;
	upcall.egress_tun_info = NULL;

	for (a = nla_data(attr), rem = nla_len(attr); rem > 0;
		 a = nla_next(a, &rem)) {
		switch (nla_type(a)) {
		case OVS_USERSPACE_ATTR_USERDATA:
			upcall.userdata = a;
			break;

		case OVS_USERSPACE_ATTR_PID:
			upcall.portid = nla_get_u32(a);
			break;

		case OVS_USERSPACE_ATTR_EGRESS_TUN_PORT: {
			/* Get out tunnel info. */
			struct vport *vport;

			vport = ovs_vport_rcu(dp, nla_get_u32(a));
			if (vport) {
				int err;

				err = ovs_vport_get_egress_tun_info(vport, skb,
								    &info);
				if (!err)
					upcall.egress_tun_info = &info;
			}
			break;
		}

		} /* End of switch. */
	}

	return ovs_dp_upcall(dp, skb, key, &upcall);
}

static int sample(struct datapath *dp, struct sk_buff *skb,
		  struct sw_flow_key *key, const struct nlattr *attr)
{
	const struct nlattr *acts_list = NULL;
	const struct nlattr *a;
	int rem;

	for (a = nla_data(attr), rem = nla_len(attr); rem > 0;
		 a = nla_next(a, &rem)) {
		switch (nla_type(a)) {
		case OVS_SAMPLE_ATTR_PROBABILITY:
			if (prandom_u32() >= nla_get_u32(a))
				return 0;
			break;

		case OVS_SAMPLE_ATTR_ACTIONS:
			acts_list = a;
			break;
		}
	}

	rem = nla_len(acts_list);
	a = nla_data(acts_list);

	/* Actions list is empty, do nothing */
	if (unlikely(!rem))
		return 0;

	/* The only known usage of sample action is having a single user-space
	 * action. Treat this usage as a special case.
	 * The output_userspace() should clone the skb to be sent to the
	 * user space. This skb will be consumed by its caller.
	 */
	if (likely(nla_type(a) == OVS_ACTION_ATTR_USERSPACE &&
		   nla_is_last(a, rem)))
		return output_userspace(dp, skb, key, a);

	skb = skb_clone(skb, GFP_ATOMIC);
	if (!skb)
		/* Skip the sample action when out of memory. */
		return 0;

	if (!add_deferred_actions(skb, key, a)) {
		if (net_ratelimit())
			pr_warn("%s: deferred actions limit reached, dropping sample action\n",
				ovs_dp_name(dp));

		kfree_skb(skb);
	}
	return 0;
}

static void execute_hash(struct sk_buff *skb, struct sw_flow_key *key,
			 const struct nlattr *attr)
{
	struct ovs_action_hash *hash_act = nla_data(attr);
	u32 hash = 0;

	/* OVS_HASH_ALG_L4 is the only possible hash algorithm.  */
	hash = skb_get_hash(skb);
	hash = jhash_1word(hash, hash_act->hash_basis);
	if (!hash)
		hash = 0x1;

	key->ovs_flow_hash = hash;
}

static int execute_set_action(struct sk_buff *skb, struct sw_flow_key *key,
			      const struct nlattr *nested_attr)
{
	int err = 0;

	switch (nla_type(nested_attr)) {
	case OVS_KEY_ATTR_PRIORITY:
		skb->priority = nla_get_u32(nested_attr);
		key->phy.priority = skb->priority;
		break;

	case OVS_KEY_ATTR_SKB_MARK:
		skb->mark = nla_get_u32(nested_attr);
		key->phy.skb_mark = skb->mark;
		break;

	case OVS_KEY_ATTR_TUNNEL_INFO:
		OVS_CB(skb)->egress_tun_info = nla_data(nested_attr);
		break;

	case OVS_KEY_ATTR_ETHERNET:
		err = set_eth_addr(skb, key, nla_data(nested_attr));
		break;

	case OVS_KEY_ATTR_IPV4:
		err = set_ipv4(skb, key, nla_data(nested_attr));
		break;

	case OVS_KEY_ATTR_IPV6:
		err = set_ipv6(skb, key, nla_data(nested_attr));
		break;

	case OVS_KEY_ATTR_TCP:
		err = set_tcp(skb, key, nla_data(nested_attr));
		break;

	case OVS_KEY_ATTR_UDP:
		err = set_udp(skb, key, nla_data(nested_attr));
		break;

	case OVS_KEY_ATTR_SCTP:
		err = set_sctp(skb, key, nla_data(nested_attr));
		break;

	case OVS_KEY_ATTR_MPLS:
		err = set_mpls(skb, key, nla_data(nested_attr));
		break;
	}

	return err;
}

static int execute_recirc(struct datapath *dp, struct sk_buff *skb,
			  struct sw_flow_key *key,
			  const struct nlattr *a, int rem)
{
	struct deferred_action *da;

	if (!is_flow_key_valid(key)) {
		int err;

		err = ovs_flow_key_update(skb, key);
		if (err)
			return err;
	}
	BUG_ON(!is_flow_key_valid(key));

	if (!nla_is_last(a, rem)) {
		/* Recirc action is the not the last action
		 * of the action list, need to clone the skb.
		 */
		skb = skb_clone(skb, GFP_ATOMIC);

		/* Skip the recirc action when out of memory, but
		 * continue on with the rest of the action list.
		 */
		if (!skb)
			return 0;
	}

	da = add_deferred_actions(skb, key, NULL);
	if (da) {
		da->pkt_key.recirc_id = nla_get_u32(a);
	} else {
		kfree_skb(skb);

		if (net_ratelimit())
			pr_warn("%s: deferred action limit reached, drop recirc action\n",
				ovs_dp_name(dp));
	}

	return 0;
}

/* Execute a list of actions against 'skb'. */
static int do_execute_actions(struct datapath *dp, struct sk_buff *skb,
			      struct sw_flow_key *key,
			      const struct nlattr *attr, int len)
{
	/* Every output action needs a separate clone of 'skb', but the common
	 * case is just a single output action, so that doing a clone and
	 * then freeing the original skbuff is wasteful.  So the following code
	 * is slightly obscure just to avoid that.
	 */
	int prev_port = -1;
	const struct nlattr *a;
	int rem;

	for (a = attr, rem = len; rem > 0;
	     a = nla_next(a, &rem)) {
		int err = 0;

		if (unlikely(prev_port != -1)) {
			struct sk_buff *out_skb = skb_clone(skb, GFP_ATOMIC);

			if (out_skb)
				do_output(dp, out_skb, prev_port);

			prev_port = -1;
		}

		switch (nla_type(a)) {
		case OVS_ACTION_ATTR_OUTPUT:
			prev_port = nla_get_u32(a);
			break;

		case OVS_ACTION_ATTR_USERSPACE:
			output_userspace(dp, skb, key, a);
			break;

		case OVS_ACTION_ATTR_HASH:
			execute_hash(skb, key, a);
			break;

		case OVS_ACTION_ATTR_PUSH_MPLS:
			err = push_mpls(skb, key, nla_data(a));
			break;

		case OVS_ACTION_ATTR_POP_MPLS:
			err = pop_mpls(skb, key, nla_get_be16(a));
			break;

		case OVS_ACTION_ATTR_PUSH_VLAN:
			err = push_vlan(skb, key, nla_data(a));
			break;

		case OVS_ACTION_ATTR_POP_VLAN:
			err = pop_vlan(skb, key);
			break;

		case OVS_ACTION_ATTR_RECIRC:
			err = execute_recirc(dp, skb, key, a, rem);
			if (nla_is_last(a, rem)) {
				/* If this is the last action, the skb has
				 * been consumed or freed.
				 * Return immediately.
				 */
				return err;
			}
			break;

		case OVS_ACTION_ATTR_SET:
			err = execute_set_action(skb, key, nla_data(a));
			break;

		case OVS_ACTION_ATTR_SAMPLE:
			err = sample(dp, skb, key, a);
			break;
		}

		if (unlikely(err)) {
			kfree_skb(skb);
			return err;
		}
	}

	if (prev_port != -1)
		do_output(dp, skb, prev_port);
	else
		consume_skb(skb);

	return 0;
}

static void process_deferred_actions(struct datapath *dp)
{
	struct action_fifo *fifo = this_cpu_ptr(action_fifos);

	/* Do not touch the FIFO in case there is no deferred actions. */
	if (action_fifo_is_empty(fifo))
		return;

	/* Finishing executing all deferred actions. */
	do {
		struct deferred_action *da = action_fifo_get(fifo);
		struct sk_buff *skb = da->skb;
		struct sw_flow_key *key = &da->pkt_key;
		const struct nlattr *actions = da->actions;

		if (actions)
			do_execute_actions(dp, skb, key, actions,
					   nla_len(actions));
		else
			ovs_dp_process_packet(skb, key);
	} while (!action_fifo_is_empty(fifo));

	/* Reset FIFO for the next packet.  */
	action_fifo_init(fifo);
}

/* Execute a list of actions against 'skb'. */
int ovs_execute_actions(struct datapath *dp, struct sk_buff *skb,
			const struct sw_flow_actions *acts,
			struct sw_flow_key *key)
{
	int level = this_cpu_read(exec_actions_level);
	int err;

	this_cpu_inc(exec_actions_level);
	OVS_CB(skb)->egress_tun_info = NULL;
	err = do_execute_actions(dp, skb, key,
				 acts->actions, acts->actions_len);

	if (!level)
		process_deferred_actions(dp);

	this_cpu_dec(exec_actions_level);
	return err;
}

int action_fifos_init(void)
{
	action_fifos = alloc_percpu(struct action_fifo);
	if (!action_fifos)
		return -ENOMEM;

	return 0;
}

void action_fifos_exit(void)
{
	free_percpu(action_fifos);
}
