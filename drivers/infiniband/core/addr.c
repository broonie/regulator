/*
 * Copyright (c) 2005 Voltaire Inc.  All rights reserved.
 * Copyright (c) 2002-2005, Network Appliance, Inc. All rights reserved.
 * Copyright (c) 1999-2005, Mellanox Technologies, Inc. All rights reserved.
 * Copyright (c) 2005 Intel Corporation.  All rights reserved.
 *
 * This software is available to you under a choice of one of two
 * licenses.  You may choose to be licensed under the terms of the GNU
 * General Public License (GPL) Version 2, available from the file
 * COPYING in the main directory of this source tree, or the
 * OpenIB.org BSD license below:
 *
 *     Redistribution and use in source and binary forms, with or
 *     without modification, are permitted provided that the following
 *     conditions are met:
 *
 *      - Redistributions of source code must retain the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer.
 *
 *      - Redistributions in binary form must reproduce the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer in the documentation and/or other materials
 *        provided with the distribution.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include <linux/mutex.h>
#include <linux/inetdevice.h>
#include <linux/workqueue.h>
#include <linux/if_arp.h>
#include <net/arp.h>
#include <net/neighbour.h>
#include <net/route.h>
#include <net/netevent.h>
#include <net/addrconf.h>
#include <net/ip6_route.h>
#include <rdma/ib_addr.h>

MODULE_AUTHOR("Sean Hefty");
MODULE_DESCRIPTION("IB Address Translation");
MODULE_LICENSE("Dual BSD/GPL");

struct addr_req {
	struct list_head list;
	struct sockaddr_storage src_addr;
	struct sockaddr_storage dst_addr;
	struct rdma_dev_addr *addr;
	struct rdma_addr_client *client;
	void *context;
	void (*callback)(int status, struct sockaddr *src_addr,
			 struct rdma_dev_addr *addr, void *context);
	unsigned long timeout;
	int status;
};

static void process_req(struct work_struct *work);

static DEFINE_MUTEX(lock);
static LIST_HEAD(req_list);
static DECLARE_DELAYED_WORK(work, process_req);
static struct workqueue_struct *addr_wq;

void rdma_addr_register_client(struct rdma_addr_client *client)
{
	atomic_set(&client->refcount, 1);
	init_completion(&client->comp);
}
EXPORT_SYMBOL(rdma_addr_register_client);

static inline void put_client(struct rdma_addr_client *client)
{
	if (atomic_dec_and_test(&client->refcount))
		complete(&client->comp);
}

void rdma_addr_unregister_client(struct rdma_addr_client *client)
{
	put_client(client);
	wait_for_completion(&client->comp);
}
EXPORT_SYMBOL(rdma_addr_unregister_client);

int rdma_copy_addr(struct rdma_dev_addr *dev_addr, struct net_device *dev,
		     const unsigned char *dst_dev_addr)
{
	switch (dev->type) {
	case ARPHRD_INFINIBAND:
		dev_addr->dev_type = RDMA_NODE_IB_CA;
		break;
	case ARPHRD_ETHER:
		dev_addr->dev_type = RDMA_NODE_RNIC;
		break;
	default:
		return -EADDRNOTAVAIL;
	}

	memcpy(dev_addr->src_dev_addr, dev->dev_addr, MAX_ADDR_LEN);
	memcpy(dev_addr->broadcast, dev->broadcast, MAX_ADDR_LEN);
	if (dst_dev_addr)
		memcpy(dev_addr->dst_dev_addr, dst_dev_addr, MAX_ADDR_LEN);
	dev_addr->src_dev = dev;
	return 0;
}
EXPORT_SYMBOL(rdma_copy_addr);

int rdma_translate_ip(struct sockaddr *addr, struct rdma_dev_addr *dev_addr)
{
	struct net_device *dev;
	int ret = -EADDRNOTAVAIL;

	switch (addr->sa_family) {
	case AF_INET:
		dev = ip_dev_find(&init_net,
			((struct sockaddr_in *) addr)->sin_addr.s_addr);

		if (!dev)
			return ret;

		ret = rdma_copy_addr(dev_addr, dev, NULL);
		dev_put(dev);
		break;
	case AF_INET6:
		for_each_netdev(&init_net, dev) {
			if (ipv6_chk_addr(&init_net,
					  &((struct sockaddr_in6 *) addr)->sin6_addr,
					  dev, 1)) {
				ret = rdma_copy_addr(dev_addr, dev, NULL);
				break;
			}
		}
		break;
	default:
		break;
	}
	return ret;
}
EXPORT_SYMBOL(rdma_translate_ip);

static void set_timeout(unsigned long time)
{
	unsigned long delay;

	cancel_delayed_work(&work);

	delay = time - jiffies;
	if ((long)delay <= 0)
		delay = 1;

	queue_delayed_work(addr_wq, &work, delay);
}

static void queue_req(struct addr_req *req)
{
	struct addr_req *temp_req;

	mutex_lock(&lock);
	list_for_each_entry_reverse(temp_req, &req_list, list) {
		if (time_after_eq(req->timeout, temp_req->timeout))
			break;
	}

	list_add(&req->list, &temp_req->list);

	if (req_list.next == &req->list)
		set_timeout(req->timeout);
	mutex_unlock(&lock);
}

static void addr_send_arp(struct sockaddr *dst_in)
{
	struct rtable *rt;
	struct flowi fl;
	struct dst_entry *dst;

	memset(&fl, 0, sizeof fl);
	if (dst_in->sa_family == AF_INET)  {
		fl.nl_u.ip4_u.daddr =
			((struct sockaddr_in *) dst_in)->sin_addr.s_addr;

		if (ip_route_output_key(&init_net, &rt, &fl))
			return;

		neigh_event_send(rt->u.dst.neighbour, NULL);
		ip_rt_put(rt);

	} else {
		fl.nl_u.ip6_u.daddr =
			((struct sockaddr_in6 *) dst_in)->sin6_addr;

		dst = ip6_route_output(&init_net, NULL, &fl);
		if (!dst)
			return;

		neigh_event_send(dst->neighbour, NULL);
		dst_release(dst);
	}
}

static int addr4_resolve_remote(struct sockaddr_in *src_in,
			       struct sockaddr_in *dst_in,
			       struct rdma_dev_addr *addr)
{
	__be32 src_ip = src_in->sin_addr.s_addr;
	__be32 dst_ip = dst_in->sin_addr.s_addr;
	struct flowi fl;
	struct rtable *rt;
	struct neighbour *neigh;
	int ret;

	memset(&fl, 0, sizeof fl);
	fl.nl_u.ip4_u.daddr = dst_ip;
	fl.nl_u.ip4_u.saddr = src_ip;
	ret = ip_route_output_key(&init_net, &rt, &fl);
	if (ret)
		goto out;

	/* If the device does ARP internally, return 'done' */
	if (rt->idev->dev->flags & IFF_NOARP) {
		rdma_copy_addr(addr, rt->idev->dev, NULL);
		goto put;
	}

	neigh = neigh_lookup(&arp_tbl, &rt->rt_gateway, rt->idev->dev);
	if (!neigh) {
		ret = -ENODATA;
		goto put;
	}

	if (!(neigh->nud_state & NUD_VALID)) {
		ret = -ENODATA;
		goto release;
	}

	if (!src_ip) {
		src_in->sin_family = dst_in->sin_family;
		src_in->sin_addr.s_addr = rt->rt_src;
	}

	ret = rdma_copy_addr(addr, neigh->dev, neigh->ha);
