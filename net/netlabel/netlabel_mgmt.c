/*
 * NetLabel Management Support
 *
 * This file defines the management functions for the NetLabel system.  The
 * NetLabel system manages static and dynamic label mappings for network
 * protocols such as CIPSO and RIPSO.
 *
 * Author: Paul Moore <paul@paul-moore.com>
 *
 */

/*
 * (c) Copyright Hewlett-Packard Development Company, L.P., 2006, 2008
 *
 * This program is free software;  you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY;  without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See
 * the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program;  if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
 *
 */

#include <linux/types.h>
#include <linux/socket.h>
#include <linux/string.h>
#include <linux/skbuff.h>
#include <linux/in.h>
#include <linux/in6.h>
#include <linux/slab.h>
#include <net/sock.h>
#include <net/netlink.h>
#include <net/genetlink.h>
#include <net/ip.h>
#include <net/ipv6.h>
#include <net/netlabel.h>
#include <net/cipso_ipv4.h>
#include <linux/atomic.h>

#include "netlabel_domainhash.h"
#include "netlabel_user.h"
#include "netlabel_mgmt.h"

/* NetLabel configured protocol counter */
atomic_t netlabel_mgmt_protocount = ATOMIC_INIT(0);

/* Argument struct for netlbl_domhsh_walk() */
struct netlbl_domhsh_walk_arg {
	struct netlink_callback *nl_cb;
	struct sk_buff *skb;
	u32 seq;
};

/* NetLabel Generic NETLINK CIPSOv4 family */
static struct genl_family netlbl_mgmt_gnl_family = {
	.id = GENL_ID_GENERATE,
	.hdrsize = 0,
	.name = NETLBL_NLTYPE_MGMT_NAME,
	.version = NETLBL_PROTO_VERSION,
	.maxattr = NLBL_MGMT_A_MAX,
};

/* NetLabel Netlink attribute policy */
static const struct nla_policy netlbl_mgmt_genl_policy[NLBL_MGMT_A_MAX + 1] = {
	[NLBL_MGMT_A_DOMAIN] = { .type = NLA_NUL_STRING },
	[NLBL_MGMT_A_PROTOCOL] = { .type = NLA_U32 },
	[NLBL_MGMT_A_VERSION] = { .type = NLA_U32 },
	[NLBL_MGMT_A_CV4DOI] = { .type = NLA_U32 },
};

/*
 * Helper Functions
 */

/**
 * netlbl_mgmt_add - Handle an ADD message
 * @info: the Generic NETLINK info block
 * @audit_info: NetLabel audit information
 *
 * Description:
 * Helper function for the ADD and ADDDEF messages to add the domain mappings
 * from the message to the hash table.  See netlabel.h for a description of the
 * message format.  Returns zero on success, negative values on failure.
 *
 */
static int netlbl_mgmt_add_common(struct genl_info *info,
				  struct netlbl_audit *audit_info)
{
	void *pmap = NULL;
	int ret_val = -EINVAL;
	struct netlbl_domaddr_map *addrmap = NULL;
	struct cipso_v4_doi *cipsov4 = NULL;
	u32 tmp_val;
	struct netlbl_dom_map *entry = kzalloc(sizeof(*entry), GFP_KERNEL);

	if (!entry)
		return -ENOMEM;
	entry->type = nla_get_u32(info->attrs[NLBL_MGMT_A_PROTOCOL]);
	if (info->attrs[NLBL_MGMT_A_DOMAIN]) {
		size_t tmp_size = nla_len(info->attrs[NLBL_MGMT_A_DOMAIN]);
		entry->domain = kmalloc(tmp_size, GFP_KERNEL);
		if (entry->domain == NULL) {
			ret_val = -ENOMEM;
			goto add_free_entry;
		}
		nla_strlcpy(entry->domain,
			    info->attrs[NLBL_MGMT_A_DOMAIN], tmp_size);
	}

	/* NOTE: internally we allow/use a entry->type value of
	 *       NETLBL_NLTYPE_ADDRSELECT but we don't currently allow users
	 *       to pass that as a protocol value because we need to know the
	 *       "real" protocol */

