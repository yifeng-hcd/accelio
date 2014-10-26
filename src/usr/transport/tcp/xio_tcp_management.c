/*
 * Copyright (c) 2013 Mellanox Technologies®. All rights reserved.
 *
 * This software is available to you under a choice of one of two licenses.
 * You may choose to be licensed under the terms of the GNU General Public
 * License (GPL) Version 2, available from the file COPYING in the main
 * directory of this source tree, or the Mellanox Technologies® BSD license
 * below:
 *
 *      - Redistribution and use in source and binary forms, with or without
 *        modification, are permitted provided that the following conditions
 *        are met:
 *
 *      - Redistributions of source code must retain the above copyright
 *        notice, this list of conditions and the following disclaimer.
 *
 *      - Redistributions in binary form must reproduce the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer in the documentation and/or other materials
 *        provided with the distribution.
 *
 *      - Neither the name of the Mellanox Technologies® nor the names of its
 *        contributors may be used to endorse or promote products derived from
 *        this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include <linux/tcp.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include "xio_os.h"
#include "libxio.h"
#include "xio_log.h"
#include "xio_common.h"
#include "xio_observer.h"
#include "xio_protocol.h"
#include "xio_mbuf.h"
#include "xio_task.h"
#include "xio_transport_mempool.h"
#include "xio_sg_table.h"
#include "xio_transport.h"
#include "xio_usr_transport.h"
#include "xio_ev_data.h"
#include "xio_workqueue.h"
#include "xio_context.h"
#include "xio_tcp_transport.h"


/* default option values */
#define XIO_OPTVAL_DEF_ENABLE_MEM_POOL			1
#define XIO_OPTVAL_DEF_ENABLE_MR_CHECK			0
#define XIO_OPTVAL_DEF_TCP_ENABLE_DMA_LATENCY		0
#define XIO_OPTVAL_DEF_TCP_BUF_THRESHOLD		SEND_BUF_SZ
#define XIO_OPTVAL_DEF_TCP_MAX_IN_IOVSZ			XIO_IOVLEN
#define XIO_OPTVAL_DEF_TCP_MAX_OUT_IOVSZ		XIO_IOVLEN
#define XIO_OPTVAL_DEF_TCP_NO_DELAY			0
#define XIO_OPTVAL_DEF_TCP_SO_SNDBUF			4194304
#define XIO_OPTVAL_DEF_TCP_SO_RCVBUF			4194304
#define XIO_OPTVAL_DEF_TCP_DUAL_SOCK			1

#define XIO_OPTVAL_MIN_TCP_BUF_THRESHOLD		256
#define XIO_OPTVAL_MAX_TCP_BUF_THRESHOLD		65536


/*---------------------------------------------------------------------------*/
/* globals								     */
/*---------------------------------------------------------------------------*/
static spinlock_t			mngmt_lock;
static pthread_once_t			ctor_key_once = PTHREAD_ONCE_INIT;
static pthread_once_t			dtor_key_once = PTHREAD_ONCE_INIT;
struct xio_transport			xio_tcp_transport;
struct xio_tcp_socket_ops		single_sock_ops;
struct xio_tcp_socket_ops		dual_sock_ops;

static int				cdl_fd = -1;

/* tcp options */
struct xio_tcp_options			tcp_options = {
	.enable_mem_pool		= XIO_OPTVAL_DEF_ENABLE_MEM_POOL,
	.enable_dma_latency		= XIO_OPTVAL_DEF_TCP_ENABLE_DMA_LATENCY,
	.enable_mr_check		= XIO_OPTVAL_DEF_ENABLE_MR_CHECK,
	.tcp_buf_threshold		= XIO_OPTVAL_DEF_TCP_BUF_THRESHOLD,
	.tcp_buf_attr_rdonly		= 0,
	.max_in_iovsz			= XIO_OPTVAL_DEF_TCP_MAX_IN_IOVSZ,
	.max_out_iovsz			= XIO_OPTVAL_DEF_TCP_MAX_OUT_IOVSZ,
	.tcp_no_delay			= XIO_OPTVAL_DEF_TCP_NO_DELAY,
	.tcp_so_sndbuf			= XIO_OPTVAL_DEF_TCP_SO_SNDBUF,
	.tcp_so_rcvbuf			= XIO_OPTVAL_DEF_TCP_SO_RCVBUF,
	.tcp_dual_sock			= XIO_OPTVAL_DEF_TCP_DUAL_SOCK,
};

/*---------------------------------------------------------------------------*/
/* xio_tcp_get_max_header_size						     */
/*---------------------------------------------------------------------------*/
static int xio_tcp_get_max_header_size(void)
{
	int req_hdr = XIO_TRANSPORT_OFFSET + sizeof(struct xio_tcp_req_hdr);
	int rsp_hdr = XIO_TRANSPORT_OFFSET + sizeof(struct xio_tcp_rsp_hdr);
	int iovsz = tcp_options.max_out_iovsz + tcp_options.max_in_iovsz;

	req_hdr += iovsz*sizeof(struct xio_sge);
	rsp_hdr += tcp_options.max_out_iovsz*sizeof(struct xio_sge);

	return max(req_hdr, rsp_hdr);
}
/*---------------------------------------------------------------------------*/
/* xio_tcp_flush_all_tasks						     */
/*---------------------------------------------------------------------------*/
static int xio_tcp_flush_all_tasks(struct xio_tcp_transport *tcp_hndl)
{
	if (!list_empty(&tcp_hndl->in_flight_list)) {
		TRACE_LOG("in_flight_list not empty!\n");
		xio_transport_flush_task_list(&tcp_hndl->in_flight_list);
		/* for task that attched to senders with ref count = 2 */
		xio_transport_flush_task_list(&tcp_hndl->in_flight_list);
	}

	if (!list_empty(&tcp_hndl->tx_comp_list)) {
		TRACE_LOG("tx_comp_list not empty!\n");
		xio_transport_flush_task_list(&tcp_hndl->tx_comp_list);
	}
	if (!list_empty(&tcp_hndl->io_list)) {
		TRACE_LOG("io_list not empty!\n");
		xio_transport_flush_task_list(&tcp_hndl->io_list);
	}

	if (!list_empty(&tcp_hndl->tx_ready_list)) {
		TRACE_LOG("tx_ready_list not empty!\n");
		xio_transport_flush_task_list(&tcp_hndl->tx_ready_list);
		/* for task that attached to senders with ref count = 2 */
		xio_transport_flush_task_list(&tcp_hndl->tx_ready_list);
	}

	if (!list_empty(&tcp_hndl->rx_list)) {
		TRACE_LOG("rx_list not empty!\n");
		xio_transport_flush_task_list(&tcp_hndl->rx_list);
	}

	tcp_hndl->tx_ready_tasks_num = 0;

	return 0;
}

/*---------------------------------------------------------------------------*/
/* on_sock_close							     */
/*---------------------------------------------------------------------------*/
static void on_sock_close(struct xio_tcp_transport *tcp_hndl)
{
	TRACE_LOG("on_sock_close tcp_hndl:%p, state:%d\n\n",
		  tcp_hndl, tcp_hndl->state);

	xio_tcp_flush_all_tasks(tcp_hndl);

	xio_transport_notify_observer(&tcp_hndl->base,
				      XIO_TRANSPORT_CLOSED,
				      NULL);

	tcp_hndl->state = XIO_STATE_DESTROYED;
}

/*---------------------------------------------------------------------------*/
/* xio_tcp_single_sock_del_ev_handlers		                             */
/*---------------------------------------------------------------------------*/
int xio_tcp_single_sock_del_ev_handlers(struct xio_tcp_transport *tcp_hndl)
{
	int retval;

	/* remove from epoll */
	retval = xio_context_del_ev_handler(tcp_hndl->base.ctx,
					    tcp_hndl->sock.cfd);

	if (retval) {
		ERROR_LOG("tcp_hndl:%p fd=%d del_ev_handler failed, %m\n",
			  tcp_hndl, tcp_hndl->sock.cfd);
	}

	return retval;
}

/*---------------------------------------------------------------------------*/
/* xio_tcp_dual_sock_del_ev_handlers		                             */
/*---------------------------------------------------------------------------*/
int xio_tcp_dual_sock_del_ev_handlers(struct xio_tcp_transport *tcp_hndl)
{
	int retval1, retval2;

	/* remove from epoll */
	retval1 = xio_context_del_ev_handler(tcp_hndl->base.ctx,
					     tcp_hndl->sock.cfd);
	if (retval1) {
		ERROR_LOG("tcp_hndl:%p fd=%d del_ev_handler failed, %m\n",
			  tcp_hndl, tcp_hndl->sock.cfd);
	}

	if (tcp_hndl->is_listen)
		return retval1;

	/* remove from epoll */
	retval2 = xio_context_del_ev_handler(tcp_hndl->base.ctx,
					     tcp_hndl->sock.dfd);

	if (retval2) {
		ERROR_LOG("tcp_hndl:%p fd=%d del_ev_handler failed, %m\n",
			  tcp_hndl, tcp_hndl->sock.dfd);
	}

	return retval1 | retval2;
}

/*---------------------------------------------------------------------------*/
/* on_sock_disconnected							     */
/*---------------------------------------------------------------------------*/
void on_sock_disconnected(struct xio_tcp_transport *tcp_hndl,
			  int passive_close)
{
	struct xio_tcp_pending_conn *pconn, *next_pconn;
	int retval;

	TRACE_LOG("on_sock_disconnected. tcp_hndl:%p, state:%d\n",
		  tcp_hndl, tcp_hndl->state);
	if (tcp_hndl->state == XIO_STATE_DISCONNECTED) {
		TRACE_LOG("call to close. tcp_hndl:%p\n",
			  tcp_hndl);
		tcp_hndl->state = XIO_STATE_CLOSED;

		xio_ctx_remove_event(tcp_hndl->base.ctx,
                                     &tcp_hndl->flush_tx_event);
		xio_ctx_remove_event(tcp_hndl->base.ctx,
				     &tcp_hndl->ctl_rx_event);

		if (tcp_hndl->sock.ops->del_ev_handlers)
			tcp_hndl->sock.ops->del_ev_handlers(tcp_hndl);

		if (!passive_close && !tcp_hndl->is_listen) { /*active close*/
			tcp_hndl->sock.ops->shutdown(&tcp_hndl->sock);
		}
		tcp_hndl->sock.ops->close(&tcp_hndl->sock);

		list_for_each_entry_safe(pconn, next_pconn,
					 &tcp_hndl->pending_conns,
					 conns_list_entry) {
			retval = xio_context_del_ev_handler(tcp_hndl->base.ctx,
							    pconn->fd);
			if (retval) {
				ERROR_LOG(
				"removing conn handler failed.(errno=%d %m)\n",
				errno);
			}
			list_del(&pconn->conns_list_entry);
			ufree(pconn);
		}

		if (passive_close) {
			xio_transport_notify_observer(
					&tcp_hndl->base,
					XIO_TRANSPORT_DISCONNECTED,
					NULL);
		}
	}
}

/*---------------------------------------------------------------------------*/
/* xio_tcp_post_close							     */
/*---------------------------------------------------------------------------*/
static void xio_tcp_post_close(struct xio_tcp_transport *tcp_hndl)
{
	TRACE_LOG("tcp transport: [post close] handle:%p\n",
		  tcp_hndl);

	xio_ctx_remove_event(tcp_hndl->base.ctx, &tcp_hndl->disconnect_event);

	xio_observable_unreg_all_observers(&tcp_hndl->base.observable);

	if (tcp_hndl->tmp_rx_buf) {
		ufree(tcp_hndl->tmp_rx_buf);
		tcp_hndl->tmp_rx_buf = NULL;
	}

	ufree(tcp_hndl->base.portal_uri);

	XIO_OBSERVABLE_DESTROY(&tcp_hndl->base.observable);

	ufree(tcp_hndl);
}

