/*
 * Copyright (c) 2014 Intel Corporation, Inc.  All rights reserved.
 *
 * This software is available to you under a choice of one of two
 * licenses.  You may choose to be licensed under the terms of the GNU
 * General Public License (GPL) Version 2, available from the file
 * COPYING in the main directory of this source tree, or the
 * BSD license below:
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

#if HAVE_CONFIG_H
#  include <config.h>
#endif /* HAVE_CONFIG_H */

#include <stdlib.h>
#include <string.h>

#include "sock.h"
#include "sock_util.h"

#define SOCK_LOG_DBG(...) _SOCK_LOG_DBG(FI_LOG_DOMAIN, __VA_ARGS__)
#define SOCK_LOG_ERROR(...) _SOCK_LOG_ERROR(FI_LOG_DOMAIN, __VA_ARGS__)

const struct fi_domain_attr sock_domain_attr = {
	.name = NULL,
	.threading = FI_THREAD_SAFE,
	.control_progress = FI_PROGRESS_AUTO,
	.data_progress = FI_PROGRESS_AUTO,
	.resource_mgmt = FI_RM_ENABLED,
	.mr_mode = FI_MR_SCALABLE,
	.mr_key_size = sizeof(uint16_t),
	.cq_data_size = sizeof(uint64_t),
	.cq_cnt = SOCK_EP_MAX_CQ_CNT,
	.ep_cnt = SOCK_EP_MAX_EP_CNT,
	.tx_ctx_cnt = SOCK_EP_MAX_TX_CNT,
	.rx_ctx_cnt = SOCK_EP_MAX_RX_CNT,
	.max_ep_tx_ctx = SOCK_EP_MAX_TX_CNT,
	.max_ep_rx_ctx = SOCK_EP_MAX_RX_CNT,
	.max_ep_stx_ctx = SOCK_EP_MAX_EP_CNT,
	.max_ep_srx_ctx = SOCK_EP_MAX_EP_CNT,
};

int sock_verify_domain_attr(struct fi_domain_attr *attr)
{
	if(!attr)
		return 0;

	if(attr->name){
		if (strcmp(attr->name, sock_dom_name))
			return -FI_ENODATA;
	}

	switch(attr->threading){
	case FI_THREAD_UNSPEC:
	case FI_THREAD_SAFE:
	case FI_THREAD_FID:
	case FI_THREAD_DOMAIN:
	case FI_THREAD_COMPLETION:
	case FI_THREAD_ENDPOINT:
		break;
	default:
		SOCK_LOG_DBG("Invalid threading model!\n");
		return -FI_ENODATA;
	}

	switch (attr->control_progress){
	case FI_PROGRESS_UNSPEC:
	case FI_PROGRESS_AUTO:
	case FI_PROGRESS_MANUAL:
		break;

	default:
		SOCK_LOG_DBG("Control progress mode not supported!\n");
		return -FI_ENODATA;
	}

	switch (attr->data_progress){
	case FI_PROGRESS_UNSPEC:
	case FI_PROGRESS_AUTO:
	case FI_PROGRESS_MANUAL:
		break;

	default:
		SOCK_LOG_DBG("Data progress mode not supported!\n");
		return -FI_ENODATA;
	}

	switch (attr->resource_mgmt){
	case FI_RM_UNSPEC:
	case FI_RM_DISABLED:
	case FI_RM_ENABLED:
		break;

	default:
		SOCK_LOG_DBG("Resource mgmt not supported!\n");
		return -FI_ENODATA;
	}

	switch (attr->av_type) {
	case FI_AV_UNSPEC:
	case FI_AV_MAP:
	case FI_AV_TABLE:
		break;

	default:
		SOCK_LOG_DBG("AV type not supported!\n");
		return -FI_ENODATA;
	}

	switch (attr->mr_mode) {
	case FI_MR_UNSPEC:
	case FI_MR_BASIC:
	case FI_MR_SCALABLE:
		break;
	default:
		SOCK_LOG_DBG("MR mode not supported\n");
		return -FI_ENODATA;
	}

	if(attr->mr_key_size > sock_domain_attr.mr_key_size)
		return -FI_ENODATA;
	
	if(attr->cq_data_size > sock_domain_attr.cq_data_size)
		return -FI_ENODATA;

	if(attr->cq_cnt > sock_domain_attr.cq_cnt)
		return -FI_ENODATA;

	if(attr->ep_cnt > sock_domain_attr.ep_cnt)
		return -FI_ENODATA;

	if(attr->max_ep_tx_ctx > sock_domain_attr.max_ep_tx_ctx)
		return -FI_ENODATA;

	if(attr->max_ep_rx_ctx > sock_domain_attr.max_ep_rx_ctx)
		return -FI_ENODATA;

	return 0;
}