	switch (entry->type) {
	case NETLBL_NLTYPE_UNLABELED:
		break;
	case NETLBL_NLTYPE_CIPSOV4:
		if (!info->attrs[NLBL_MGMT_A_CV4DOI])
			goto add_free_domain;

		tmp_val = nla_get_u32(info->attrs[NLBL_MGMT_A_CV4DOI]);
		cipsov4 = cipso_v4_doi_getdef(tmp_val);
		if (cipsov4 == NULL)
			goto add_free_domain;
		entry->type_def.cipsov4 = cipsov4;
		break;
	default:
		goto add_free_domain;
	}

	if (info->attrs[NLBL_MGMT_A_IPV4ADDR]) {
		struct in_addr *addr;
		struct in_addr *mask;
		struct netlbl_domaddr4_map *map;

		addrmap = kzalloc(sizeof(*addrmap), GFP_KERNEL);
		if (addrmap == NULL) {
			ret_val = -ENOMEM;
			goto add_doi_put_def;
		}
		INIT_LIST_HEAD(&addrmap->list4);
		INIT_LIST_HEAD(&addrmap->list6);

		if (nla_len(info->attrs[NLBL_MGMT_A_IPV4ADDR]) !=
		    sizeof(struct in_addr)) {
			ret_val = -EINVAL;
			goto add_free_addrmap;
		}
		if (nla_len(info->attrs[NLBL_MGMT_A_IPV4MASK]) !=
		    sizeof(struct in_addr)) {
			ret_val = -EINVAL;
			goto add_free_addrmap;
		}
		addr = nla_data(info->attrs[NLBL_MGMT_A_IPV4ADDR]);
		mask = nla_data(info->attrs[NLBL_MGMT_A_IPV4MASK]);

		map = kzalloc(sizeof(*map), GFP_KERNEL);
		if (map == NULL) {
			ret_val = -ENOMEM;
			goto add_free_addrmap;
		}
		pmap = map;
		map->list.addr = addr->s_addr & mask->s_addr;
		map->list.mask = mask->s_addr;
		map->list.valid = 1;
		map->type = entry->type;
		if (cipsov4)
			map->type_def.cipsov4 = cipsov4;

		ret_val = netlbl_af4list_add(&map->list, &addrmap->list4);
		if (ret_val != 0)
			goto add_free_map;

		entry->type = NETLBL_NLTYPE_ADDRSELECT;
		entry->type_def.addrsel = addrmap;
#if IS_ENABLED(CONFIG_IPV6)
	} else if (info->attrs[NLBL_MGMT_A_IPV6ADDR]) {
		struct in6_addr *addr;
		struct in6_addr *mask;
		struct netlbl_domaddr6_map *map;

		addrmap = kzalloc(sizeof(*addrmap), GFP_KERNEL);
		if (addrmap == NULL) {
			ret_val = -ENOMEM;
			goto add_doi_put_def;
		}
		INIT_LIST_HEAD(&addrmap->list4);
		INIT_LIST_HEAD(&addrmap->list6);

		if (nla_len(info->attrs[NLBL_MGMT_A_IPV6ADDR]) !=
		    sizeof(struct in6_addr)) {
			ret_val = -EINVAL;
			goto add_free_addrmap;
		}
		if (nla_len(info->attrs[NLBL_MGMT_A_IPV6MASK]) !=
		    sizeof(struct in6_addr)) {
			ret_val = -EINVAL;
			goto add_free_addrmap;
		}
		addr = nla_data(info->attrs[NLBL_MGMT_A_IPV6ADDR]);
		mask = nla_data(info->attrs[NLBL_MGMT_A_IPV6MASK]);

		map = kzalloc(sizeof(*map), GFP_KERNEL);
		if (map == NULL) {
			ret_val = -ENOMEM;
			goto add_free_addrmap;
		}
		pmap = map;
		map->list.addr = *addr;
		map->list.addr.s6_addr32[0] &= mask->s6_addr32[0];
		map->list.addr.s6_addr32[1] &= mask->s6_addr32[1];
		map->list.addr.s6_addr32[2] &= mask->s6_addr32[2];
		map->list.addr.s6_addr32[3] &= mask->s6_addr32[3];
		map->list.mask = *mask;
		map->list.valid = 1;
		map->type = entry->type;

		ret_val = netlbl_af6list_add(&map->list, &addrmap->list6);
		if (ret_val != 0)
			goto add_free_map;

		entry->type = NETLBL_NLTYPE_ADDRSELECT;
		entry->type_def.addrsel = addrmap;
#endif /* IPv6 */
	}