release:
	neigh_release(neigh);
put:
	ip_rt_put(rt);
out:
	return ret;
}

static int addr6_resolve_remote(struct sockaddr_in6 *src_in,
			       struct sockaddr_in6 *dst_in,
			       struct rdma_dev_addr *addr)
{
	struct flowi fl;
	struct neighbour *neigh;
	struct dst_entry *dst;
	int ret = -ENODATA;

	memset(&fl, 0, sizeof fl);
	fl.nl_u.ip6_u.daddr = dst_in->sin6_addr;
	fl.nl_u.ip6_u.saddr = src_in->sin6_addr;

	dst = ip6_route_output(&init_net, NULL, &fl);
	if (!dst)
		return ret;

	if (dst->dev->flags & IFF_NOARP) {
		ret = rdma_copy_addr(addr, dst->dev, NULL);
	} else {
		neigh = dst->neighbour;
		if (neigh && (neigh->nud_state & NUD_VALID))
			ret = rdma_copy_addr(addr, neigh->dev, neigh->ha);
	}

	dst_release(dst);
	return ret;
}

static int addr_resolve_remote(struct sockaddr *src_in,
				struct sockaddr *dst_in,
				struct rdma_dev_addr *addr)
{
	if (src_in->sa_family == AF_INET) {
		return addr4_resolve_remote((struct sockaddr_in *) src_in,
			(struct sockaddr_in *) dst_in, addr);
	} else
		return addr6_resolve_remote((struct sockaddr_in6 *) src_in,
			(struct sockaddr_in6 *) dst_in, addr);
}