/*---------------------------------------------------------------------------*/
/* xio_tcp_close_cb		                                             */
/*---------------------------------------------------------------------------*/
static void xio_tcp_close_cb(struct kref *kref)
{
	struct xio_transport_base *transport = container_of(
					kref, struct xio_transport_base, kref);
	struct xio_tcp_transport *tcp_hndl =
		(struct xio_tcp_transport *)transport;

	/* now it is zero */
	TRACE_LOG("xio_tcp_close: [close] handle:%p, fd:%d\n",
		  tcp_hndl, tcp_hndl->sock.cfd);

	switch (tcp_hndl->state) {
	case XIO_STATE_LISTEN:
	case XIO_STATE_CONNECTED:
		tcp_hndl->state = XIO_STATE_DISCONNECTED;
		/*fallthrough*/
	case XIO_STATE_DISCONNECTED:
		on_sock_disconnected(tcp_hndl, 0);
		/*fallthrough*/
	case XIO_STATE_CLOSED:
		on_sock_close(tcp_hndl);
		break;
	default:
		xio_transport_notify_observer(
				&tcp_hndl->base,
				XIO_TRANSPORT_CLOSED,
				NULL);
		tcp_hndl->state = XIO_STATE_DESTROYED;
		break;
	}

	if (tcp_hndl->state  == XIO_STATE_DESTROYED)
		xio_tcp_post_close(tcp_hndl);
}

/*---------------------------------------------------------------------------*/
/* xio_tcp_close		                                             */
/*---------------------------------------------------------------------------*/
static void xio_tcp_close(struct xio_transport_base *transport)
{
	int was = atomic_read(&transport->kref.refcount);

	/* this is only for debugging - please note that the combination of
	 * atomic_read and kref_put is not atomic - please remove if this
	 * error does not pop up. Otherwise contact me and report bug.
	 */

	/* was already 0 */
	if (!was) {
		ERROR_LOG("xio_tcp_close double close. handle:%p\n",
			  transport);
		return;
	}

	kref_put(&transport->kref, xio_tcp_close_cb);
}

/*---------------------------------------------------------------------------*/
/* xio_tcp_single_sock_shutdown		                                     */
/*---------------------------------------------------------------------------*/
int xio_tcp_single_sock_shutdown(struct xio_tcp_socket *sock)
{
	int retval;

	retval = shutdown(sock->cfd, SHUT_RDWR);
	if (retval) {
		xio_set_error(errno);
		DEBUG_LOG("tcp shutdown failed. (errno=%d %m)\n", errno);
	}

	return retval;
}

/*---------------------------------------------------------------------------*/
/* xio_tcp_single_sock_close		                                     */
/*---------------------------------------------------------------------------*/
int xio_tcp_single_sock_close(struct xio_tcp_socket *sock)
{
	int retval;

	retval = close(sock->cfd);
	if (retval) {
		xio_set_error(errno);
		DEBUG_LOG("tcp close failed. (errno=%d %m)\n", errno);
	}

	return retval;
}

/*---------------------------------------------------------------------------*/
/* xio_tcp_dual_sock_shutdown		                                     */
/*---------------------------------------------------------------------------*/
int xio_tcp_dual_sock_shutdown(struct xio_tcp_socket *sock)
{
	int retval1, retval2;

	retval1 = shutdown(sock->cfd, SHUT_RDWR);
	if (retval1) {
		xio_set_error(errno);
		DEBUG_LOG("tcp shutdown failed. (errno=%d %m)\n", errno);
	}

	retval2 = shutdown(sock->dfd, SHUT_RDWR);
	if (retval2) {
		xio_set_error(errno);
		DEBUG_LOG("tcp shutdown failed. (errno=%d %m)\n", errno);
	}

	return (retval1 | retval2);
}

/*---------------------------------------------------------------------------*/
/* xio_tcp_dual_sock_close		                                     */
/*---------------------------------------------------------------------------*/
int xio_tcp_dual_sock_close(struct xio_tcp_socket *sock)
{
	int retval1, retval2;

	retval1 = close(sock->cfd);
	if (retval1) {
		xio_set_error(errno);
		DEBUG_LOG("tcp close failed. (errno=%d %m)\n", errno);
	}

	retval2 = close(sock->dfd);
	if (retval2) {
		xio_set_error(errno);
		DEBUG_LOG("tcp close failed. (errno=%d %m)\n", errno);
	}

	return (retval1 | retval2);
}

/*---------------------------------------------------------------------------*/
/* xio_tcp_reject		                                             */
/*---------------------------------------------------------------------------*/
static int xio_tcp_reject(struct xio_transport_base *transport)
{
	struct xio_tcp_transport *tcp_hndl =
		(struct xio_tcp_transport *)transport;
	int				retval;

	tcp_hndl->sock.ops->shutdown(&tcp_hndl->sock);

	retval = tcp_hndl->sock.ops->close(&tcp_hndl->sock);
	if (retval)
		return -1;

	TRACE_LOG("tcp transport: [reject] handle:%p\n", tcp_hndl);

	return 0;
}

/*---------------------------------------------------------------------------*/
/* xio_tcp_context_shutdown						     */
/*---------------------------------------------------------------------------*/
static int xio_tcp_context_shutdown(struct xio_transport_base *trans_hndl,
				    struct xio_context *ctx)
{
	struct xio_tcp_transport *tcp_hndl =
			(struct xio_tcp_transport *)trans_hndl;

	TRACE_LOG("tcp transport context_shutdown handle:%p\n", tcp_hndl);

	switch (tcp_hndl->state) {
	case XIO_STATE_LISTEN:
	case XIO_STATE_CONNECTED:
		tcp_hndl->state = XIO_STATE_DISCONNECTED;
		/*fallthrough*/
	case XIO_STATE_DISCONNECTED:
		on_sock_disconnected(tcp_hndl, 0);
		break;
	default:
		break;
	}

	tcp_hndl->state = XIO_STATE_DESTROYED;
	xio_tcp_flush_all_tasks(tcp_hndl);
	xio_tcp_post_close(tcp_hndl);

	return 0;
}

/*---------------------------------------------------------------------------*/
/* xio_tcp_disconnect_handler						     */
/*---------------------------------------------------------------------------*/
static void xio_tcp_disconnect_handler(xio_ctx_event_t *tev,
				       void *xio_tcp_hndl)
{
	struct xio_tcp_transport *tcp_hndl = xio_tcp_hndl;
	on_sock_disconnected(tcp_hndl, 1);
}

/*---------------------------------------------------------------------------*/
/* xio_tcp_flush_tx_handler						     */
/*---------------------------------------------------------------------------*/
void xio_tcp_flush_tx_handler(xio_ctx_event_t *tev, void *xio_tcp_hndl)
{
	struct xio_tcp_transport *tcp_hndl = xio_tcp_hndl;
	xio_tcp_xmit(tcp_hndl);
}

/*---------------------------------------------------------------------------*/
/* xio_tcp_single_sock_rx_ctl_handler					     */
/*---------------------------------------------------------------------------*/
int xio_tcp_single_sock_rx_ctl_handler(struct xio_tcp_transport *tcp_hndl)
{
	return xio_tcp_rx_ctl_handler(tcp_hndl, 1);
}

/*---------------------------------------------------------------------------*/
/* xio_tcp_dual_sock_rx_ctl_handler					     */
/*---------------------------------------------------------------------------*/
int xio_tcp_dual_sock_rx_ctl_handler(struct xio_tcp_transport *tcp_hndl)
{
	return xio_tcp_rx_ctl_handler(tcp_hndl, RX_BATCH);
}

/*---------------------------------------------------------------------------*/
/* xio_tcp_consume_ctl_rx						     */
/*---------------------------------------------------------------------------*/
void xio_tcp_consume_ctl_rx(xio_ctx_event_t *tev, void *xio_tcp_hndl)
{
	struct xio_tcp_transport *tcp_hndl = xio_tcp_hndl;
	int retval = 0, count = 0;

	xio_ctx_remove_event(tcp_hndl->base.ctx, &tcp_hndl->ctl_rx_event);

	do {
		retval = tcp_hndl->sock.ops->rx_ctl_handler(tcp_hndl);
		++count;
	} while (retval > 0 && count <  RX_POLL_NR_MAX);

	if (/*retval > 0 && */ tcp_hndl->tmp_rx_buf_len &&
	    tcp_hndl->state == XIO_STATE_CONNECTED) {
		xio_ctx_add_event(tcp_hndl->base.ctx, &tcp_hndl->ctl_rx_event);
	}
}

/*---------------------------------------------------------------------------*/
/* xio_tcp_ctl_ready_ev_handler						     */
/*---------------------------------------------------------------------------*/
void xio_tcp_ctl_ready_ev_handler(int fd, int events, void *user_context)
{
	struct xio_tcp_transport	*tcp_hndl = user_context;

	if (events & EPOLLOUT) {
		xio_context_modify_ev_handler(tcp_hndl->base.ctx, fd,
					      XIO_POLLIN | XIO_POLLRDHUP);
		xio_tcp_xmit(tcp_hndl);
	}

	if (events & EPOLLIN)
		xio_tcp_consume_ctl_rx(NULL, tcp_hndl);

	if (events & (EPOLLHUP | EPOLLRDHUP | EPOLLERR)) {
		DEBUG_LOG("epoll returned with error events=%d for fd=%d\n",
			  events, fd);
		xio_tcp_disconnect_helper(tcp_hndl);
	}

	/* ORK todo add work instead of poll_nr? */
}

/*---------------------------------------------------------------------------*/
/* xio_tcp_data_ready_ev_handler					     */
/*---------------------------------------------------------------------------*/
void xio_tcp_data_ready_ev_handler(int fd, int events, void *user_context)
{
	struct xio_tcp_transport	*tcp_hndl = user_context;
	int retval = 0, count = 0;

	if (events & EPOLLOUT) {
		xio_context_modify_ev_handler(tcp_hndl->base.ctx, fd,
					      XIO_POLLIN | XIO_POLLRDHUP);
		xio_tcp_xmit(tcp_hndl);
	}

	if (events & EPOLLIN) {
		do {
			retval = tcp_hndl->sock.ops->rx_data_handler(
							tcp_hndl, RX_BATCH);
			++count;
		} while (retval > 0 && count <  RX_POLL_NR_MAX);
	}

	if (events & (EPOLLHUP | EPOLLRDHUP | EPOLLERR)) {
		DEBUG_LOG("epoll returned with error events=%d for fd=%d\n",
			  events, fd);
		xio_tcp_disconnect_helper(tcp_hndl);
	}
}

/*---------------------------------------------------------------------------*/
/* xio_tcp_single_sock_add_ev_handlers		                             */
/*---------------------------------------------------------------------------*/
int xio_tcp_single_sock_add_ev_handlers(struct xio_tcp_transport *tcp_hndl)
{
	/* add to epoll */
	int retval = xio_context_add_ev_handler(
			tcp_hndl->base.ctx,
			tcp_hndl->sock.cfd,
			XIO_POLLIN | XIO_POLLRDHUP,
			xio_tcp_ctl_ready_ev_handler,
			tcp_hndl);

	if (retval) {
		ERROR_LOG("setting connection handler failed. (errno=%d %m)\n",
			  errno);
	}

	return retval;
}