	ret_val = netlbl_domhsh_add(entry, audit_info);
	if (ret_val != 0)
		goto add_free_map;

	return 0;

add_free_map:
	kfree(pmap);
add_free_addrmap:
	kfree(addrmap);
add_doi_put_def:
	cipso_v4_doi_putdef(cipsov4);
add_free_domain:
	kfree(entry->domain);
add_free_entry:
	kfree(entry);
	return ret_val;
}

/**
 * netlbl_mgmt_listentry - List a NetLabel/LSM domain map entry
 * @skb: the NETLINK buffer
 * @entry: the map entry
 *
 * Description:
 * This function is a helper function used by the LISTALL and LISTDEF command
 * handlers.  The caller is responsible for ensuring that the RCU read lock
 * is held.  Returns zero on success, negative values on failure.
 *
 */
static int netlbl_mgmt_listentry(struct sk_buff *skb,
				 struct netlbl_dom_map *entry)
{
	int ret_val = 0;
	struct nlattr *nla_a;
	struct nlattr *nla_b;
	struct netlbl_af4list *iter4;
#if IS_ENABLED(CONFIG_IPV6)
	struct netlbl_af6list *iter6;
#endif

	if (entry->domain != NULL) {
		ret_val = nla_put_string(skb,
					 NLBL_MGMT_A_DOMAIN, entry->domain);
		if (ret_val != 0)
			return ret_val;
	}

	switch (entry->type) {
	case NETLBL_NLTYPE_ADDRSELECT:
		nla_a = nla_nest_start(skb, NLBL_MGMT_A_SELECTORLIST);
		if (nla_a == NULL)
			return -ENOMEM;

		netlbl_af4list_foreach_rcu(iter4,
					   &entry->type_def.addrsel->list4) {
			struct netlbl_domaddr4_map *map4;
			struct in_addr addr_struct;

			nla_b = nla_nest_start(skb, NLBL_MGMT_A_ADDRSELECTOR);
			if (nla_b == NULL)
				return -ENOMEM;

			addr_struct.s_addr = iter4->addr;
			ret_val = nla_put(skb, NLBL_MGMT_A_IPV4ADDR,
					  sizeof(struct in_addr),
					  &addr_struct);
			if (ret_val != 0)
				return ret_val;
			addr_struct.s_addr = iter4->mask;
			ret_val = nla_put(skb, NLBL_MGMT_A_IPV4MASK,
					  sizeof(struct in_addr),
					  &addr_struct);
			if (ret_val != 0)
				return ret_val;
			map4 = netlbl_domhsh_addr4_entry(iter4);
			ret_val = nla_put_u32(skb, NLBL_MGMT_A_PROTOCOL,
					      map4->type);
			if (ret_val != 0)
				return ret_val;
			switch (map4->type) {
			case NETLBL_NLTYPE_CIPSOV4:
				ret_val = nla_put_u32(skb, NLBL_MGMT_A_CV4DOI,
						  map4->type_def.cipsov4->doi);
				if (ret_val != 0)
					return ret_val;
				break;
			}

			nla_nest_end(skb, nla_b);
		}
#if IS_ENABLED(CONFIG_IPV6)
		netlbl_af6list_foreach_rcu(iter6,
					   &entry->type_def.addrsel->list6) {
			struct netlbl_domaddr6_map *map6;

			nla_b = nla_nest_start(skb, NLBL_MGMT_A_ADDRSELECTOR);
			if (nla_b == NULL)
				return -ENOMEM;

			ret_val = nla_put(skb, NLBL_MGMT_A_IPV6ADDR,
					  sizeof(struct in6_addr),
					  &iter6->addr);
			if (ret_val != 0)
				return ret_val;
			ret_val = nla_put(skb, NLBL_MGMT_A_IPV6MASK,
					  sizeof(struct in6_addr),
					  &iter6->mask);
			if (ret_val != 0)
				return ret_val;
			map6 = netlbl_domhsh_addr6_entry(iter6);
			ret_val = nla_put_u32(skb, NLBL_MGMT_A_PROTOCOL,
					      map6->type);
			if (ret_val != 0)
				return ret_val;

			nla_nest_end(skb, nla_b);
		}
#endif /* IPv6 */

		nla_nest_end(skb, nla_a);
		break;
	case NETLBL_NLTYPE_UNLABELED:
		ret_val = nla_put_u32(skb, NLBL_MGMT_A_PROTOCOL, entry->type);
		break;
	case NETLBL_NLTYPE_CIPSOV4:
		ret_val = nla_put_u32(skb, NLBL_MGMT_A_PROTOCOL, entry->type);
		if (ret_val != 0)
			return ret_val;
		ret_val = nla_put_u32(skb, NLBL_MGMT_A_CV4DOI,
				      entry->type_def.cipsov4->doi);
		break;
	}