static int sock_dom_close(struct fid *fid)
{
	struct sock_domain *dom;
	dom = container_of(fid, struct sock_domain, dom_fid.fid);
	if (atomic_get(&dom->ref)) {
		return -FI_EBUSY;
	}

	sock_pe_finalize(dom->pe);
	if (dom->r_cmap.size)
		sock_conn_map_destroy(&dom->r_cmap);
	fastlock_destroy(&dom->r_cmap.lock);
	fastlock_destroy(&dom->lock);
	sock_dom_remove_from_list(dom);
	free(dom);
	return 0;
}

static uint16_t sock_get_mr_key(struct sock_domain *dom)
{
	uint16_t i;

	for (i = 1; i < IDX_MAX_INDEX; i++) {
		if (!idm_lookup(&dom->mr_idm, i))
			return i;
	}
	return 0;
}

static int sock_mr_close(struct fid *fid)
{
	struct sock_domain *dom;
	struct sock_mr *mr;

	mr = container_of(fid, struct sock_mr, mr_fid.fid);
	dom = mr->domain;
	fastlock_acquire(&dom->lock);
	idm_clear(&dom->mr_idm , (int) mr->mr_fid.key);
	fastlock_release(&dom->lock);
	atomic_dec(&dom->ref);
	free(mr);
	return 0;
}

static int sock_mr_bind(struct fid *fid, struct fid *bfid, uint64_t flags)
{
	struct sock_cntr *cntr;
	struct sock_cq *cq;
	struct sock_mr *mr;

	mr = container_of(fid, struct sock_mr, mr_fid.fid);
	switch (bfid->fclass) {
	case FI_CLASS_CQ:
		cq = container_of(bfid, struct sock_cq, cq_fid.fid);
		if (mr->domain != cq->domain)
			return -FI_EINVAL;

		if (flags & FI_REMOTE_WRITE)
			mr->cq = cq;
		break;

	case FI_CLASS_CNTR:
		cntr = container_of(bfid, struct sock_cntr, cntr_fid.fid);
		if (mr->domain != cntr->domain)
			return -FI_EINVAL;

		if (flags & FI_REMOTE_WRITE)
			mr->cntr = cntr;
		break;

	default:
		return -FI_EINVAL;
	}
	return 0;
}

static struct fi_ops sock_mr_fi_ops = {
	.size = sizeof(struct fi_ops),
	.close = sock_mr_close,
	.bind = sock_mr_bind,
	.control = fi_no_control,
	.ops_open = fi_no_ops_open,
};

struct sock_mr * sock_mr_get_entry(struct sock_domain *domain, uint16_t key)
{
	return (struct sock_mr *)idm_lookup(&domain->mr_idm, key);
}

struct sock_mr *sock_mr_verify_key(struct sock_domain *domain, uint16_t key, 
				   void *buf, size_t len, uint64_t access)
{
	int i;
	struct sock_mr *mr;
	mr = idm_lookup(&domain->mr_idm, key);
	
	if (!mr)
		return NULL;

	if (domain->attr.mr_mode == FI_MR_SCALABLE)
		buf = (char*)buf + mr->offset;
	
	for (i = 0; i < mr->iov_count; i++) {
		if ((uintptr_t)buf >= (uintptr_t)mr->mr_iov[i].iov_base &&
		    ((uintptr_t)buf + len <= (uintptr_t) mr->mr_iov[i].iov_base + 
		     mr->mr_iov[i].iov_len)) {
			if ((access & mr->access) == access)
				return mr;
		}
	}
	SOCK_LOG_ERROR("MR check failed\n");
	return NULL;
}