/*---------------------------------------------------------------------------*/
/* xio_tcp_dual_sock_add_ev_handlers		                             */
/*---------------------------------------------------------------------------*/
int xio_tcp_dual_sock_add_ev_handlers(struct xio_tcp_transport *tcp_hndl)
{
	int retval = 0;

	/* add to epoll */
	retval = xio_context_add_ev_handler(
			tcp_hndl->base.ctx,
			tcp_hndl->sock.cfd,
			XIO_POLLIN | XIO_POLLRDHUP,
			xio_tcp_ctl_ready_ev_handler,
			tcp_hndl);

	if (retval) {
		ERROR_LOG("setting connection handler failed. (errno=%d %m)\n",
			  errno);
		return retval;
	}

	/* add to epoll */
	retval = xio_context_add_ev_handler(
			tcp_hndl->base.ctx,
			tcp_hndl->sock.dfd,
			XIO_POLLIN | XIO_POLLRDHUP,
			xio_tcp_data_ready_ev_handler,
			tcp_hndl);

	if (retval) {
		ERROR_LOG("setting connection handler failed. (errno=%d %m)\n",
			  errno);
		xio_context_del_ev_handler(tcp_hndl->base.ctx,
					   tcp_hndl->sock.cfd);
	}

	return retval;
}

/*---------------------------------------------------------------------------*/
/* xio_tcp_accept		                                             */
/*---------------------------------------------------------------------------*/
static int xio_tcp_accept(struct xio_transport_base *transport)
{
	struct xio_tcp_transport *tcp_hndl =
			(struct xio_tcp_transport *)transport;

	if (tcp_hndl->sock.ops->add_ev_handlers(tcp_hndl)) {
		xio_transport_notify_observer_error(&tcp_hndl->base,
						    XIO_E_UNSUCCESSFUL);
	}

	TRACE_LOG("tcp transport: [accept] handle:%p\n", tcp_hndl);

	xio_transport_notify_observer(
			&tcp_hndl->base,
			XIO_TRANSPORT_ESTABLISHED,
			NULL);

	return 0;
}

/*---------------------------------------------------------------------------*/
/* xio_tcp_socket_create		                                     */
/*---------------------------------------------------------------------------*/
int xio_tcp_socket_create(void)
{
	int sock_fd, retval, optval = 1;

	sock_fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
	if (sock_fd < 0) {
		xio_set_error(errno);
		ERROR_LOG("create socket failed. (errno=%d %m)\n", errno);
		return sock_fd;
	}

	retval = setsockopt(sock_fd,
			    SOL_SOCKET,
			    SO_REUSEADDR,
			    &optval,
			    sizeof(optval));
	if (retval) {
		xio_set_error(errno);
		ERROR_LOG("setsockopt failed. (errno=%d %m)\n", errno);
		goto cleanup;
	}

	if (tcp_options.tcp_no_delay) {
		retval = setsockopt(sock_fd,
				    IPPROTO_TCP,
				    TCP_NODELAY,
				    (char *)&optval,
				    sizeof(int));
		if (retval) {
			xio_set_error(errno);
			ERROR_LOG("setsockopt failed. (errno=%d %m)\n", errno);
			goto cleanup;
		}
	}


	optval = tcp_options.tcp_so_sndbuf;
	retval = setsockopt(sock_fd, SOL_SOCKET, SO_SNDBUF,
			    (char *)&optval, sizeof(optval));
	if (retval) {
		xio_set_error(errno);
		ERROR_LOG("setsockopt failed. (errno=%d %m)\n", errno);
		goto cleanup;
	}
	optval = tcp_options.tcp_so_rcvbuf;
	retval = setsockopt(sock_fd, SOL_SOCKET, SO_RCVBUF,
			    (char *)&optval, sizeof(optval));
	if (retval) {
		xio_set_error(errno);
		ERROR_LOG("setsockopt failed. (errno=%d %m)\n", errno);
		goto cleanup;
	}

	return sock_fd;

cleanup:
	close(sock_fd);
	return -1;
}

/*---------------------------------------------------------------------------*/
/* xio_tcp_single_sock_create		                                     */
/*---------------------------------------------------------------------------*/
int xio_tcp_single_sock_create(struct xio_tcp_socket *sock)
{
	sock->cfd = xio_tcp_socket_create();
	if (sock->cfd < 0)
		return -1;

	sock->dfd = sock->cfd;

	return 0;
}

/*---------------------------------------------------------------------------*/
/* xio_tcp_dual_sock_create		                                     */
/*---------------------------------------------------------------------------*/
int xio_tcp_dual_sock_create(struct xio_tcp_socket *sock)
{
	sock->cfd = xio_tcp_socket_create();
	if (sock->cfd < 0)
		return -1;

	sock->dfd = xio_tcp_socket_create();
	if (sock->dfd < 0) {
		close(sock->cfd);
		return -1;
	}
	return 0;
}

/*---------------------------------------------------------------------------*/
/* xio_tcp_transport_create		                                     */
/*---------------------------------------------------------------------------*/
struct xio_tcp_transport *xio_tcp_transport_create(
		struct xio_transport	*transport,
		struct xio_context	*ctx,
		struct xio_observer	*observer,
		int			create_socket)
{
	struct xio_tcp_transport	*tcp_hndl;
	int				xio_hdr_size;


	/*allocate tcp handl */
	tcp_hndl = ucalloc(1, sizeof(struct xio_tcp_transport));
	if (!tcp_hndl) {
		xio_set_error(ENOMEM);
		ERROR_LOG("ucalloc failed. %m\n");
		return NULL;
	}

	XIO_OBSERVABLE_INIT(&tcp_hndl->base.observable, tcp_hndl);

	if (tcp_options.enable_mem_pool) {
		tcp_hndl->tcp_mempool =
			xio_transport_mempool_get(ctx, 0);
		if (tcp_hndl->tcp_mempool == NULL) {
			xio_set_error(ENOMEM);
			ERROR_LOG("allocating tcp mempool failed. %m\n");
			goto cleanup;
		}
	}

	tcp_hndl->base.portal_uri	= NULL;
	tcp_hndl->base.proto		= XIO_PROTO_TCP;
	kref_init(&tcp_hndl->base.kref);
	tcp_hndl->transport		= transport;
	tcp_hndl->base.ctx		= ctx;
	tcp_hndl->is_listen		= 0;

	tcp_hndl->tmp_rx_buf		= NULL;
	tcp_hndl->tmp_rx_buf_cur	= NULL;
	tcp_hndl->tmp_rx_buf_len	= 0;

	tcp_hndl->tx_ready_tasks_num = 0;
	tcp_hndl->tx_comp_cnt = 0;

	memset(&tcp_hndl->tmp_work, 0, sizeof(struct xio_tcp_work_req));
	tcp_hndl->tmp_work.msg_iov = tcp_hndl->tmp_iovec;

	xio_hdr_size = xio_tcp_get_max_header_size();
	xio_hdr_size =	ALIGN(xio_hdr_size, 64);


	/* create tcp socket */
	if (create_socket) {
		tcp_hndl->sock.ops = tcp_options.tcp_dual_sock ?
					&dual_sock_ops : &single_sock_ops;
		if (tcp_hndl->sock.ops->open(&tcp_hndl->sock))
			goto cleanup;
	}

	/* from now on don't allow changes */
	tcp_options.tcp_buf_attr_rdonly = 1;
	tcp_hndl->max_send_buf_sz	= tcp_options.tcp_buf_threshold +
					  xio_hdr_size;
	tcp_hndl->max_send_buf_sz	=
				ALIGN(tcp_hndl->max_send_buf_sz, 64);


	tcp_hndl->membuf_sz		= tcp_hndl->max_send_buf_sz;

	if (observer)
		xio_observable_reg_observer(&tcp_hndl->base.observable,
					    observer);

	INIT_LIST_HEAD(&tcp_hndl->in_flight_list);
	INIT_LIST_HEAD(&tcp_hndl->tx_ready_list);
	INIT_LIST_HEAD(&tcp_hndl->tx_comp_list);
	INIT_LIST_HEAD(&tcp_hndl->rx_list);
	INIT_LIST_HEAD(&tcp_hndl->io_list);

	INIT_LIST_HEAD(&tcp_hndl->pending_conns);

	memset(&tcp_hndl->flush_tx_event, 0, sizeof(xio_ctx_event_t));
	xio_ctx_init_event(&tcp_hndl->flush_tx_event,
			   xio_tcp_flush_tx_handler, tcp_hndl);
	memset(&tcp_hndl->ctl_rx_event, 0, sizeof(xio_ctx_event_t));
	xio_ctx_init_event(&tcp_hndl->ctl_rx_event,
			   xio_tcp_consume_ctl_rx, tcp_hndl);
	memset(&tcp_hndl->disconnect_event, 0, sizeof(xio_ctx_event_t));
	xio_ctx_init_event(&tcp_hndl->disconnect_event,
			   xio_tcp_disconnect_handler,
			   tcp_hndl);

	TRACE_LOG("xio_tcp_open: [new] handle:%p\n", tcp_hndl);

	return tcp_hndl;

cleanup:
	ufree(tcp_hndl);

	return NULL;
}