	return ret_val;
}

/*
 * NetLabel Command Handlers
 */

/**
 * netlbl_mgmt_add - Handle an ADD message
 * @skb: the NETLINK buffer
 * @info: the Generic NETLINK info block
 *
 * Description:
 * Process a user generated ADD message and add the domains from the message
 * to the hash table.  See netlabel.h for a description of the message format.
 * Returns zero on success, negative values on failure.
 *
 */
static int netlbl_mgmt_add(struct sk_buff *skb, struct genl_info *info)
{
	struct netlbl_audit audit_info;

	if ((!info->attrs[NLBL_MGMT_A_DOMAIN]) ||
	    (!info->attrs[NLBL_MGMT_A_PROTOCOL]) ||
	    (info->attrs[NLBL_MGMT_A_IPV4ADDR] &&
	     info->attrs[NLBL_MGMT_A_IPV6ADDR]) ||
	    (info->attrs[NLBL_MGMT_A_IPV4MASK] &&
	     info->attrs[NLBL_MGMT_A_IPV6MASK]) ||
	    ((info->attrs[NLBL_MGMT_A_IPV4ADDR] != NULL) ^
	     (info->attrs[NLBL_MGMT_A_IPV4MASK] != NULL)) ||
	    ((info->attrs[NLBL_MGMT_A_IPV6ADDR] != NULL) ^
	     (info->attrs[NLBL_MGMT_A_IPV6MASK] != NULL)))
		return -EINVAL;

	netlbl_netlink_auditinfo(skb, &audit_info);

	return netlbl_mgmt_add_common(info, &audit_info);
}

/**
 * netlbl_mgmt_remove - Handle a REMOVE message
 * @skb: the NETLINK buffer
 * @info: the Generic NETLINK info block
 *
 * Description:
 * Process a user generated REMOVE message and remove the specified domain
 * mappings.  Returns zero on success, negative values on failure.
 *
 */
static int netlbl_mgmt_remove(struct sk_buff *skb, struct genl_info *info)
{
	char *domain;
	struct netlbl_audit audit_info;

	if (!info->attrs[NLBL_MGMT_A_DOMAIN])
		return -EINVAL;

	netlbl_netlink_auditinfo(skb, &audit_info);

	domain = nla_data(info->attrs[NLBL_MGMT_A_DOMAIN]);
	return netlbl_domhsh_remove(domain, &audit_info);
}