static void process_req(struct work_struct *work)
{
	struct addr_req *req, *temp_req;
	struct sockaddr *src_in, *dst_in;
	struct list_head done_list;

	INIT_LIST_HEAD(&done_list);

	mutex_lock(&lock);
	list_for_each_entry_safe(req, temp_req, &req_list, list) {
		if (req->status == -ENODATA) {
			src_in = (struct sockaddr *) &req->src_addr;
			dst_in = (struct sockaddr *) &req->dst_addr;
			req->status = addr_resolve_remote(src_in, dst_in,
							  req->addr);
			if (req->status && time_after_eq(jiffies, req->timeout))
				req->status = -ETIMEDOUT;
			else if (req->status == -ENODATA)
				continue;
		}
		list_move_tail(&req->list, &done_list);
	}

	if (!list_empty(&req_list)) {
		req = list_entry(req_list.next, struct addr_req, list);
		set_timeout(req->timeout);
	}
	mutex_unlock(&lock);

	list_for_each_entry_safe(req, temp_req, &done_list, list) {
		list_del(&req->list);
		req->callback(req->status, (struct sockaddr *) &req->src_addr,
			req->addr, req->context);
		put_client(req->client);
		kfree(req);
	}
}

static int addr_resolve_local(struct sockaddr *src_in,
			      struct sockaddr *dst_in,
			      struct rdma_dev_addr *addr)
{
	struct net_device *dev;
	int ret;

	if (dst_in->sa_family == AF_INET) {
		__be32 src_ip = ((struct sockaddr_in *) src_in)->sin_addr.s_addr;
		__be32 dst_ip = ((struct sockaddr_in *) dst_in)->sin_addr.s_addr;

		dev = ip_dev_find(&init_net, dst_ip);
		if (!dev)
			return -EADDRNOTAVAIL;

		if (ipv4_is_zeronet(src_ip)) {
			src_in->sa_family = dst_in->sa_family;
			((struct sockaddr_in *) src_in)->sin_addr.s_addr = dst_ip;
			ret = rdma_copy_addr(addr, dev, dev->dev_addr);
		} else if (ipv4_is_loopback(src_ip)) {
			ret = rdma_translate_ip(dst_in, addr);
			if (!ret)
				memcpy(addr->dst_dev_addr, dev->dev_addr, MAX_ADDR_LEN);
		} else {
			ret = rdma_translate_ip(src_in, addr);
			if (!ret)
				memcpy(addr->dst_dev_addr, dev->dev_addr, MAX_ADDR_LEN);
		}
		dev_put(dev);
	} else {
		struct in6_addr *a;

		for_each_netdev(&init_net, dev)
			if (ipv6_chk_addr(&init_net,
					  &((struct sockaddr_in6 *) addr)->sin6_addr,
					  dev, 1))
				break;

		if (!dev)
			return -EADDRNOTAVAIL;

		a = &((struct sockaddr_in6 *) src_in)->sin6_addr;

		if (ipv6_addr_any(a)) {
			src_in->sa_family = dst_in->sa_family;
			((struct sockaddr_in6 *) src_in)->sin6_addr =
				((struct sockaddr_in6 *) dst_in)->sin6_addr;
			ret = rdma_copy_addr(addr, dev, dev->dev_addr);
		} else if (ipv6_addr_loopback(a)) {
			ret = rdma_translate_ip(dst_in, addr);
			if (!ret)
				memcpy(addr->dst_dev_addr, dev->dev_addr, MAX_ADDR_LEN);
		} else  {
			ret = rdma_translate_ip(src_in, addr);
			if (!ret)
				memcpy(addr->dst_dev_addr, dev->dev_addr, MAX_ADDR_LEN);
		}
	}