/*---------------------------------------------------------------------------*/
/* xio_tcp_handle_pending_conn						     */
/*---------------------------------------------------------------------------*/
void xio_tcp_handle_pending_conn(int fd,
				 struct xio_tcp_transport *parent_hndl,
				 int error)
{
	int retval;
	struct xio_tcp_pending_conn *pconn, *next_pconn;
	struct xio_tcp_pending_conn *pending_conn = NULL, *matching_conn = NULL;
	struct xio_tcp_pending_conn *ctl_conn = NULL, *data_conn = NULL;
	void *buf;
	int cfd = 0, dfd = 0, is_single = 1;
	socklen_t len = 0;
	struct xio_tcp_transport *child_hndl = NULL;
	union xio_transport_event_data ev_data;

	list_for_each_entry_safe(pconn, next_pconn,
				 &parent_hndl->pending_conns,
				 conns_list_entry) {
		if (pconn->fd == fd) {
			pending_conn = pconn;
			break;
		}
	}

	if (!pending_conn) {
		ERROR_LOG("could not find pending fd [%d] on the list\n", fd);
		goto cleanup2;
	}

	if (error) {
		DEBUG_LOG("epoll returned with error=%d for fd=%d\n",
			  error, fd);
		goto cleanup1;
	}


	buf = &pending_conn->msg;
	buf += sizeof(struct xio_tcp_connect_msg) -
			pending_conn->waiting_for_bytes;
	while (pending_conn->waiting_for_bytes) {
		retval = recv(fd, buf, pending_conn->waiting_for_bytes, 0);
		if (retval > 0) {
			pending_conn->waiting_for_bytes -= retval;
			buf += retval;
		} else if (retval == 0) {
			ERROR_LOG("got EOF while establishing connection\n");
			goto cleanup1;
		} else {
			if (errno != EAGAIN) {
				ERROR_LOG("recv return with errno=%d\n", errno);
				goto cleanup1;
			}
			return;
		}
	}

	UNPACK_LVAL(&pending_conn->msg, &pending_conn->msg, sock_type);
	UNPACK_SVAL(&pending_conn->msg, &pending_conn->msg, second_port);
	UNPACK_SVAL(&pending_conn->msg, &pending_conn->msg, pad);

	if (pending_conn->msg.sock_type == XIO_TCP_SINGLE_SOCK) {
		ctl_conn = pending_conn;
		goto single_sock;
	}

	is_single = 0;

	list_for_each_entry_safe(pconn, next_pconn,
				 &parent_hndl->pending_conns,
				 conns_list_entry) {
		if (pconn->waiting_for_bytes)
			continue;

		if (pconn->sa.sa.sa_family == AF_INET) {
			if ((pconn->msg.second_port ==
			    ntohs(pending_conn->sa.sa_in.sin_port)) &&
			    (pconn->sa.sa_in.sin_addr.s_addr ==
			    pending_conn->sa.sa_in.sin_addr.s_addr)) {
				matching_conn = pconn;
				if (ntohs(matching_conn->sa.sa_in.sin_port) !=
				    pending_conn->msg.second_port) {
					ERROR_LOG("ports mismatch\n");
					return;
				}
				break;
			}
		} else if (pconn->sa.sa.sa_family == AF_INET6) {
			if ((pconn->msg.second_port ==
			     ntohs(pending_conn->sa.sa_in6.sin6_port)) &&
			     !memcmp(&pconn->sa.sa_in6.sin6_addr,
				     &pending_conn->sa.sa_in6.sin6_addr,
				     sizeof(pconn->sa.sa_in6.sin6_addr))) {
				matching_conn = pconn;
				if (ntohs(matching_conn->sa.sa_in6.sin6_port)
				    != pending_conn->msg.second_port) {
					ERROR_LOG("ports mismatch\n");
					return;
				}
				break;
			}
		} else {
			ERROR_LOG("unknown family %d\n",
				  pconn->sa.sa.sa_family);
		}
	}

	if (!matching_conn)
		return;

	if (pending_conn->msg.sock_type == XIO_TCP_CTL_SOCK) {
		ctl_conn = pending_conn;
		data_conn = matching_conn;
	} else if (pending_conn->msg.sock_type == XIO_TCP_DATA_SOCK) {
		ctl_conn = matching_conn;
		data_conn = pending_conn;
	}
	cfd = ctl_conn->fd;
	dfd = data_conn->fd;

	retval = xio_context_del_ev_handler(parent_hndl->base.ctx,
					    data_conn->fd);
	list_del(&data_conn->conns_list_entry);
	if (retval) {
		ERROR_LOG("removing connection handler failed.(errno=%d %m)\n",
			  errno);
	}
	ufree(data_conn);

single_sock:

	list_del(&ctl_conn->conns_list_entry);
	retval = xio_context_del_ev_handler(parent_hndl->base.ctx,
					    ctl_conn->fd);
	if (retval) {
		ERROR_LOG("removing connection handler failed.(errno=%d %m)\n",
			  errno);
	}

	child_hndl = xio_tcp_transport_create(parent_hndl->transport,
					      parent_hndl->base.ctx,
					      NULL,
					      0);
	if (!child_hndl) {
		ERROR_LOG("failed to create tcp child\n");
		xio_transport_notify_observer_error(&parent_hndl->base,
						    xio_errno());
		ufree(ctl_conn);
		goto cleanup3;
	}

	memcpy(&child_hndl->base.peer_addr,
	       &ctl_conn->sa.sa_stor,
	       sizeof(child_hndl->base.peer_addr));
	ufree(ctl_conn);

	if (is_single) {
		child_hndl->sock.cfd = fd;
		child_hndl->sock.dfd = fd;
		child_hndl->sock.ops = &single_sock_ops;

	} else {
		child_hndl->sock.cfd = cfd;
		child_hndl->sock.dfd = dfd;
		child_hndl->sock.ops = &dual_sock_ops;

		child_hndl->tmp_rx_buf = ucalloc(1, TMP_RX_BUF_SIZE);
		if (!child_hndl->tmp_rx_buf) {
			xio_set_error(ENOMEM);
			ERROR_LOG("ucalloc failed. %m\n");
			goto cleanup3;
		}
		child_hndl->tmp_rx_buf_cur = child_hndl->tmp_rx_buf;
	}


	len = sizeof(child_hndl->base.local_addr);
	retval = getsockname(child_hndl->sock.cfd,
			     (struct sockaddr *)&child_hndl->base.local_addr,
			     &len);
	if (retval) {
		xio_set_error(errno);
		ERROR_LOG("tcp getsockname failed. (errno=%d %m)\n", errno);
	}

	ev_data.new_connection.child_trans_hndl =
		(struct xio_transport_base *)child_hndl;
	xio_transport_notify_observer((struct xio_transport_base *)parent_hndl,
				      XIO_TRANSPORT_NEW_CONNECTION,
				      &ev_data);

	return;

cleanup1:
	list_del(&pending_conn->conns_list_entry);
	ufree(pending_conn);
cleanup2:
	/* remove from epoll */
	retval = xio_context_del_ev_handler(parent_hndl->base.ctx, fd);
	if (retval) {
		ERROR_LOG(
		"removing connection handler failed.(errno=%d %m)\n",
		errno);
	}
cleanup3:
	if (is_single) {
		close(fd);
	} else {
		close(cfd);
		close(dfd);
	}

	if (child_hndl)
		xio_tcp_post_close(child_hndl);
}

/*---------------------------------------------------------------------------*/
/* xio_tcp_pending_conn_ev_handler					     */
/*---------------------------------------------------------------------------*/
void xio_tcp_pending_conn_ev_handler(int fd, int events, void *user_context)
{
	struct xio_tcp_transport *tcp_hndl = user_context;

	xio_tcp_handle_pending_conn(fd, tcp_hndl,
				    events &
				    (EPOLLHUP | EPOLLRDHUP | EPOLLERR));
}

/*---------------------------------------------------------------------------*/
/* xio_tcp_new_connection						     */
/*---------------------------------------------------------------------------*/
void xio_tcp_new_connection(struct xio_tcp_transport *parent_hndl)
{
	int retval;
	socklen_t len = sizeof(struct sockaddr_storage);
	struct xio_tcp_pending_conn *pending_conn;

	/*allocate pending fd struct */
	pending_conn = ucalloc(1, sizeof(struct xio_tcp_pending_conn));
	if (!pending_conn) {
		xio_set_error(ENOMEM);
		ERROR_LOG("ucalloc failed. %m\n");
		xio_transport_notify_observer_error(&parent_hndl->base,
						    xio_errno());
		return;
	}

	pending_conn->waiting_for_bytes = sizeof(struct xio_tcp_connect_msg);

	/* "accept" the connection */
	retval = accept4(parent_hndl->sock.cfd,
			 (struct sockaddr *)&pending_conn->sa.sa_stor,
			 &len,
			 SOCK_NONBLOCK);
	if (retval < 0) {
		xio_set_error(errno);
		ERROR_LOG("tcp accept failed. (errno=%d %m)\n", errno);
		ufree(pending_conn);
		return;
	}
	pending_conn->fd = retval;

	list_add_tail(&pending_conn->conns_list_entry,
		      &parent_hndl->pending_conns);

	/* add to epoll */
	retval = xio_context_add_ev_handler(
			parent_hndl->base.ctx,
			pending_conn->fd,
			XIO_POLLIN | XIO_POLLRDHUP,
			xio_tcp_pending_conn_ev_handler,
			parent_hndl);
	if (retval)
		ERROR_LOG("adding pending_conn_ev_handler failed\n");
}

/*---------------------------------------------------------------------------*/
/* xio_tcp_listener_ev_handler						     */
/*---------------------------------------------------------------------------*/
void xio_tcp_listener_ev_handler(int fd, int events, void *user_context)
{
	struct xio_tcp_transport *tcp_hndl = user_context;

	if (events & EPOLLIN)
		xio_tcp_new_connection(tcp_hndl);

	if ((events & (EPOLLHUP | EPOLLERR))) {
		DEBUG_LOG("epoll returned with error events=%d for fd=%d\n",
			  events, fd);
		xio_tcp_disconnect_helper(tcp_hndl);
	}
}

/*---------------------------------------------------------------------------*/
/* xio_tcp_listen							     */
/*---------------------------------------------------------------------------*/
static int xio_tcp_listen(struct xio_transport_base *transport,
			  const char *portal_uri, uint16_t *src_port,
			  int backlog)
{
	struct xio_tcp_transport *tcp_hndl =
		(struct xio_tcp_transport *)transport;
	union xio_sockaddr	sa;
	int			sa_len;
	int			retval = 0;
	uint16_t		sport;

	/* resolve the portal_uri */
	sa_len = xio_uri_to_ss(portal_uri, &sa.sa_stor);
	if (sa_len == -1) {
		xio_set_error(XIO_E_ADDR_ERROR);
		ERROR_LOG("address [%s] resolving failed\n", portal_uri);
		return -1;
	}
	tcp_hndl->base.is_client = 0;

	/* bind */
	retval = bind(tcp_hndl->sock.cfd,
		      (struct sockaddr *)&sa.sa_stor,
		      sa_len);
	if (retval) {
		xio_set_error(errno);
		ERROR_LOG("tcp bind failed. (errno=%d %m)\n", errno);
		goto exit;
	}

	tcp_hndl->is_listen = 1;

	retval  = listen(tcp_hndl->sock.cfd,
			 backlog > 0 ? backlog : MAX_BACKLOG);
	if (retval) {
		xio_set_error(errno);
		ERROR_LOG("tcp listen failed. (errno=%d %m)\n", errno);
		goto exit;
	}

	/* add to epoll */
	retval = xio_context_add_ev_handler(
			tcp_hndl->base.ctx,
			tcp_hndl->sock.cfd,
			XIO_POLLIN,
			xio_tcp_listener_ev_handler,
			tcp_hndl);

	retval  = getsockname(tcp_hndl->sock.cfd,
			      (struct sockaddr *)&sa.sa_stor,
			      (socklen_t *)&sa_len);
	if (retval) {
		xio_set_error(errno);
		ERROR_LOG("getsockname failed. (errno=%d %m)\n", errno);
		goto exit;
	}

	switch (sa.sa_stor.ss_family) {
	case AF_INET:
		sport = ntohs(sa.sa_in.sin_port);
		break;
	case AF_INET6:
		sport = ntohs(sa.sa_in6.sin6_port);
		break;
	default:
		xio_set_error(XIO_E_ADDR_ERROR);
		ERROR_LOG("invalid family type %d.\n", sa.sa_stor.ss_family);
		goto exit;
	}

	if (src_port)
		*src_port = sport;

	tcp_hndl->state = XIO_STATE_LISTEN;
	DEBUG_LOG("listen on [%s] src_port:%d\n", portal_uri, sport);

	return 0;

exit:
	return -1;
}

/*---------------------------------------------------------------------------*/
/* xio_tcp_conn_established_helper	                                     */
/*---------------------------------------------------------------------------*/
void xio_tcp_conn_established_helper(int fd,
				     struct xio_tcp_transport *tcp_hndl,
				     struct xio_tcp_connect_msg	*msg,
				     int error)
{
	int				retval = 0;
	int				so_error = 0;
	socklen_t			len = sizeof(so_error);

	/* remove from epoll */
	retval = xio_context_del_ev_handler(tcp_hndl->base.ctx,
					    tcp_hndl->sock.cfd);
	if (retval) {
		ERROR_LOG("removing connection handler failed.(errno=%d %m)\n",
			  errno);
		goto cleanup;
	}

	retval = getsockopt(tcp_hndl->sock.cfd,
			    SOL_SOCKET,
			    SO_ERROR,
			    &so_error,
			    &len);
	if (retval) {
		ERROR_LOG("getsockopt failed. (errno=%d %m)\n", errno);
		so_error = errno;
	}
	if (so_error || error) {
		DEBUG_LOG("fd=%d connection establishment failed\n",
			  tcp_hndl->sock.cfd);
		DEBUG_LOG("so_error=%d, epoll_error=%d\n", so_error, error);
		tcp_hndl->sock.ops->del_ev_handlers = NULL;
		goto cleanup;
	}

	/* add to epoll */
	retval = tcp_hndl->sock.ops->add_ev_handlers(tcp_hndl);
	if (retval) {
		ERROR_LOG("setting connection handler failed. (errno=%d %m)\n",
			  errno);
		goto cleanup;
	}

	len = sizeof(tcp_hndl->base.peer_addr);
	retval = getpeername(tcp_hndl->sock.cfd,
			     (struct sockaddr *)&tcp_hndl->base.peer_addr,
			     &len);
	if (retval) {
		xio_set_error(errno);
		ERROR_LOG("tcp getpeername failed. (errno=%d %m)\n", errno);
		so_error = errno;
		goto cleanup;
	}

	retval = xio_tcp_send_connect_msg(tcp_hndl->sock.cfd, msg);
	if (retval)
		goto cleanup;

	xio_transport_notify_observer(&tcp_hndl->base,
				      XIO_TRANSPORT_ESTABLISHED,
				      NULL);

	return;

cleanup:
	xio_transport_notify_observer_error(&tcp_hndl->base,
					    so_error ? so_error :
					    XIO_E_CONNECT_ERROR);
}