/**
 * netlbl_mgmt_listall_cb - netlbl_domhsh_walk() callback for LISTALL
 * @entry: the domain mapping hash table entry
 * @arg: the netlbl_domhsh_walk_arg structure
 *
 * Description:
 * This function is designed to be used as a callback to the
 * netlbl_domhsh_walk() function for use in generating a response for a LISTALL
 * message.  Returns the size of the message on success, negative values on
 * failure.
 *
 */
static int netlbl_mgmt_listall_cb(struct netlbl_dom_map *entry, void *arg)
{
	int ret_val = -ENOMEM;
	struct netlbl_domhsh_walk_arg *cb_arg = arg;
	void *data;

	data = genlmsg_put(cb_arg->skb, NETLINK_CB(cb_arg->nl_cb->skb).portid,
			   cb_arg->seq, &netlbl_mgmt_gnl_family,
			   NLM_F_MULTI, NLBL_MGMT_C_LISTALL);
	if (data == NULL)
		goto listall_cb_failure;

	ret_val = netlbl_mgmt_listentry(cb_arg->skb, entry);
	if (ret_val != 0)
		goto listall_cb_failure;

	cb_arg->seq++;
	return genlmsg_end(cb_arg->skb, data);

listall_cb_failure:
	genlmsg_cancel(cb_arg->skb, data);
	return ret_val;
}

/**
 * netlbl_mgmt_listall - Handle a LISTALL message
 * @skb: the NETLINK buffer
 * @cb: the NETLINK callback
 *
 * Description:
 * Process a user generated LISTALL message and dumps the domain hash table in
 * a form suitable for use in a kernel generated LISTALL message.  Returns zero
 * on success, negative values on failure.
 *
 */
static int netlbl_mgmt_listall(struct sk_buff *skb,
			       struct netlink_callback *cb)
{
	struct netlbl_domhsh_walk_arg cb_arg;
	u32 skip_bkt = cb->args[0];
	u32 skip_chain = cb->args[1];

	cb_arg.nl_cb = cb;
	cb_arg.skb = skb;
	cb_arg.seq = cb->nlh->nlmsg_seq;

	netlbl_domhsh_walk(&skip_bkt,
			   &skip_chain,
			   netlbl_mgmt_listall_cb,
			   &cb_arg);

	cb->args[0] = skip_bkt;
	cb->args[1] = skip_chain;
	return skb->len;
}

/**
 * netlbl_mgmt_adddef - Handle an ADDDEF message
 * @skb: the NETLINK buffer
 * @info: the Generic NETLINK info block
 *
 * Description:
 * Process a user generated ADDDEF message and respond accordingly.  Returns
 * zero on success, negative values on failure.
 *
 */
static int netlbl_mgmt_adddef(struct sk_buff *skb, struct genl_info *info)
{
	struct netlbl_audit audit_info;

	if ((!info->attrs[NLBL_MGMT_A_PROTOCOL]) ||
	    (info->attrs[NLBL_MGMT_A_IPV4ADDR] &&
	     info->attrs[NLBL_MGMT_A_IPV6ADDR]) ||
	    (info->attrs[NLBL_MGMT_A_IPV4MASK] &&
	     info->attrs[NLBL_MGMT_A_IPV6MASK]) ||
	    ((info->attrs[NLBL_MGMT_A_IPV4ADDR] != NULL) ^
	     (info->attrs[NLBL_MGMT_A_IPV4MASK] != NULL)) ||
	    ((info->attrs[NLBL_MGMT_A_IPV6ADDR] != NULL) ^
	     (info->attrs[NLBL_MGMT_A_IPV6MASK] != NULL)))
		return -EINVAL;

	netlbl_netlink_auditinfo(skb, &audit_info);

	return netlbl_mgmt_add_common(info, &audit_info);
}

/**
 * netlbl_mgmt_removedef - Handle a REMOVEDEF message
 * @skb: the NETLINK buffer
 * @info: the Generic NETLINK info block
 *
 * Description:
 * Process a user generated REMOVEDEF message and remove the default domain
 * mapping.  Returns zero on success, negative values on failure.
 *
 */