struct sock_mr *sock_mr_verify_desc(struct sock_domain *domain, void *desc, 
			void *buf, size_t len, uint64_t access)
{
	uint64_t key = (uintptr_t) desc;
	return sock_mr_verify_key(domain, key, buf, len, access);
}

static int sock_regattr(struct fid *fid, const struct fi_mr_attr *attr,
		uint64_t flags, struct fid_mr **mr)
{
	struct fi_eq_entry eq_entry;
	struct sock_domain *dom;
	struct sock_mr *_mr;
	uint64_t key;
	struct fid_domain *domain;

	if (fid->fclass != FI_CLASS_DOMAIN || !attr || attr->iov_count <= 0) {
		return -FI_EINVAL;
	}

	domain = container_of(fid, struct fid_domain, fid);
	dom = container_of(domain, struct sock_domain, dom_fid);
	if ((dom->attr.mr_mode == FI_MR_SCALABLE) &&
	    ((attr->requested_key > IDX_MAX_INDEX) ||
	     idm_lookup(&dom->mr_idm, (int) attr->requested_key)))
		return -FI_ENOKEY;
	
	_mr = calloc(1, sizeof(*_mr) + 
		     sizeof(_mr->mr_iov) * (attr->iov_count - 1));
	if (!_mr)
		return -FI_ENOMEM;

	_mr->mr_fid.fid.fclass = FI_CLASS_MR;
	_mr->mr_fid.fid.context = attr->context;
	_mr->mr_fid.fid.ops = &sock_mr_fi_ops;

	_mr->domain = dom;
	_mr->access = attr->access;
	_mr->flags = flags;
	_mr->offset = (dom->attr.mr_mode == FI_MR_SCALABLE) ?
		(uintptr_t) attr->mr_iov[0].iov_base + attr->offset : 
		(uintptr_t) attr->mr_iov[0].iov_base;

	fastlock_acquire(&dom->lock);
	key = (dom->attr.mr_mode == FI_MR_BASIC) ?
		sock_get_mr_key(dom) : (uint16_t) attr->requested_key;
	if (idm_set(&dom->mr_idm, key, _mr) < 0)
		goto err;
	_mr->mr_fid.key = key;
	_mr->mr_fid.mem_desc = (void *) (uintptr_t) key;
	fastlock_release(&dom->lock);

	_mr->iov_count = attr->iov_count;
	memcpy(&_mr->mr_iov, attr->mr_iov, sizeof(_mr->mr_iov) * attr->iov_count);

	*mr = &_mr->mr_fid;
	atomic_inc(&dom->ref);

	if (dom->mr_eq) {
		eq_entry.fid = &domain->fid;
		eq_entry.context = attr->context;
		return sock_eq_report_event(dom->mr_eq, FI_MR_COMPLETE,
					    &eq_entry, sizeof(eq_entry), 0);
	}

	return 0;

err:
	fastlock_release(&dom->lock);
	free(_mr);
	return -errno;
}

static int sock_regv(struct fid *fid, const struct iovec *iov,
		size_t count, uint64_t access,
		uint64_t offset, uint64_t requested_key,
		uint64_t flags, struct fid_mr **mr, void *context)
{
	struct fi_mr_attr attr;

	attr.mr_iov = iov;
	attr.iov_count = count;
	attr.access = access;
	attr.offset = offset;
	attr.requested_key = requested_key;
	attr.context = context;
	return sock_regattr(fid, &attr, flags, mr);
}

static int sock_reg(struct fid *fid, const void *buf, size_t len,
		uint64_t access, uint64_t offset, uint64_t requested_key,
		uint64_t flags, struct fid_mr **mr, void *context)
{
	struct iovec iov;

	iov.iov_base = (void *) buf;
	iov.iov_len = len;
	return sock_regv(fid, &iov, 1, access,  offset, requested_key,
			 flags, mr, context);
}

static int sock_dom_bind(struct fid *fid, struct fid *bfid, uint64_t flags)
{
	struct sock_domain *dom;
	struct sock_eq *eq;

	dom = container_of(fid, struct sock_domain, dom_fid.fid);
	eq = container_of(bfid, struct sock_eq, eq.fid);

	if (dom->eq)
		return -FI_EINVAL;

	dom->eq = eq;
	if (flags & FI_REG_MR)
		dom->mr_eq = eq;

	return 0;
}