/*---------------------------------------------------------------------------*/
/* xio_tcp_single_conn_established_ev_handler	                             */
/*---------------------------------------------------------------------------*/
void xio_tcp_single_conn_established_ev_handler(int fd,
						int events, void *user_context)
{
	struct xio_tcp_transport	*tcp_hndl = user_context;
	struct xio_tcp_connect_msg	msg;
	msg.sock_type = XIO_TCP_SINGLE_SOCK;
	msg.second_port = 0;
	msg.pad = 0;
	xio_tcp_conn_established_helper(fd, tcp_hndl, &msg,
					events &
					(EPOLLERR | EPOLLHUP | EPOLLRDHUP));
}

/*---------------------------------------------------------------------------*/
/* xio_tcp_cfd_conn_established_ev_handler	                             */
/*---------------------------------------------------------------------------*/
void xio_tcp_cfd_conn_established_ev_handler(int fd,
					     int events, void *user_context)
{
	struct xio_tcp_transport	*tcp_hndl = user_context;
	struct xio_tcp_connect_msg	msg;
	msg.sock_type = XIO_TCP_CTL_SOCK;
	msg.second_port = tcp_hndl->sock.port_dfd;
	msg.pad = 0;
	xio_tcp_conn_established_helper(fd, tcp_hndl, &msg,
					events &
					(EPOLLERR | EPOLLHUP | EPOLLRDHUP));
}

/*---------------------------------------------------------------------------*/
/* xio_tcp_dfd_conn_established_ev_handler	                             */
/*---------------------------------------------------------------------------*/
void xio_tcp_dfd_conn_established_ev_handler(int fd,
					     int events, void *user_context)
{
	struct xio_tcp_transport	*tcp_hndl = user_context;
	int				retval = 0;
	int				so_error = 0;
	socklen_t			so_error_len = sizeof(so_error);
	struct xio_tcp_connect_msg	msg;

	/* remove from epoll */
	retval = xio_context_del_ev_handler(tcp_hndl->base.ctx,
					    tcp_hndl->sock.dfd);
	if (retval) {
		ERROR_LOG("removing connection handler failed.(errno=%d %m)\n",
			  errno);
		goto cleanup;
	}

	retval = getsockopt(tcp_hndl->sock.dfd,
			    SOL_SOCKET,
			    SO_ERROR,
			    &so_error,
			    &so_error_len);
	if (retval) {
		ERROR_LOG("getsockopt failed. (errno=%d %m)\n", errno);
		so_error = errno;
	}
	if (so_error || (events & (EPOLLERR | EPOLLHUP | EPOLLRDHUP))) {
		DEBUG_LOG("fd=%d connection establishment failed\n",
			  tcp_hndl->sock.dfd);
		DEBUG_LOG("so_error=%d, epoll_events=%d\n", so_error, events);
		tcp_hndl->sock.ops->del_ev_handlers = NULL;
		goto cleanup;
	}

	/* add to epoll */
	retval = xio_context_add_ev_handler(
			tcp_hndl->base.ctx,
			tcp_hndl->sock.cfd,
			XIO_POLLOUT | XIO_POLLRDHUP,
			xio_tcp_cfd_conn_established_ev_handler,
			tcp_hndl);
	if (retval) {
		ERROR_LOG("setting connection handler failed. (errno=%d %m)\n",
			  errno);
		goto cleanup;
	}

	msg.sock_type = XIO_TCP_DATA_SOCK;
	msg.second_port = tcp_hndl->sock.port_cfd;
	msg.pad = 0;
	retval = xio_tcp_send_connect_msg(tcp_hndl->sock.dfd, &
			msg);
	if (retval)
		goto cleanup;

	return;

cleanup:
	xio_transport_notify_observer_error(&tcp_hndl->base,
					    so_error ? so_error :
					    XIO_E_CONNECT_ERROR);
}

/*---------------------------------------------------------------------------*/
/* xio_tcp_connect_helper	                                             */
/*---------------------------------------------------------------------------*/
static int xio_tcp_connect_helper(int fd, struct sockaddr *sa,
				  socklen_t sa_len, uint16_t *bound_port,
				  struct sockaddr_storage *lss)
{
	int retval;
	union xio_sockaddr *lsa = (union xio_sockaddr *)lss;
	struct sockaddr_storage sa_stor;
	socklen_t lsa_len = sizeof(struct sockaddr_storage);

	retval = connect(fd, sa, sa_len);
	if (retval) {
		if (errno == EINPROGRESS) {
			/*set iomux for write event*/
		} else {
			xio_set_error(errno);
			ERROR_LOG("tcp connect failed. (errno=%d %m)\n", errno);
			return retval;
		}
	} else {
		/*handle in ev_handler*/
	}

	if (!lss)
		lsa = (union xio_sockaddr *)&sa_stor;

	retval = getsockname(fd, &lsa->sa, &lsa_len);
	if (retval) {
		xio_set_error(errno);
		ERROR_LOG("tcp getsockname failed. (errno=%d %m)\n", errno);
		return retval;
	}

	if (lsa->sa.sa_family == AF_INET) {
		*bound_port = ntohs(lsa->sa_in.sin_port);
	} else if (lsa->sa.sa_family == AF_INET6) {
		*bound_port = ntohs(lsa->sa_in6.sin6_port);
	} else {
		ERROR_LOG("getsockname unknown family = %d\n",
			  lsa->sa.sa_family);
		return -1;
	}

	return 0;
}

/*---------------------------------------------------------------------------*/
/* xio_tcp_single_sock_connect	                                             */
/*---------------------------------------------------------------------------*/
int xio_tcp_single_sock_connect(struct xio_tcp_transport *tcp_hndl,
				struct sockaddr *sa,
				socklen_t sa_len)
{
	int retval;

	retval = xio_tcp_connect_helper(tcp_hndl->sock.cfd, sa, sa_len,
					&tcp_hndl->sock.port_cfd,
					&tcp_hndl->base.local_addr);
	if (retval)
		return retval;

	/* add to epoll */
	retval = xio_context_add_ev_handler(
			tcp_hndl->base.ctx,
			tcp_hndl->sock.cfd,
			XIO_POLLOUT | XIO_POLLRDHUP,
			xio_tcp_single_conn_established_ev_handler,
			tcp_hndl);
	if (retval) {
		ERROR_LOG("setting connection handler failed. (errno=%d %m)\n",
			  errno);
		return retval;
	}

	return 0;
}

/*---------------------------------------------------------------------------*/
/* xio_tcp_dual_sock_connect	                                             */
/*---------------------------------------------------------------------------*/
int xio_tcp_dual_sock_connect(struct xio_tcp_transport *tcp_hndl,
			      struct sockaddr *sa,
			      socklen_t sa_len)
{
	int retval;

	tcp_hndl->tmp_rx_buf = ucalloc(1, TMP_RX_BUF_SIZE);
	if (!tcp_hndl->tmp_rx_buf) {
		xio_set_error(ENOMEM);
		ERROR_LOG("ucalloc failed. %m\n");
		return -1;
	}
	tcp_hndl->tmp_rx_buf_cur = tcp_hndl->tmp_rx_buf;

	retval = xio_tcp_connect_helper(tcp_hndl->sock.cfd, sa, sa_len,
					&tcp_hndl->sock.port_cfd,
					&tcp_hndl->base.local_addr);
	if (retval)
		return retval;

	retval = xio_tcp_connect_helper(tcp_hndl->sock.dfd, sa, sa_len,
					&tcp_hndl->sock.port_dfd,
					NULL);
	if (retval)
		return retval;

	/* add to epoll */
	retval = xio_context_add_ev_handler(
			tcp_hndl->base.ctx,
			tcp_hndl->sock.dfd,
			XIO_POLLOUT | XIO_POLLRDHUP,
			xio_tcp_dfd_conn_established_ev_handler,
			tcp_hndl);
	if (retval) {
		ERROR_LOG("setting connection handler failed. (errno=%d %m)\n",
			  errno);
		return retval;
	}

	return 0;
}

/*---------------------------------------------------------------------------*/
/* xio_tcp_connect		                                             */
/*---------------------------------------------------------------------------*/
static int xio_tcp_connect(struct xio_transport_base *transport,
			   const char *portal_uri, const char *out_if_addr)
{
	struct xio_tcp_transport	*tcp_hndl =
					(struct xio_tcp_transport *)transport;
	union xio_sockaddr		rsa;
	socklen_t			rsa_len = 0;
	int				retval = 0;

	/* resolve the portal_uri */
	rsa_len = xio_uri_to_ss(portal_uri, &rsa.sa_stor);
	if (rsa_len == (socklen_t)-1) {
		xio_set_error(XIO_E_ADDR_ERROR);
		ERROR_LOG("address [%s] resolving failed\n", portal_uri);
		return -1;
	}
	/* allocate memory for portal_uri */
	tcp_hndl->base.portal_uri = strdup(portal_uri);
	if (tcp_hndl->base.portal_uri == NULL) {
		xio_set_error(ENOMEM);
		ERROR_LOG("strdup failed. %m\n");
		return -1;
	}
	tcp_hndl->base.is_client = 1;

	if (out_if_addr) {
		union xio_sockaddr	if_sa;
		int			sa_len;

		sa_len = xio_host_port_to_ss(out_if_addr, &if_sa.sa_stor);
		if (sa_len == -1) {
			xio_set_error(XIO_E_ADDR_ERROR);
			ERROR_LOG("outgoing interface [%s] resolving failed\n",
				  out_if_addr);
			goto exit;
		}
		retval = bind(tcp_hndl->sock.cfd,
			      (struct sockaddr *)&if_sa.sa_stor,
			      sa_len);
		if (retval) {
			xio_set_error(errno);
			ERROR_LOG("tcp bind failed. (errno=%d %m)\n",
				  errno);
			goto exit;
		}
	}

	/* connect */
	retval = tcp_hndl->sock.ops->connect(tcp_hndl,
					     (struct sockaddr *)&rsa.sa_stor,
					     rsa_len);
	if (retval)
		goto exit;

	return 0;

exit:
	ufree(tcp_hndl->base.portal_uri);

	return -1;
}

/*---------------------------------------------------------------------------*/
/* xio_tcp_open								     */
/*---------------------------------------------------------------------------*/
static struct xio_transport_base *xio_tcp_open(
		struct xio_transport	*transport,
		struct xio_context	*ctx,
		struct xio_observer	*observer)
{
	struct xio_tcp_transport	*tcp_hndl;

	tcp_hndl = xio_tcp_transport_create(transport, ctx, observer, 1);
	if (!tcp_hndl) {
		ERROR_LOG("failed. to create tcp transport%m\n");
		return NULL;
	}
	return (struct xio_transport_base *)tcp_hndl;
}