static int netlbl_mgmt_removedef(struct sk_buff *skb, struct genl_info *info)
{
	struct netlbl_audit audit_info;

	netlbl_netlink_auditinfo(skb, &audit_info);

	return netlbl_domhsh_remove_default(&audit_info);
}

/**
 * netlbl_mgmt_listdef - Handle a LISTDEF message
 * @skb: the NETLINK buffer
 * @info: the Generic NETLINK info block
 *
 * Description:
 * Process a user generated LISTDEF message and dumps the default domain
 * mapping in a form suitable for use in a kernel generated LISTDEF message.
 * Returns zero on success, negative values on failure.
 *
 */
static int netlbl_mgmt_listdef(struct sk_buff *skb, struct genl_info *info)
{
	int ret_val = -ENOMEM;
	struct sk_buff *ans_skb = NULL;
	void *data;
	struct netlbl_dom_map *entry;

	ans_skb = nlmsg_new(NLMSG_DEFAULT_SIZE, GFP_KERNEL);
	if (ans_skb == NULL)
		return -ENOMEM;
	data = genlmsg_put_reply(ans_skb, info, &netlbl_mgmt_gnl_family,
				 0, NLBL_MGMT_C_LISTDEF);
	if (data == NULL)
		goto listdef_failure;

	rcu_read_lock();
	entry = netlbl_domhsh_getentry(NULL);
	if (entry == NULL) {
		ret_val = -ENOENT;
		goto listdef_failure_lock;
	}
	ret_val = netlbl_mgmt_listentry(ans_skb, entry);
	rcu_read_unlock();
	if (ret_val != 0)
		goto listdef_failure;

	genlmsg_end(ans_skb, data);
	return genlmsg_reply(ans_skb, info);

listdef_failure_lock:
	rcu_read_unlock();
listdef_failure:
	kfree_skb(ans_skb);
	return ret_val;
}

/**
 * netlbl_mgmt_protocols_cb - Write an individual PROTOCOL message response
 * @skb: the skb to write to
 * @cb: the NETLINK callback
 * @protocol: the NetLabel protocol to use in the message
 *
 * Description:
 * This function is to be used in conjunction with netlbl_mgmt_protocols() to
 * answer a application's PROTOCOLS message.  Returns the size of the message
 * on success, negative values on failure.
 *
 */
static int netlbl_mgmt_protocols_cb(struct sk_buff *skb,
				    struct netlink_callback *cb,
				    u32 protocol)
{
	int ret_val = -ENOMEM;
	void *data;

	data = genlmsg_put(skb, NETLINK_CB(cb->skb).portid, cb->nlh->nlmsg_seq,
			   &netlbl_mgmt_gnl_family, NLM_F_MULTI,
			   NLBL_MGMT_C_PROTOCOLS);
	if (data == NULL)
		goto protocols_cb_failure;

	ret_val = nla_put_u32(skb, NLBL_MGMT_A_PROTOCOL, protocol);
	if (ret_val != 0)
		goto protocols_cb_failure;

	return genlmsg_end(skb, data);

protocols_cb_failure:
	genlmsg_cancel(skb, data);
	return ret_val;
}

/**
 * netlbl_mgmt_protocols - Handle a PROTOCOLS message
 * @skb: the NETLINK buffer
 * @cb: the NETLINK callback
 *
 * Description:
 * Process a user generated PROTOCOLS message and respond accordingly.
 *
 */
static int netlbl_mgmt_protocols(struct sk_buff *skb,
				 struct netlink_callback *cb)
{
	u32 protos_sent = cb->args[0];

	if (protos_sent == 0) {
		if (netlbl_mgmt_protocols_cb(skb,
					     cb,
					     NETLBL_NLTYPE_UNLABELED) < 0)
			goto protocols_return;
		protos_sent++;
	}
	if (protos_sent == 1) {
		if (netlbl_mgmt_protocols_cb(skb,
					     cb,
					     NETLBL_NLTYPE_CIPSOV4) < 0)
			goto protocols_return;
		protos_sent++;
	}

protocols_return:
	cb->args[0] = protos_sent;
	return skb->len;
}