	return ret;
}

int rdma_resolve_ip(struct rdma_addr_client *client,
		    struct sockaddr *src_addr, struct sockaddr *dst_addr,
		    struct rdma_dev_addr *addr, int timeout_ms,
		    void (*callback)(int status, struct sockaddr *src_addr,
				     struct rdma_dev_addr *addr, void *context),
		    void *context)
{
	struct sockaddr *src_in, *dst_in;
	struct addr_req *req;
	int ret = 0;

	req = kzalloc(sizeof *req, GFP_KERNEL);
	if (!req)
		return -ENOMEM;

	if (src_addr)
		memcpy(&req->src_addr, src_addr, ip_addr_size(src_addr));
	memcpy(&req->dst_addr, dst_addr, ip_addr_size(dst_addr));
	req->addr = addr;
	req->callback = callback;
	req->context = context;
	req->client = client;
	atomic_inc(&client->refcount);

	src_in = (struct sockaddr *) &req->src_addr;
	dst_in = (struct sockaddr *) &req->dst_addr;

	req->status = addr_resolve_local(src_in, dst_in, addr);
	if (req->status == -EADDRNOTAVAIL)
		req->status = addr_resolve_remote(src_in, dst_in, addr);

	switch (req->status) {
	case 0:
		req->timeout = jiffies;
		queue_req(req);
		break;
	case -ENODATA:
		req->timeout = msecs_to_jiffies(timeout_ms) + jiffies;
		queue_req(req);
		addr_send_arp(dst_in);
		break;
	default:
		ret = req->status;
		atomic_dec(&client->refcount);
		kfree(req);
		break;
	}
	return ret;
}
EXPORT_SYMBOL(rdma_resolve_ip);

void rdma_addr_cancel(struct rdma_dev_addr *addr)
{
	struct addr_req *req, *temp_req;

	mutex_lock(&lock);
	list_for_each_entry_safe(req, temp_req, &req_list, list) {
		if (req->addr == addr) {
			req->status = -ECANCELED;
			req->timeout = jiffies;
			list_move(&req->list, &req_list);
			set_timeout(req->timeout);
			break;
		}
	}
	mutex_unlock(&lock);
}
EXPORT_SYMBOL(rdma_addr_cancel);

static int netevent_callback(struct notifier_block *self, unsigned long event,
	void *ctx)
{
	if (event == NETEVENT_NEIGH_UPDATE) {
		struct neighbour *neigh = ctx;

		if (neigh->nud_state & NUD_VALID) {
			set_timeout(jiffies);
		}
	}
	return 0;
}

static struct notifier_block nb = {
	.notifier_call = netevent_callback
};

static int addr_init(void)
{
	addr_wq = create_singlethread_workqueue("ib_addr");
	if (!addr_wq)
		return -ENOMEM;

	register_netevent_notifier(&nb);
	return 0;
}

static void addr_cleanup(void)
{
	unregister_netevent_notifier(&nb);
	destroy_workqueue(addr_wq);
}

module_init(addr_init);
module_exit(addr_cleanup);