/*
 * To dynamically control C-states, open the file /dev/cpu_dma_latency and
 * write the maximum allowable latency to it. This will prevent C-states with
 * transition latencies higher than the specified value from being used, as
 * long as the file /dev/cpu_dma_latency is kept open.
 * Writing a maximum allowable latency of 0 will keep the processors in C0
 * (like using kernel parameter ―idle=poll), and writing 1 should force
 * the processors to C1 when idle. Higher values could also be written to
 * restrict the use of C-states with latency greater than the value written.
 *
 * http://en.community.dell.com/techcenter/extras/m/white_papers/20227764/download.aspx
 */

/*---------------------------------------------------------------------------*/
/* xio_set_cpu_latency							     */
/*---------------------------------------------------------------------------*/
static int xio_set_cpu_latency(int *fd)
{
	int32_t latency = 0;

	if (!tcp_options.enable_dma_latency)
		return 0;

	DEBUG_LOG("setting latency to %d us\n", latency);
	*fd = open("/dev/cpu_dma_latency", O_WRONLY);
	if (*fd < 0) {
		ERROR_LOG(
		 "open /dev/cpu_dma_latency %m - need root permissions\n");
		return -1;
	}
	if (write(*fd, &latency, sizeof(latency)) != sizeof(latency)) {
		ERROR_LOG(
		 "write to /dev/cpu_dma_latency %m - need root permissions\n");
		close(*fd);
		*fd = -1;
		return -1;
	}
	return 0;
}

/*---------------------------------------------------------------------------*/
/* xio_tcp_init							     */
/*---------------------------------------------------------------------------*/
static void xio_tcp_init(void)
{
	spin_lock_init(&mngmt_lock);

	/* set cpu latency until process is down */
	xio_set_cpu_latency(&cdl_fd);
}

/*---------------------------------------------------------------------------*/
/* xio_tcp_transport_init						     */
/*---------------------------------------------------------------------------*/
static int xio_tcp_transport_init(struct xio_transport *transport)
{
	pthread_once(&ctor_key_once, xio_tcp_init);

	return 0;
}

/*---------------------------------------------------------------------------*/
/* xio_tcp_release							     */
/*---------------------------------------------------------------------------*/
static void xio_tcp_release(void)
{
	if (cdl_fd >= 0)
		close(cdl_fd);

	/*ORK todo close everything? see xio_cq_release*/
}

/*---------------------------------------------------------------------------*/
/* xio_tcp_transport_constructor					     */
/*---------------------------------------------------------------------------*/
void xio_tcp_transport_constructor(void)
{
}

/*---------------------------------------------------------------------------*/
/* xio_tcp_transport_destructor					     */
/*---------------------------------------------------------------------------*/
void xio_tcp_transport_destructor(void)
{
	ctor_key_once = PTHREAD_ONCE_INIT;
	dtor_key_once = PTHREAD_ONCE_INIT;
}

/*---------------------------------------------------------------------------*/
/* xio_tcp_transport_release		                                     */
/*---------------------------------------------------------------------------*/
static void xio_tcp_transport_release(struct xio_transport *transport)
{
	if (ctor_key_once == PTHREAD_ONCE_INIT)
		return;

	pthread_once(&dtor_key_once, xio_tcp_release);
}

/*---------------------------------------------------------------------------*/
/* xio_tcp_rxd_init							     */
/*---------------------------------------------------------------------------*/
static void xio_tcp_rxd_init(struct xio_tcp_work_req *rxd,
			     void *buf, unsigned size)
{
	rxd->msg_iov[0].iov_base = buf;
	rxd->msg_iov[0].iov_len	= sizeof(struct xio_tlv);
	rxd->msg_iov[1].iov_base = rxd->msg_iov[0].iov_base +
				   rxd->msg_iov[0].iov_len;
	rxd->msg_iov[1].iov_len	= size - sizeof(struct xio_tlv);
	rxd->msg_len = 2;

	rxd->tot_iov_byte_len = 0;

	rxd->stage = XIO_TCP_RX_START;
	rxd->msg.msg_control = NULL;
	rxd->msg.msg_controllen = 0;
	rxd->msg.msg_flags = 0;
	rxd->msg.msg_name = NULL;
	rxd->msg.msg_namelen = 0;
	rxd->msg.msg_iov = NULL;
	rxd->msg.msg_iovlen = 0;
}

/*---------------------------------------------------------------------------*/
/* xio_tcp_txd_init							     */
/*---------------------------------------------------------------------------*/
static void xio_tcp_txd_init(struct xio_tcp_work_req *txd,
			     void *buf, unsigned size)
{
	txd->ctl_msg = buf;
	txd->ctl_msg_len = 0;
	txd->msg_iov[0].iov_base = buf;
	txd->msg_iov[0].iov_len	= size;
	txd->msg_len = 1;
	txd->tot_iov_byte_len = 0;

	txd->stage = XIO_TCP_TX_BEFORE;
	txd->msg.msg_control = NULL;
	txd->msg.msg_controllen = 0;
	txd->msg.msg_flags = 0;
	txd->msg.msg_name = NULL;
	txd->msg.msg_namelen = 0;
	txd->msg.msg_iov = NULL;
	txd->msg.msg_iovlen = 0;
}

/*---------------------------------------------------------------------------*/
/* xio_tcp_task_init							     */
/*---------------------------------------------------------------------------*/
static void xio_tcp_task_init(struct xio_task *task,
			      struct xio_tcp_transport *tcp_hndl,
			      void *buf,
			      unsigned long size)
{
	XIO_TO_TCP_TASK(task, tcp_task);

	tcp_task->tcp_hndl = tcp_hndl;

	xio_tcp_rxd_init(&tcp_task->rxd, buf, size);
	xio_tcp_txd_init(&tcp_task->txd, buf, size);

	/* initialize the mbuf */
	xio_mbuf_init(&task->mbuf, buf, size, 0);
}

/* task pools management */
/*---------------------------------------------------------------------------*/
/* xio_tcp_initial_pool_slab_pre_create					     */
/*---------------------------------------------------------------------------*/
static int xio_tcp_initial_pool_slab_pre_create(
		struct xio_transport_base *transport_hndl,
		int alloc_nr,
		void *pool_dd_data, void *slab_dd_data)
{
	struct xio_tcp_tasks_slab *tcp_slab =
		(struct xio_tcp_tasks_slab *)slab_dd_data;
	uint32_t pool_size;

	tcp_slab->buf_size = CONN_SETUP_BUF_SIZE;
	pool_size = tcp_slab->buf_size * alloc_nr;

	tcp_slab->data_pool = ucalloc(pool_size * alloc_nr, sizeof(uint8_t));
	if (tcp_slab->data_pool == NULL) {
		xio_set_error(ENOMEM);
		ERROR_LOG("ucalloc conn_setup_data_pool sz: %u failed\n",
			  pool_size);
		return -1;
	}

	return 0;
}

/*---------------------------------------------------------------------------*/
/* xio_tcp_initial_task_alloc						     */
/*---------------------------------------------------------------------------*/
static inline struct xio_task *xio_tcp_initial_task_alloc(
					struct xio_tcp_transport *tcp_hndl)
{
	if (tcp_hndl->initial_pool_cls.task_get) {
		return tcp_hndl->initial_pool_cls.task_get(
					tcp_hndl->initial_pool_cls.pool);
	}
	return NULL;
}

/*---------------------------------------------------------------------------*/
/* xio_tcp_primary_task_alloc						     */
/*---------------------------------------------------------------------------*/
struct xio_task *xio_tcp_primary_task_alloc(
					struct xio_tcp_transport *tcp_hndl)
{
	if (tcp_hndl->primary_pool_cls.task_get)
		return tcp_hndl->primary_pool_cls.task_get(
					tcp_hndl->primary_pool_cls.pool);
	return NULL;
}

/*---------------------------------------------------------------------------*/
/* xio_tcp_primary_task_lookup						     */
/*---------------------------------------------------------------------------*/
struct xio_task *xio_tcp_primary_task_lookup(
					struct xio_tcp_transport *tcp_hndl,
					int tid)
{
	if (tcp_hndl->primary_pool_cls.task_lookup)
		return tcp_hndl->primary_pool_cls.task_lookup(
					tcp_hndl->primary_pool_cls.pool, tid);
	return NULL;
}

/*---------------------------------------------------------------------------*/
/* xio_tcp_task_free							     */
/*---------------------------------------------------------------------------*/
inline void xio_tcp_task_free(struct xio_tcp_transport *tcp_hndl,
			       struct xio_task *task)
{
	if (tcp_hndl->primary_pool_cls.task_put)
		return tcp_hndl->primary_pool_cls.task_put(task);
}

/*---------------------------------------------------------------------------*/
/* xio_tcp_initial_pool_post_create					     */
/*---------------------------------------------------------------------------*/
static int xio_tcp_initial_pool_post_create(
		struct xio_transport_base *transport_hndl,
		void *pool, void *pool_dd_data)
{
	struct xio_task *task;
	struct xio_tcp_task *tcp_task;
	struct xio_tcp_transport *tcp_hndl =
		(struct xio_tcp_transport *)transport_hndl;

	tcp_hndl->initial_pool_cls.pool = pool;

	task = xio_tcp_initial_task_alloc(tcp_hndl);
	if (task == NULL) {
		ERROR_LOG("failed to get task\n");
	} else {
		list_add_tail(&task->tasks_list_entry, &tcp_hndl->rx_list);
		tcp_task = (struct xio_tcp_task *)task->dd_data;
		tcp_task->tcp_op = XIO_TCP_RECV;
	}

	return 0;
}

/*---------------------------------------------------------------------------*/
/* xio_tcp_initial_pool_slab_destroy					     */
/*---------------------------------------------------------------------------*/
static int xio_tcp_initial_pool_slab_destroy(
		struct xio_transport_base *transport_hndl,
		void *pool_dd_data, void *slab_dd_data)
{
	struct xio_tcp_tasks_slab *tcp_slab =
		(struct xio_tcp_tasks_slab *)slab_dd_data;

	ufree(tcp_slab->data_pool);

	return 0;
}

/*---------------------------------------------------------------------------*/
/* xio_tcp_initial_pool_slab_init_task					     */
/*---------------------------------------------------------------------------*/
static int xio_tcp_initial_pool_slab_init_task(
		struct xio_transport_base *transport_hndl,
		void *pool_dd_data, void *slab_dd_data,
		int tid, struct xio_task *task)
{
	struct xio_tcp_transport *tcp_hndl =
		(struct xio_tcp_transport *)transport_hndl;
	struct xio_tcp_tasks_slab *tcp_slab =
		(struct xio_tcp_tasks_slab *)slab_dd_data;
	void *buf = tcp_slab->data_pool + tid*tcp_slab->buf_size;
	char *ptr;

	XIO_TO_TCP_TASK(task, tcp_task);

	/* fill xio_tcp_task */
	ptr = (char *)tcp_task;
	ptr += sizeof(struct xio_tcp_task);

	/* fill xio_tcp_work_req */
	tcp_task->txd.msg_iov = (void *)ptr;
	ptr += sizeof(struct iovec);

	tcp_task->rxd.msg_iov = (void *)ptr;
	ptr += 2 * sizeof(struct iovec);
	/*****************************************/

	xio_tcp_task_init(
			task,
			tcp_hndl,
			buf,
			tcp_slab->buf_size);

	return 0;
}

/*---------------------------------------------------------------------------*/
/* xio_tcp_initial_pool_get_params					     */
/*---------------------------------------------------------------------------*/
static void xio_tcp_initial_pool_get_params(
		struct xio_transport_base *transport_hndl,
		int *start_nr, int *max_nr, int *alloc_nr,
		int *pool_dd_sz, int *slab_dd_sz, int *task_dd_sz)
{
	*start_nr = NUM_CONN_SETUP_TASKS;
	*alloc_nr = 0;
	*max_nr = NUM_CONN_SETUP_TASKS;
	*pool_dd_sz = 0;
	*slab_dd_sz = sizeof(struct xio_tcp_tasks_slab);
	*task_dd_sz = sizeof(struct xio_tcp_task) +
			      3*sizeof(struct iovec);
}