/**
 * netlbl_mgmt_version - Handle a VERSION message
 * @skb: the NETLINK buffer
 * @info: the Generic NETLINK info block
 *
 * Description:
 * Process a user generated VERSION message and respond accordingly.  Returns
 * zero on success, negative values on failure.
 *
 */
static int netlbl_mgmt_version(struct sk_buff *skb, struct genl_info *info)
{
	int ret_val = -ENOMEM;
	struct sk_buff *ans_skb = NULL;
	void *data;

	ans_skb = nlmsg_new(NLMSG_DEFAULT_SIZE, GFP_KERNEL);
	if (ans_skb == NULL)
		return -ENOMEM;
	data = genlmsg_put_reply(ans_skb, info, &netlbl_mgmt_gnl_family,
				 0, NLBL_MGMT_C_VERSION);
	if (data == NULL)
		goto version_failure;

	ret_val = nla_put_u32(ans_skb,
			      NLBL_MGMT_A_VERSION,
			      NETLBL_PROTO_VERSION);
	if (ret_val != 0)
		goto version_failure;

	genlmsg_end(ans_skb, data);
	return genlmsg_reply(ans_skb, info);

version_failure:
	kfree_skb(ans_skb);
	return ret_val;
}


/*
 * NetLabel Generic NETLINK Command Definitions
 */

static struct genl_ops netlbl_mgmt_genl_ops[] = {
	{
	.cmd = NLBL_MGMT_C_ADD,
	.flags = GENL_ADMIN_PERM,
	.policy = netlbl_mgmt_genl_policy,
	.doit = netlbl_mgmt_add,
	.dumpit = NULL,
	},
	{
	.cmd = NLBL_MGMT_C_REMOVE,
	.flags = GENL_ADMIN_PERM,
	.policy = netlbl_mgmt_genl_policy,
	.doit = netlbl_mgmt_remove,
	.dumpit = NULL,
	},
	{
	.cmd = NLBL_MGMT_C_LISTALL,
	.flags = 0,
	.policy = netlbl_mgmt_genl_policy,
	.doit = NULL,
	.dumpit = netlbl_mgmt_listall,
	},
	{
	.cmd = NLBL_MGMT_C_ADDDEF,
	.flags = GENL_ADMIN_PERM,
	.policy = netlbl_mgmt_genl_policy,
	.doit = netlbl_mgmt_adddef,
	.dumpit = NULL,
	},
	{
	.cmd = NLBL_MGMT_C_REMOVEDEF,
	.flags = GENL_ADMIN_PERM,
	.policy = netlbl_mgmt_genl_policy,
	.doit = netlbl_mgmt_removedef,
	.dumpit = NULL,
	},
	{
	.cmd = NLBL_MGMT_C_LISTDEF,
	.flags = 0,
	.policy = netlbl_mgmt_genl_policy,
	.doit = netlbl_mgmt_listdef,
	.dumpit = NULL,
	},
	{
	.cmd = NLBL_MGMT_C_PROTOCOLS,
	.flags = 0,
	.policy = netlbl_mgmt_genl_policy,
	.doit = NULL,
	.dumpit = netlbl_mgmt_protocols,
	},
	{
	.cmd = NLBL_MGMT_C_VERSION,
	.flags = 0,
	.policy = netlbl_mgmt_genl_policy,
	.doit = netlbl_mgmt_version,
	.dumpit = NULL,
	},
};

/*
 * NetLabel Generic NETLINK Protocol Functions
 */

/**
 * netlbl_mgmt_genl_init - Register the NetLabel management component
 *
 * Description:
 * Register the NetLabel management component with the Generic NETLINK
 * mechanism.  Returns zero on success, negative values on failure.
 *
 */
int __init netlbl_mgmt_genl_init(void)
{
	return genl_register_family_with_ops(&netlbl_mgmt_gnl_family,
		netlbl_mgmt_genl_ops, ARRAY_SIZE(netlbl_mgmt_genl_ops));
}