static int sock_endpoint(struct fid_domain *domain, struct fi_info *info,
			 struct fid_ep **ep, void *context)
{
	switch (info->ep_attr->type) {
	case FI_EP_RDM:
		return sock_rdm_ep(domain, info, ep, context);
	case FI_EP_DGRAM:
		return sock_dgram_ep(domain, info, ep, context);
	case FI_EP_MSG:
		return sock_msg_ep(domain, info, ep, context);
	default:
		return -FI_ENOPROTOOPT;
	}
}

static int sock_scalable_ep(struct fid_domain *domain, struct fi_info *info,
		     struct fid_ep **sep, void *context)
{
	switch (info->ep_attr->type) {
	case FI_EP_RDM:
		return sock_rdm_sep(domain, info, sep, context);
	case FI_EP_DGRAM:
		return sock_dgram_sep(domain, info, sep, context);
	case FI_EP_MSG:
		return sock_msg_sep(domain, info, sep, context);
	default:
		return -FI_ENOPROTOOPT;
	}
}

static struct fi_ops sock_dom_fi_ops = {
	.size = sizeof(struct fi_ops),
	.close = sock_dom_close,
	.bind = sock_dom_bind,
	.control = fi_no_control,
	.ops_open = fi_no_ops_open,
};

static struct fi_ops_domain sock_dom_ops = {
	.size = sizeof(struct fi_ops_domain),
	.av_open = sock_av_open,
	.cq_open = sock_cq_open,
	.endpoint = sock_endpoint,
	.scalable_ep = sock_scalable_ep,
	.cntr_open = sock_cntr_open,
	.poll_open = sock_poll_open,
	.stx_ctx = sock_stx_ctx,
	.srx_ctx = sock_srx_ctx,
};

static struct fi_ops_mr sock_dom_mr_ops = {
	.size = sizeof(struct fi_ops_mr),
	.reg = sock_reg,
	.regv = sock_regv,
	.regattr = sock_regattr,
};

int sock_domain(struct fid_fabric *fabric, struct fi_info *info,
		struct fid_domain **dom, void *context)
{
	struct sock_domain *sock_domain;
	struct sock_fabric *fab;
	int ret;

	fab = container_of(fabric, struct sock_fabric, fab_fid);
	if(info && info->domain_attr){
		ret = sock_verify_domain_attr(info->domain_attr);
		if(ret)
			return ret;
	}

	sock_domain = calloc(1, sizeof *sock_domain);
	if (!sock_domain)
		return -FI_ENOMEM;

	fastlock_init(&sock_domain->lock);
	atomic_initialize(&sock_domain->ref, 0);

	if (info) {
		sock_domain->info = *info;
	} else {
		SOCK_LOG_ERROR("invalid fi_info\n");
		goto err;
	}

	sock_domain->dom_fid.fid.fclass = FI_CLASS_DOMAIN;
	sock_domain->dom_fid.fid.context = context;
	sock_domain->dom_fid.fid.ops = &sock_dom_fi_ops;
	sock_domain->dom_fid.ops = &sock_dom_ops;
	sock_domain->dom_fid.mr = &sock_dom_mr_ops;

	if (!info || !info->domain_attr || 
	    info->domain_attr->data_progress == FI_PROGRESS_UNSPEC)
		sock_domain->progress_mode = FI_PROGRESS_AUTO;
	else
		sock_domain->progress_mode = info->domain_attr->data_progress;

	sock_domain->pe = sock_pe_init(sock_domain);
	if (!sock_domain->pe){
		SOCK_LOG_ERROR("Failed to init PE\n");
		goto err;
	}

	if (sock_conn_map_init(&sock_domain->r_cmap, sock_cm_def_map_sz))
		goto err;

	sock_domain->r_cmap.domain = sock_domain;
	fastlock_init(&sock_domain->r_cmap.lock);

	sock_domain->fab = fab;
	*dom = &sock_domain->dom_fid;

	if (info->domain_attr)
		sock_domain->attr = *(info->domain_attr);
	else
		sock_domain->attr = sock_domain_attr;

	sock_dom_add_to_list(sock_domain);
	return 0;

err:
	free(sock_domain);
	return -FI_EINVAL;
}