static struct xio_tasks_pool_ops initial_tasks_pool_ops = {
	.pool_get_params	= xio_tcp_initial_pool_get_params,
	.slab_pre_create	= xio_tcp_initial_pool_slab_pre_create,
	.slab_destroy		= xio_tcp_initial_pool_slab_destroy,
	.slab_init_task		= xio_tcp_initial_pool_slab_init_task,
	.pool_post_create	= xio_tcp_initial_pool_post_create
};


/*---------------------------------------------------------------------------*/
/* xio_tcp_primary_pool_slab_pre_create				     */
/*---------------------------------------------------------------------------*/
static int xio_tcp_primary_pool_slab_pre_create(
		struct xio_transport_base *transport_hndl,
		int alloc_nr, void *pool_dd_data, void *slab_dd_data)
{
	struct xio_tcp_transport *tcp_hndl =
		(struct xio_tcp_transport *)transport_hndl;
	struct xio_tcp_tasks_slab *tcp_slab =
		(struct xio_tcp_tasks_slab *)slab_dd_data;
	size_t	alloc_sz = alloc_nr*tcp_hndl->membuf_sz;

	tcp_slab->buf_size = tcp_hndl->membuf_sz;

	if (disable_huge_pages) {
		tcp_slab->io_buf = xio_alloc(alloc_sz);
		if (!tcp_slab->io_buf) {
			xio_set_error(ENOMEM);
			ERROR_LOG("xio_alloc tcp pool sz:%zu failed\n",
				  alloc_sz);
			return -1;
		}
		tcp_slab->data_pool = tcp_slab->io_buf->addr;
	} else {
		/* maybe allocation of with unuma_alloc can provide better
		 * performance?
		 */
		tcp_slab->data_pool = umalloc_huge_pages(alloc_sz);
		if (!tcp_slab->data_pool) {
			xio_set_error(ENOMEM);
			ERROR_LOG("malloc tcp pool sz:%zu failed\n",
				  alloc_sz);
			return -1;
		}
	}

	DEBUG_LOG("pool buf:%p\n", tcp_slab->data_pool);

	return 0;
}

/*---------------------------------------------------------------------------*/
/* xio_tcp_primary_pool_post_create					     */
/*---------------------------------------------------------------------------*/
static int xio_tcp_primary_pool_post_create(
		struct xio_transport_base *transport_hndl,
		void *pool, void *pool_dd_data)
{
	struct xio_task		*task = NULL;
	struct xio_tcp_task	*tcp_task = NULL;
	int			i;
	struct xio_tcp_transport *tcp_hndl =
		(struct xio_tcp_transport *)transport_hndl;

	tcp_hndl->primary_pool_cls.pool = pool;

	for (i = 0; i < RX_LIST_POST_NR; i++) {
		/* get ready to receive message */
		task = xio_tcp_primary_task_alloc(tcp_hndl);
		if (task == 0) {
			ERROR_LOG("primary task pool is empty\n");
			return -1;
		}
		tcp_task = task->dd_data;
		tcp_task->tcp_op = XIO_TCP_RECV;
		list_add_tail(&task->tasks_list_entry, &tcp_hndl->rx_list);
	}

	return 0;
}

/*---------------------------------------------------------------------------*/
/* xio_tcp_primary_pool_slab_destroy					     */
/*---------------------------------------------------------------------------*/
static int xio_tcp_primary_pool_slab_destroy(
		struct xio_transport_base *transport_hndl,
		void *pool_dd_data, void *slab_dd_data)
{
	struct xio_tcp_tasks_slab *tcp_slab =
		(struct xio_tcp_tasks_slab *)slab_dd_data;

	if (tcp_slab->io_buf)
		xio_free(&tcp_slab->io_buf);
	else
		ufree_huge_pages(tcp_slab->data_pool);


	return 0;
}

/*---------------------------------------------------------------------------*/
/* xio_tcp_primary_pool_slab_init_task					     */
/*---------------------------------------------------------------------------*/
static int xio_tcp_primary_pool_slab_init_task(
		struct xio_transport_base *transport_hndl,
		void *pool_dd_data,
		void *slab_dd_data, int tid, struct xio_task *task)
{
	struct xio_tcp_transport *tcp_hndl =
		(struct xio_tcp_transport *)transport_hndl;
	struct xio_tcp_tasks_slab *tcp_slab =
		(struct xio_tcp_tasks_slab *)slab_dd_data;
	void *buf = tcp_slab->data_pool + tid*tcp_slab->buf_size;
	int  max_iovsz = max(tcp_options.max_out_iovsz,
				     tcp_options.max_in_iovsz) + 1;
	char *ptr;

	XIO_TO_TCP_TASK(task, tcp_task);

	/* fill xio_tco_task */
	ptr = (char *)tcp_task;
	ptr += sizeof(struct xio_tcp_task);

	/* fill xio_tcp_work_req */
	tcp_task->txd.msg_iov = (void *)ptr;
	ptr += (max_iovsz + 1)*sizeof(struct iovec);
	tcp_task->rxd.msg_iov = (void *)ptr;
	ptr += (max_iovsz + 1)*sizeof(struct iovec);

	tcp_task->read_sge = (void *)ptr;
	ptr += max_iovsz*sizeof(struct xio_mempool_obj);
	tcp_task->write_sge = (void *)ptr;
	ptr += max_iovsz*sizeof(struct xio_mempool_obj);

	tcp_task->req_read_sge = (void *)ptr;
	ptr += max_iovsz*sizeof(struct xio_sge);
	tcp_task->req_write_sge = (void *)ptr;
	ptr += max_iovsz*sizeof(struct xio_sge);
	tcp_task->req_recv_sge = (void *)ptr;
	ptr += max_iovsz*sizeof(struct xio_sge);
	tcp_task->rsp_write_sge = (void *)ptr;
	ptr += max_iovsz*sizeof(struct xio_sge);
	/*****************************************/

	tcp_task->tcp_op = 0x200;
	xio_tcp_task_init(
			task,
			tcp_hndl,
			buf,
			tcp_slab->buf_size);

	return 0;
}

/*---------------------------------------------------------------------------*/
/* xio_tcp_task_pre_put						     */
/*---------------------------------------------------------------------------*/
static int xio_tcp_task_pre_put(
		struct xio_transport_base *trans_hndl,
		struct xio_task *task)
{
	unsigned int	i;
	XIO_TO_TCP_TASK(task, tcp_task);

	/* recycle TCP  buffers back to pool */

	/* put buffers back to pool */

	for (i = 0; i < tcp_task->read_num_sge; i++) {
		if (tcp_task->read_sge[i].cache) {
			xio_mempool_free(&tcp_task->read_sge[i]);
			tcp_task->read_sge[i].cache = NULL;
		}
	}
	tcp_task->read_num_sge = 0;

	for (i = 0; i < tcp_task->write_num_sge; i++) {
		if (tcp_task->write_sge[i].cache) {
			xio_mempool_free(&tcp_task->write_sge[i]);
			tcp_task->write_sge[i].cache = NULL;
		}
	}
	tcp_task->write_num_sge		= 0;
	tcp_task->req_write_num_sge	= 0;
	tcp_task->rsp_write_num_sge	= 0;
	tcp_task->req_read_num_sge	= 0;
	tcp_task->req_recv_num_sge	= 0;
	tcp_task->sn			= 0;

	tcp_task->tcp_op		= XIO_TCP_NULL;

	xio_tcp_rxd_init(&tcp_task->rxd,
			 task->mbuf.buf.head,
			 task->mbuf.buf.buflen);
	xio_tcp_txd_init(&tcp_task->txd,
			 task->mbuf.buf.head,
			 task->mbuf.buf.buflen);

	xio_ctx_del_work(tcp_task->tcp_hndl->base.ctx, &tcp_task->comp_work);

	return 0;
}


/*---------------------------------------------------------------------------*/
/* xio_tcp_primary_pool_get_params					     */
/*---------------------------------------------------------------------------*/
static void xio_tcp_primary_pool_get_params(
		struct xio_transport_base *transport_hndl,
		int *start_nr, int *max_nr, int *alloc_nr,
		int *pool_dd_sz, int *slab_dd_sz, int *task_dd_sz)
{
	int  max_iovsz = max(tcp_options.max_out_iovsz,
				    tcp_options.max_in_iovsz) + 1;

	*start_nr = NUM_START_PRIMARY_POOL_TASKS;
	*alloc_nr = NUM_ALLOC_PRIMARY_POOL_TASKS;
	*max_nr = max((g_options.snd_queue_depth +
		       g_options.rcv_queue_depth)*40, 1024);


	*pool_dd_sz = 0;
	*slab_dd_sz = sizeof(struct xio_tcp_tasks_slab);
	*task_dd_sz = sizeof(struct xio_tcp_task) +
			(2 * (max_iovsz + 1))*sizeof(struct iovec) +
			 2 * max_iovsz * sizeof(struct xio_mempool_obj) +
			 4 * max_iovsz * sizeof(struct xio_sge);
}

static struct xio_tasks_pool_ops   primary_tasks_pool_ops = {
	.pool_get_params	= xio_tcp_primary_pool_get_params,
	.slab_pre_create	= xio_tcp_primary_pool_slab_pre_create,
	.slab_destroy		= xio_tcp_primary_pool_slab_destroy,
	.slab_init_task		= xio_tcp_primary_pool_slab_init_task,
	.pool_post_create	= xio_tcp_primary_pool_post_create,
	.task_pre_put		= xio_tcp_task_pre_put,
};

/*---------------------------------------------------------------------------*/
/* xio_tcp_get_pools_ops						     */
/*---------------------------------------------------------------------------*/
static void xio_tcp_get_pools_ops(struct xio_transport_base *trans_hndl,
				  struct xio_tasks_pool_ops **initial_pool_ops,
				  struct xio_tasks_pool_ops **primary_pool_ops)
{
	*initial_pool_ops = &initial_tasks_pool_ops;
	*primary_pool_ops = &primary_tasks_pool_ops;
}

/*---------------------------------------------------------------------------*/
/* xio_tcp_set_pools_cls						     */
/*---------------------------------------------------------------------------*/
static void xio_tcp_set_pools_cls(struct xio_transport_base *trans_hndl,
				  struct xio_tasks_pool_cls *initial_pool_cls,
				  struct xio_tasks_pool_cls *primary_pool_cls)
{
	struct xio_tcp_transport *tcp_hndl =
		(struct xio_tcp_transport *)trans_hndl;

	if (initial_pool_cls)
		tcp_hndl->initial_pool_cls = *initial_pool_cls;
	if (primary_pool_cls)
		tcp_hndl->primary_pool_cls = *primary_pool_cls;
}

/*---------------------------------------------------------------------------*/
/* xio_tcp_set_opt                                                           */
/*---------------------------------------------------------------------------*/
static int xio_tcp_set_opt(void *xio_obj,
			   int optname, const void *optval, int optlen)
{
	switch (optname) {
	case XIO_OPTNAME_ENABLE_MEM_POOL:
		VALIDATE_SZ(sizeof(int));
		tcp_options.enable_mem_pool = *((int *)optval);
		return 0;
		break;
	case XIO_OPTNAME_ENABLE_DMA_LATENCY:
		VALIDATE_SZ(sizeof(int));
		tcp_options.enable_dma_latency = *((int *)optval);
		return 0;
		break;
	case XIO_OPTNAME_TRANS_BUF_THRESHOLD:
		VALIDATE_SZ(sizeof(int));

		/* changing the parameter is not allowed */
		if (tcp_options.tcp_buf_attr_rdonly) {
			xio_set_error(EPERM);
			return -1;
		}
		if (*(int *)optval < 0 ||
		    *(int *)optval > XIO_OPTVAL_MAX_TCP_BUF_THRESHOLD) {
			xio_set_error(EINVAL);
			return -1;
		}
		tcp_options.tcp_buf_threshold = *((int *)optval);
		g_options.trans_buf_threshold = *((int *)optval);
		return 0;
		break;
	case XIO_OPTNAME_MAX_IN_IOVLEN:
		VALIDATE_SZ(sizeof(int));
		tcp_options.max_in_iovsz = *((int *)optval);
		return 0;
		break;
	case XIO_OPTNAME_MAX_OUT_IOVLEN:
		VALIDATE_SZ(sizeof(int));
		tcp_options.max_out_iovsz = *((int *)optval);
		return 0;
		break;
	case XIO_OPTNAME_TCP_ENABLE_MR_CHECK:
		VALIDATE_SZ(sizeof(int));
		tcp_options.enable_mr_check = *((int *)optval);
		return 0;
		break;
	case XIO_OPTNAME_TCP_NO_DELAY:
		VALIDATE_SZ(sizeof(int));
		tcp_options.tcp_no_delay = *((int *)optval);
		return 0;
		break;
	case XIO_OPTNAME_TCP_SO_SNDBUF:
		VALIDATE_SZ(sizeof(int));
		tcp_options.tcp_so_sndbuf = *((int *)optval);
		return 0;
		break;
	case XIO_OPTNAME_TCP_SO_RCVBUF:
		VALIDATE_SZ(sizeof(int));
		tcp_options.tcp_so_rcvbuf = *((int *)optval);
		return 0;
		break;
	case XIO_OPTNAME_TCP_DUAL_STREAM:
		VALIDATE_SZ(sizeof(int));
		tcp_options.tcp_dual_sock = *((int *)optval);
		return 0;
		break;
	default:
		break;
	}
	xio_set_error(XIO_E_NOT_SUPPORTED);
	return -1;
}

/*---------------------------------------------------------------------------*/
/* xio_tcp_get_opt                                                           */
/*---------------------------------------------------------------------------*/
static int xio_tcp_get_opt(void  *xio_obj,
			   int optname, void *optval, int *optlen)
{
	switch (optname) {
	case XIO_OPTNAME_ENABLE_MEM_POOL:
		*((int *)optval) = tcp_options.enable_mem_pool;
		*optlen = sizeof(int);
		return 0;
		break;
	case XIO_OPTNAME_ENABLE_DMA_LATENCY:
		*((int *)optval) = tcp_options.enable_dma_latency;
		*optlen = sizeof(int);
		return 0;
		break;
	case XIO_OPTNAME_TRANS_BUF_THRESHOLD:
		*((int *)optval) = tcp_options.tcp_buf_threshold;
		*optlen = sizeof(int);
		return 0;
	case XIO_OPTNAME_MAX_IN_IOVLEN:
		*((int *)optval) = tcp_options.max_in_iovsz;
		*optlen = sizeof(int);
		return 0;
		break;
	case XIO_OPTNAME_MAX_OUT_IOVLEN:
		*((int *)optval) = tcp_options.max_out_iovsz;
		*optlen = sizeof(int);
		return 0;
		break;
	case XIO_OPTNAME_TCP_ENABLE_MR_CHECK:
		*((int *)optval) = tcp_options.enable_mr_check;
		*optlen = sizeof(int);
		return 0;
		break;
	case XIO_OPTNAME_TCP_NO_DELAY:
		*((int *)optval) = tcp_options.tcp_no_delay;
		*optlen = sizeof(int);
		return 0;
		break;
	case XIO_OPTNAME_TCP_SO_SNDBUF:
		*((int *)optval) = tcp_options.tcp_so_sndbuf;
		*optlen = sizeof(int);
		return 0;
		break;
	case XIO_OPTNAME_TCP_SO_RCVBUF:
		*((int *)optval) = tcp_options.tcp_so_rcvbuf;
		*optlen = sizeof(int);
		return 0;
		break;
	case XIO_OPTNAME_TCP_DUAL_STREAM:
		*((int *)optval) = tcp_options.tcp_dual_sock;
		*optlen = sizeof(int);
		return 0;
		break;
	default:
		break;
	}
	xio_set_error(XIO_E_NOT_SUPPORTED);
	return -1;
}

/*---------------------------------------------------------------------------*/
/* xio_is_valid_in_req							     */
/*---------------------------------------------------------------------------*/
static int xio_tcp_is_valid_in_req(struct xio_msg *msg)
{
	unsigned int		i;
	unsigned int		mr_found = 0;
	struct xio_vmsg *vmsg = &msg->in;
	struct xio_sg_table_ops	*sgtbl_ops;
	void			*sgtbl;
	void			*sge;
	unsigned long		nents, max_nents;

	sgtbl		= xio_sg_table_get(&msg->in);
	sgtbl_ops	= xio_sg_table_ops_get(msg->in.sgl_type);
	nents		= tbl_nents(sgtbl_ops, sgtbl);
	max_nents	= tbl_max_nents(sgtbl_ops, sgtbl);

	if ((nents > (unsigned long)tcp_options.max_in_iovsz) ||
	    (nents > max_nents) ||
	    (max_nents > (unsigned long)tcp_options.max_in_iovsz)) {
		return 0;
	}

	if (vmsg->sgl_type == XIO_SGL_TYPE_IOV && nents > XIO_IOVLEN)
		return 0;

	if ((vmsg->header.iov_base != NULL)  &&
	    (vmsg->header.iov_len == 0))
		return 0;

	for_each_sge(sgtbl, sgtbl_ops, sge, i) {
		if (sge_mr(sgtbl_ops, sge))
			mr_found++;
		if (sge_addr(sgtbl_ops, sge) == NULL) {
			if (sge_mr(sgtbl_ops, sge))
				return 0;
		} else {
			if (sge_length(sgtbl_ops, sge)  == 0)
				return 0;
		}
	}
	if (tcp_options.enable_mr_check &&
	    (mr_found != nents) && mr_found)
		return 0;

	return 1;
}

/*---------------------------------------------------------------------------*/
/* xio_tcp_is_valid_out_msg						     */
/*---------------------------------------------------------------------------*/
static int xio_tcp_is_valid_out_msg(struct xio_msg *msg)
{
	unsigned int		i;
	unsigned int		mr_found = 0;
	struct xio_vmsg		*vmsg = &msg->out;
	struct xio_sg_table_ops	*sgtbl_ops;
	void			*sgtbl;
	void			*sge;
	unsigned long		nents, max_nents;

	sgtbl		= xio_sg_table_get(&msg->out);
	sgtbl_ops	= xio_sg_table_ops_get(msg->out.sgl_type);
	nents		= tbl_nents(sgtbl_ops, sgtbl);
	max_nents	= tbl_max_nents(sgtbl_ops, sgtbl);

	if ((nents > (unsigned long)tcp_options.max_out_iovsz) ||
	    (nents > max_nents) ||
	    (max_nents > (unsigned long)tcp_options.max_out_iovsz))
		return 0;

	if (vmsg->sgl_type == XIO_SGL_TYPE_IOV && nents > XIO_IOVLEN)
		return 0;

	if (((vmsg->header.iov_base != NULL)  &&
	     (vmsg->header.iov_len == 0)) ||
	    ((vmsg->header.iov_base == NULL)  &&
	     (vmsg->header.iov_len != 0)))
			return 0;

	for_each_sge(sgtbl, sgtbl_ops, sge, i) {
		if (sge_mr(sgtbl_ops, sge))
			mr_found++;
		if ((sge_addr(sgtbl_ops, sge) == NULL) ||
		    (sge_length(sgtbl_ops, sge)  == 0))
			return 0;
	}

	if (tcp_options.enable_mr_check &&
	    (mr_found != nents) && mr_found)
		return 0;

	return 1;
}

/*---------------------------------------------------------------------------*/
/* xio_tcp_dup2			                                             */
/* makes new_trans_hndl be the copy of old_trans_hndl, closes new_trans_hndl */
/* Note old and new are in dup2 terminology opposite to reconnect terms	     */
/* --------------------------------------------------------------------------*/
static int xio_tcp_dup2(struct xio_transport_base *old_trans_hndl,
			struct xio_transport_base **new_trans_hndl)
{
	xio_tcp_close(*new_trans_hndl);

	/* conn layer will call close which will only decrement */
	kref_get(&old_trans_hndl->kref);
	*new_trans_hndl = old_trans_hndl;

	return 0;
}

struct xio_tcp_socket_ops single_sock_ops = {
	.open			= xio_tcp_single_sock_create,
	.add_ev_handlers	= xio_tcp_single_sock_add_ev_handlers,
	.del_ev_handlers	= xio_tcp_single_sock_del_ev_handlers,
	.connect		= xio_tcp_single_sock_connect,
	.set_txd		= xio_tcp_single_sock_set_txd,
	.set_rxd		= xio_tcp_single_sock_set_rxd,
	.rx_ctl_work		= xio_tcp_recvmsg_work,
	.rx_ctl_handler		= xio_tcp_single_sock_rx_ctl_handler,
	.rx_data_handler	= xio_tcp_rx_data_handler,
	.shutdown		= xio_tcp_single_sock_shutdown,
	.close			= xio_tcp_single_sock_close,
};

struct xio_tcp_socket_ops dual_sock_ops = {
	.open			= xio_tcp_dual_sock_create,
	.add_ev_handlers	= xio_tcp_dual_sock_add_ev_handlers,
	.del_ev_handlers	= xio_tcp_dual_sock_del_ev_handlers,
	.connect		= xio_tcp_dual_sock_connect,
	.set_txd		= xio_tcp_dual_sock_set_txd,
	.set_rxd		= xio_tcp_dual_sock_set_rxd,
	.rx_ctl_work		= xio_tcp_recv_ctl_work,
	.rx_ctl_handler		= xio_tcp_dual_sock_rx_ctl_handler,
	.rx_data_handler	= xio_tcp_rx_data_handler,
	.shutdown		= xio_tcp_dual_sock_shutdown,
	.close			= xio_tcp_dual_sock_close,
};

struct xio_transport xio_tcp_transport = {
	.name			= "tcp",
	.ctor			= xio_tcp_transport_constructor,
	.dtor			= xio_tcp_transport_destructor,
	.init			= xio_tcp_transport_init,
	.release		= xio_tcp_transport_release,
	.context_shutdown	= xio_tcp_context_shutdown,
	.open			= xio_tcp_open,
	.connect		= xio_tcp_connect,
	.listen			= xio_tcp_listen,
	.accept			= xio_tcp_accept,
	.reject			= xio_tcp_reject,
	.close			= xio_tcp_close,
	.dup2			= xio_tcp_dup2,
/*	.update_task		= xio_tcp_update_task,*/
	.send			= xio_tcp_send,
	.poll			= xio_tcp_poll,
	.set_opt		= xio_tcp_set_opt,
	.get_opt		= xio_tcp_get_opt,
	.cancel_req		= xio_tcp_cancel_req,
	.cancel_rsp		= xio_tcp_cancel_rsp,
	.get_pools_setup_ops	= xio_tcp_get_pools_ops,
	.set_pools_cls		= xio_tcp_set_pools_cls,

	.validators_cls.is_valid_in_req  = xio_tcp_is_valid_in_req,
	.validators_cls.is_valid_out_msg = xio_tcp_is_valid_out_msg,
};
