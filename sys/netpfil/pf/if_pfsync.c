/*	$OpenBSD: if_pfsync.c,v 1.110 2009/02/24 05:39:19 dlg Exp $	*/

/*
 * Copyright (c) 2002 Michael Shalayeff
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR OR HIS RELATIVES BE LIABLE FOR ANY DIRECT,
 * INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF MIND, USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
 * STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING
 * IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 * THE POSSIBILITY OF SUCH DAMAGE.
 */

/*
 * Copyright (c) 2009 David Gwynne <dlg@openbsd.org>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

/*
 * Revisions picked from OpenBSD after revision 1.110 import:
 * 1.118, 1.124, 1.148, 1.149, 1.151, 1.171 - fixes to bulk updates
 * 1.120, 1.175 - use monotonic time_uptime
 * 1.122 - reduce number of updates for non-TCP sessions
 * 1.125 - rewrite merge or stale processing
 * 1.128 - cleanups
 * 1.146 - bzero() mbuf before sparsely filling it with data
 * 1.170 - SIOCSIFMTU checks
 * 1.126, 1.142 - deferred packets processing
 * 1.173 - correct expire time processing
 */

#include <sys/cdefs.h>
__FBSDID("$FreeBSD$");

#include "opt_inet.h"
#include "opt_inet6.h"
#include "opt_pf.h"

#include <sys/param.h>
#include <sys/bus.h>
#include <sys/endian.h>
#include <sys/interrupt.h>
#include <sys/kernel.h>
#include <sys/lock.h>
#include <sys/mbuf.h>
#include <sys/module.h>
#include <sys/mutex.h>
#include <sys/priv.h>
#include <sys/protosw.h>
#include <sys/socket.h>
#include <sys/sockio.h>
#include <sys/sysctl.h>

#include <net/bpf.h>
#include <net/if.h>
#include <net/if_clone.h>
#include <net/if_types.h>
#include <net/pfvar.h>
#include <net/if_pfsync.h>

#include <netinet/if_ether.h>
#include <netinet/in.h>
#include <netinet/in_var.h>
#include <netinet/ip.h>
#include <netinet/ip_carp.h>
#include <netinet/ip_var.h>
#include <netinet/tcp.h>
#include <netinet/tcp_fsm.h>
#include <netinet/tcp_seq.h>

#define PFSYNC_MINPKT ( \
	sizeof(struct ip) + \
	sizeof(struct pfsync_header) + \
	sizeof(struct pfsync_subheader) + \
	sizeof(struct pfsync_eof))

struct pfsync_pkt {
	struct ip *ip;
	struct in_addr src;
	u_int8_t flags;
};

static int	pfsync_upd_tcp(struct pf_state *, struct pfsync_state_peer *,
		    struct pfsync_state_peer *);
static int	pfsync_in_clr(struct pfsync_pkt *, struct mbuf *, int, int);
static int	pfsync_in_ins(struct pfsync_pkt *, struct mbuf *, int, int);
static int	pfsync_in_iack(struct pfsync_pkt *, struct mbuf *, int, int);
static int	pfsync_in_upd(struct pfsync_pkt *, struct mbuf *, int, int);
static int	pfsync_in_upd_c(struct pfsync_pkt *, struct mbuf *, int, int);
static int	pfsync_in_ureq(struct pfsync_pkt *, struct mbuf *, int, int);
static int	pfsync_in_del(struct pfsync_pkt *, struct mbuf *, int, int);
static int	pfsync_in_del_c(struct pfsync_pkt *, struct mbuf *, int, int);
static int	pfsync_in_bus(struct pfsync_pkt *, struct mbuf *, int, int);
static int	pfsync_in_tdb(struct pfsync_pkt *, struct mbuf *, int, int);
static int	pfsync_in_eof(struct pfsync_pkt *, struct mbuf *, int, int);
static int	pfsync_in_error(struct pfsync_pkt *, struct mbuf *, int, int);

static int (*pfsync_acts[])(struct pfsync_pkt *, struct mbuf *, int, int) = {
	pfsync_in_clr,			/* PFSYNC_ACT_CLR */
	pfsync_in_ins,			/* PFSYNC_ACT_INS */
	pfsync_in_iack,			/* PFSYNC_ACT_INS_ACK */
	pfsync_in_upd,			/* PFSYNC_ACT_UPD */
	pfsync_in_upd_c,		/* PFSYNC_ACT_UPD_C */
	pfsync_in_ureq,			/* PFSYNC_ACT_UPD_REQ */
	pfsync_in_del,			/* PFSYNC_ACT_DEL */
	pfsync_in_del_c,		/* PFSYNC_ACT_DEL_C */
	pfsync_in_error,		/* PFSYNC_ACT_INS_F */
	pfsync_in_error,		/* PFSYNC_ACT_DEL_F */
	pfsync_in_bus,			/* PFSYNC_ACT_BUS */
	pfsync_in_tdb,			/* PFSYNC_ACT_TDB */
	pfsync_in_eof			/* PFSYNC_ACT_EOF */
};

struct pfsync_q {
	void		(*write)(struct pf_state *, void *);
	size_t		len;
	u_int8_t	action;
};

/* we have one of these for every PFSYNC_S_ */
static void	pfsync_out_state(struct pf_state *, void *);
static void	pfsync_out_iack(struct pf_state *, void *);
static void	pfsync_out_upd_c(struct pf_state *, void *);
static void	pfsync_out_del(struct pf_state *, void *);

static struct pfsync_q pfsync_qs[] = {
	{ pfsync_out_state, sizeof(struct pfsync_state),   PFSYNC_ACT_INS },
	{ pfsync_out_iack,  sizeof(struct pfsync_ins_ack), PFSYNC_ACT_INS_ACK },
	{ pfsync_out_state, sizeof(struct pfsync_state),   PFSYNC_ACT_UPD },
	{ pfsync_out_upd_c, sizeof(struct pfsync_upd_c),   PFSYNC_ACT_UPD_C },
	{ pfsync_out_del,   sizeof(struct pfsync_del_c),   PFSYNC_ACT_DEL_C }
};

static void	pfsync_q_ins(struct pf_state *, int);
static void	pfsync_q_del(struct pf_state *);

static void	pfsync_update_state(struct pf_state *);

struct pfsync_upd_req_item {
	TAILQ_ENTRY(pfsync_upd_req_item)	ur_entry;
	struct pfsync_upd_req			ur_msg;
};

struct pfsync_deferral {
	struct pfsync_softc		*pd_sc;
	TAILQ_ENTRY(pfsync_deferral)	pd_entry;
	u_int				pd_refs;
	struct callout			pd_tmo;

	struct pf_state			*pd_st;
	struct mbuf			*pd_m;
};

struct pfsync_softc {
	/* Configuration */
	struct ifnet		*sc_ifp;
	struct ifnet		*sc_sync_if;
	struct ip_moptions	sc_imo;
	struct in_addr		sc_sync_peer;
	uint32_t		sc_flags;
#define	PFSYNCF_OK		0x00000001
#define	PFSYNCF_DEFER		0x00000002
#define	PFSYNCF_PUSH		0x00000004
	uint8_t			sc_maxupdates;
	struct ip		sc_template;
	struct callout		sc_tmo;
	struct mtx		sc_mtx;

	/* Queued data */
	size_t			sc_len;
	TAILQ_HEAD(, pf_state)			sc_qs[PFSYNC_S_COUNT];
	TAILQ_HEAD(, pfsync_upd_req_item)	sc_upd_req_list;
	TAILQ_HEAD(, pfsync_deferral)		sc_deferrals;
	u_int			sc_deferred;
	void			*sc_plus;
	size_t			sc_pluslen;

	/* Bulk update info */
	struct mtx		sc_bulk_mtx;
	uint32_t		sc_ureq_sent;
	int			sc_bulk_tries;
	uint32_t		sc_ureq_received;
	int			sc_bulk_hashid;
	uint64_t		sc_bulk_stateid;
	uint32_t		sc_bulk_creatorid;
	struct callout		sc_bulk_tmo;
	struct callout		sc_bulkfail_tmo;
};

#define	PFSYNC_LOCK(sc)		mtx_lock(&(sc)->sc_mtx)
#define	PFSYNC_UNLOCK(sc)	mtx_unlock(&(sc)->sc_mtx)
#define	PFSYNC_LOCK_ASSERT(sc)	mtx_assert(&(sc)->sc_mtx, MA_OWNED)

#define	PFSYNC_BLOCK(sc)	mtx_lock(&(sc)->sc_bulk_mtx)
#define	PFSYNC_BUNLOCK(sc)	mtx_unlock(&(sc)->sc_bulk_mtx)
#define	PFSYNC_BLOCK_ASSERT(sc)	mtx_assert(&(sc)->sc_bulk_mtx, MA_OWNED)

static const char pfsyncname[] = "pfsync";
static MALLOC_DEFINE(M_PFSYNC, pfsyncname, "pfsync(4) data");
static VNET_DEFINE(struct pfsync_softc	*, pfsyncif) = NULL;
#define	V_pfsyncif		VNET(pfsyncif)
static VNET_DEFINE(void *, pfsync_swi_cookie) = NULL;
#define	V_pfsync_swi_cookie	VNET(pfsync_swi_cookie)
static VNET_DEFINE(struct pfsyncstats, pfsyncstats);
#define	V_pfsyncstats		VNET(pfsyncstats)
static VNET_DEFINE(int, pfsync_carp_adj) = CARP_MAXSKEW;
#define	V_pfsync_carp_adj	VNET(pfsync_carp_adj)

static void	pfsync_timeout(void *);
static void	pfsync_push(struct pfsync_softc *);
static void	pfsyncintr(void *);
static int	pfsync_multicast_setup(struct pfsync_softc *, struct ifnet *,
		    void *);
static void	pfsync_multicast_cleanup(struct pfsync_softc *);
static void	pfsync_pointers_init(void);
static void	pfsync_pointers_uninit(void);
static int	pfsync_init(void);
static void	pfsync_uninit(void);

SYSCTL_NODE(_net, OID_AUTO, pfsync, CTLFLAG_RW, 0, "PFSYNC");
SYSCTL_VNET_STRUCT(_net_pfsync, OID_AUTO, stats, CTLFLAG_RW,
    &VNET_NAME(pfsyncstats), pfsyncstats,
    "PFSYNC statistics (struct pfsyncstats, net/if_pfsync.h)");
SYSCTL_INT(_net_pfsync, OID_AUTO, carp_demotion_factor, CTLFLAG_RW,
    &VNET_NAME(pfsync_carp_adj), 0, "pfsync's CARP demotion factor adjustment");

static int	pfsync_clone_create(struct if_clone *, int, caddr_t);
static void	pfsync_clone_destroy(struct ifnet *);
static int	pfsync_alloc_scrub_memory(struct pfsync_state_peer *,
		    struct pf_state_peer *);
static int	pfsyncoutput(struct ifnet *, struct mbuf *, struct sockaddr *,
		    struct route *);
static int	pfsyncioctl(struct ifnet *, u_long, caddr_t);

static int	pfsync_defer(struct pf_state *, struct mbuf *);
static void	pfsync_undefer(struct pfsync_deferral *, int);
static void	pfsync_undefer_state(struct pf_state *, int);
static void	pfsync_defer_tmo(void *);

static void	pfsync_request_update(u_int32_t, u_int64_t);
static void	pfsync_update_state_req(struct pf_state *);

static void	pfsync_drop(struct pfsync_softc *);
static void	pfsync_sendout(int);
static void	pfsync_send_plus(void *, size_t);

static void	pfsync_bulk_start(void);
static void	pfsync_bulk_status(u_int8_t);
static void	pfsync_bulk_update(void *);
static void	pfsync_bulk_fail(void *);

#ifdef IPSEC
static void	pfsync_update_net_tdb(struct pfsync_tdb *);
#endif

#define PFSYNC_MAX_BULKTRIES	12

VNET_DEFINE(struct if_clone *, pfsync_cloner);
#define	V_pfsync_cloner	VNET(pfsync_cloner)

static int
pfsync_clone_create(struct if_clone *ifc, int unit, caddr_t param)
{
	struct pfsync_softc *sc;
	struct ifnet *ifp;
	int q;

	if (unit != 0)
		return (EINVAL);

	sc = malloc(sizeof(struct pfsync_softc), M_PFSYNC, M_WAITOK | M_ZERO);
	sc->sc_flags |= PFSYNCF_OK;

	for (q = 0; q < PFSYNC_S_COUNT; q++)
		TAILQ_INIT(&sc->sc_qs[q]);

	TAILQ_INIT(&sc->sc_upd_req_list);
	TAILQ_INIT(&sc->sc_deferrals);

	sc->sc_len = PFSYNC_MINPKT;
	sc->sc_maxupdates = 128;

	ifp = sc->sc_ifp = if_alloc(IFT_PFSYNC);
	if (ifp == NULL) {
		free(sc, M_PFSYNC);
		return (ENOSPC);
	}
	if_initname(ifp, pfsyncname, unit);
	ifp->if_softc = sc;
	ifp->if_ioctl = pfsyncioctl;
	ifp->if_output = pfsyncoutput;
	ifp->if_type = IFT_PFSYNC;
	ifp->if_snd.ifq_maxlen = ifqmaxlen;
	ifp->if_hdrlen = sizeof(struct pfsync_header);
	ifp->if_mtu = ETHERMTU;
	mtx_init(&sc->sc_mtx, pfsyncname, NULL, MTX_DEF);
	mtx_init(&sc->sc_bulk_mtx, "pfsync bulk", NULL, MTX_DEF);
	callout_init(&sc->sc_tmo, CALLOUT_MPSAFE);
	callout_init_mtx(&sc->sc_bulk_tmo, &sc->sc_bulk_mtx, 0);
	callout_init_mtx(&sc->sc_bulkfail_tmo, &sc->sc_bulk_mtx, 0);

	if_attach(ifp);

	bpfattach(ifp, DLT_PFSYNC, PFSYNC_HDRLEN);

	V_pfsyncif = sc;

	return (0);
}

static void
pfsync_clone_destroy(struct ifnet *ifp)
{
	struct pfsync_softc *sc = ifp->if_softc;

	/*
	 * At this stage, everything should have already been
	 * cleared by pfsync_uninit(), and we have only to
	 * drain callouts.
	 */
	while (sc->sc_deferred > 0) {
		struct pfsync_deferral *pd = TAILQ_FIRST(&sc->sc_deferrals);

		TAILQ_REMOVE(&sc->sc_deferrals, pd, pd_entry);
		sc->sc_deferred--;
		if (callout_stop(&pd->pd_tmo)) {
			pf_release_state(pd->pd_st);
			m_freem(pd->pd_m);
			free(pd, M_PFSYNC);
		} else {
			pd->pd_refs++;
			callout_drain(&pd->pd_tmo);
			free(pd, M_PFSYNC);
		}
	}

	callout_drain(&sc->sc_tmo);
	callout_drain(&sc->sc_bulkfail_tmo);
	callout_drain(&sc->sc_bulk_tmo);

	if (!(sc->sc_flags & PFSYNCF_OK) && carp_demote_adj_p)
		(*carp_demote_adj_p)(-V_pfsync_carp_adj, "pfsync destroy");
	bpfdetach(ifp);
	if_detach(ifp);

	pfsync_drop(sc);

	if_free(ifp);
	if (sc->sc_imo.imo_membership)
		pfsync_multicast_cleanup(sc);
	mtx_destroy(&sc->sc_mtx);
	mtx_destroy(&sc->sc_bulk_mtx);
	free(sc, M_PFSYNC);

	V_pfsyncif = NULL;
}

static int
pfsync_alloc_scrub_memory(struct pfsync_state_peer *s,
    struct pf_state_peer *d)
{
	if (s->scrub.scrub_flag && d->scrub == NULL) {
		d->scrub = uma_zalloc(V_pf_state_scrub_z, M_NOWAIT | M_ZERO);
		if (d->scrub == NULL)
			return (ENOMEM);
	}

	return (0);
}


static int
pfsync_state_import(struct pfsync_state *sp, u_int8_t flags)
{
	struct pfsync_softc *sc = V_pfsyncif;
	struct pf_state	*st = NULL;
	struct pf_state_key *skw = NULL, *sks = NULL;
	struct pf_rule *r = NULL;
	struct pfi_kif	*kif;
	int error;

	PF_RULES_RASSERT();

	if (sp->creatorid == 0 && V_pf_status.debug >= PF_DEBUG_MISC) {
		printf("%s: invalid creator id: %08x\n", __func__,
		    ntohl(sp->creatorid));
		return (EINVAL);
	}

	if ((kif = pfi_kif_find(sp->ifname)) == NULL) {
		if (V_pf_status.debug >= PF_DEBUG_MISC)
			printf("%s: unknown interface: %s\n", __func__,
			    sp->ifname);
		if (flags & PFSYNC_SI_IOCTL)
			return (EINVAL);
		return (0);	/* skip this state */
	}

	/*
	 * If the ruleset checksums match or the state is coming from the ioctl,
	 * it's safe to associate the state with the rule of that number.
	 */
	if (sp->rule != htonl(-1) && sp->anchor == htonl(-1) &&
	    (flags & (PFSYNC_SI_IOCTL | PFSYNC_SI_CKSUM)) && ntohl(sp->rule) <
	    pf_main_ruleset.rules[PF_RULESET_FILTER].active.rcount)
		r = pf_main_ruleset.rules[
		    PF_RULESET_FILTER].active.ptr_array[ntohl(sp->rule)];
	else
		r = &V_pf_default_rule;

	if ((r->max_states && r->states_cur >= r->max_states))
		goto cleanup;

	/*
	 * XXXGL: consider M_WAITOK in ioctl path after.
	 */
	if ((st = uma_zalloc(V_pf_state_z, M_NOWAIT | M_ZERO)) == NULL)
		goto cleanup;

	if ((skw = uma_zalloc(V_pf_state_key_z, M_NOWAIT)) == NULL)
		goto cleanup;

	if (PF_ANEQ(&sp->key[PF_SK_WIRE].addr[0],
	    &sp->key[PF_SK_STACK].addr[0], sp->af) ||
	    PF_ANEQ(&sp->key[PF_SK_WIRE].addr[1],
	    &sp->key[PF_SK_STACK].addr[1], sp->af) ||
	    sp->key[PF_SK_WIRE].port[0] != sp->key[PF_SK_STACK].port[0] ||
	    sp->key[PF_SK_WIRE].port[1] != sp->key[PF_SK_STACK].port[1]) {
		sks = uma_zalloc(V_pf_state_key_z, M_NOWAIT);
		if (sks == NULL)
			goto cleanup;
	} else
		sks = skw;

	/* allocate memory for scrub info */
	if (pfsync_alloc_scrub_memory(&sp->src, &st->src) ||
	    pfsync_alloc_scrub_memory(&sp->dst, &st->dst))
		goto cleanup;

	/* copy to state key(s) */
	skw->addr[0] = sp->key[PF_SK_WIRE].addr[0];
	skw->addr[1] = sp->key[PF_SK_WIRE].addr[1];
	skw->port[0] = sp->key[PF_SK_WIRE].port[0];
	skw->port[1] = sp->key[PF_SK_WIRE].port[1];
	skw->proto = sp->proto;
	skw->af = sp->af;
	if (sks != skw) {
		sks->addr[0] = sp->key[PF_SK_STACK].addr[0];
		sks->addr[1] = sp->key[PF_SK_STACK].addr[1];
		sks->port[0] = sp->key[PF_SK_STACK].port[0];
		sks->port[1] = sp->key[PF_SK_STACK].port[1];
		sks->proto = sp->proto;
		sks->af = sp->af;
	}

	/* copy to state */
	bcopy(&sp->rt_addr, &st->rt_addr, sizeof(st->rt_addr));
	st->creation = time_uptime - ntohl(sp->creation);
	st->expire = time_uptime;
	if (sp->expire) {
		uint32_t timeout;

		timeout = r->timeout[sp->timeout];
		if (!timeout)
			timeout = V_pf_default_rule.timeout[sp->timeout];

		/* sp->expire may have been adaptively scaled by export. */
		st->expire -= timeout - ntohl(sp->expire);
	}

	st->direction = sp->direction;
	st->log = sp->log;
	st->timeout = sp->timeout;
	st->state_flags = sp->state_flags;

	st->id = sp->id;
	st->creatorid = sp->creatorid;
	pf_state_peer_ntoh(&sp->src, &st->src);
	pf_state_peer_ntoh(&sp->dst, &st->dst);

	st->rule.ptr = r;
	st->nat_rule.ptr = NULL;
	st->anchor.ptr = NULL;
	st->rt_kif = NULL;

	st->pfsync_time = time_uptime;
	st->sync_state = PFSYNC_S_NONE;

	/* XXX when we have nat_rule/anchors, use STATE_INC_COUNTERS */
	r->states_cur++;
	r->states_tot++;

	if (!(flags & PFSYNC_SI_IOCTL))
		st->state_flags |= PFSTATE_NOSYNC;

	if ((error = pf_state_insert(kif, skw, sks, st)) != 0) {
		/* XXX when we have nat_rule/anchors, use STATE_DEC_COUNTERS */
		r->states_cur--;
		goto cleanup_state;
	}

	if (!(flags & PFSYNC_SI_IOCTL)) {
		st->state_flags &= ~PFSTATE_NOSYNC;
		if (st->state_flags & PFSTATE_ACK) {
			pfsync_q_ins(st, PFSYNC_S_IACK);
			pfsync_push(sc);
		}
	}
	st->state_flags &= ~PFSTATE_ACK;
	PF_STATE_UNLOCK(st);

	return (0);

cleanup:
	error = ENOMEM;
	if (skw == sks)
		sks = NULL;
	if (skw != NULL)
		uma_zfree(V_pf_state_key_z, skw);
	if (sks != NULL)
		uma_zfree(V_pf_state_key_z, sks);

cleanup_state:	/* pf_state_insert() frees the state keys. */
	if (st) {
		if (st->dst.scrub)
			uma_zfree(V_pf_state_scrub_z, st->dst.scrub);
		if (st->src.scrub)
			uma_zfree(V_pf_state_scrub_z, st->src.scrub);
		uma_zfree(V_pf_state_z, st);
	}
	return (error);
}

static void
pfsync_input(struct mbuf *m, __unused int off)
{
	struct pfsync_softc *sc = V_pfsyncif;
	struct pfsync_pkt pkt;
	struct ip *ip = mtod(m, struct ip *);
	struct pfsync_header *ph;
	struct pfsync_subheader subh;

	int offset;
	int rv;
	uint16_t count;

	V_pfsyncstats.pfsyncs_ipackets++;

	/* Verify that we have a sync interface configured. */
	if (!sc || !sc->sc_sync_if || !V_pf_status.running ||
	    (sc->sc_ifp->if_drv_flags & IFF_DRV_RUNNING) == 0)
		goto done;

	/* verify that the packet came in on the right interface */
	if (sc->sc_sync_if != m->m_pkthdr.rcvif) {
		V_pfsyncstats.pfsyncs_badif++;
		goto done;
	}

	sc->sc_ifp->if_ipackets++;
	sc->sc_ifp->if_ibytes += m->m_pkthdr.len;
	/* verify that the IP TTL is 255. */
	if (ip->ip_ttl != PFSYNC_DFLTTL) {
		V_pfsyncstats.pfsyncs_badttl++;
		goto done;
	}

	offset = ip->ip_hl << 2;
	if (m->m_pkthdr.len < offset + sizeof(*ph)) {
		V_pfsyncstats.pfsyncs_hdrops++;
		goto done;
	}

	if (offset + sizeof(*ph) > m->m_len) {
		if (m_pullup(m, offset + sizeof(*ph)) == NULL) {
			V_pfsyncstats.pfsyncs_hdrops++;
			return;
		}
		ip = mtod(m, struct ip *);
	}
	ph = (struct pfsync_header *)((char *)ip + offset);

	/* verify the version */
	if (ph->version != PFSYNC_VERSION) {
		V_pfsyncstats.pfsyncs_badver++;
		goto done;
	}

	/* Cheaper to grab this now than having to mess with mbufs later */
	pkt.ip = ip;
	pkt.src = ip->ip_src;
	pkt.flags = 0;

	/*
	 * Trusting pf_chksum during packet processing, as well as seeking
	 * in interface name tree, require holding PF_RULES_RLOCK().
	 */
	PF_RULES_RLOCK();
	if (!bcmp(&ph->pfcksum, &V_pf_status.pf_chksum, PF_MD5_DIGEST_LENGTH))
		pkt.flags |= PFSYNC_SI_CKSUM;

	offset += sizeof(*ph);
	for (;;) {
		m_copydata(m, offset, sizeof(subh), (caddr_t)&subh);
		offset += sizeof(subh);

		if (subh.action >= PFSYNC_ACT_MAX) {
			V_pfsyncstats.pfsyncs_badact++;
			PF_RULES_RUNLOCK();
			goto done;
		}

		count = ntohs(subh.count);
		V_pfsyncstats.pfsyncs_iacts[subh.action] += count;
		rv = (*pfsync_acts[subh.action])(&pkt, m, offset, count);
		if (rv == -1) {
			PF_RULES_RUNLOCK();
			return;
		}

		offset += rv;
	}
	PF_RULES_RUNLOCK();

done:
	m_freem(m);
}

static int
pfsync_in_clr(struct pfsync_pkt *pkt, struct mbuf *m, int offset, int count)
{
	struct pfsync_clr *clr;
	struct mbuf *mp;
	int len = sizeof(*clr) * count;
	int i, offp;
	u_int32_t creatorid;

	mp = m_pulldown(m, offset, len, &offp);
	if (mp == NULL) {
		V_pfsyncstats.pfsyncs_badlen++;
		return (-1);
	}
	clr = (struct pfsync_clr *)(mp->m_data + offp);

	for (i = 0; i < count; i++) {
		creatorid = clr[i].creatorid;

		if (clr[i].ifname[0] != '\0' &&
		    pfi_kif_find(clr[i].ifname) == NULL)
			continue;

		for (int i = 0; i <= V_pf_hashmask; i++) {
			struct pf_idhash *ih = &V_pf_idhash[i];
			struct pf_state *s;
relock:
			PF_HASHROW_LOCK(ih);
			LIST_FOREACH(s, &ih->states, entry) {
				if (s->creatorid == creatorid) {
					s->state_flags |= PFSTATE_NOSYNC;
					pf_unlink_state(s, PF_ENTER_LOCKED);
					goto relock;
				}
			}
			PF_HASHROW_UNLOCK(ih);
		}
	}

	return (len);
}

static int
pfsync_in_ins(struct pfsync_pkt *pkt, struct mbuf *m, int offset, int count)
{
	struct mbuf *mp;
	struct pfsync_state *sa, *sp;
	int len = sizeof(*sp) * count;
	int i, offp;

	mp = m_pulldown(m, offset, len, &offp);
	if (mp == NULL) {
		V_pfsyncstats.pfsyncs_badlen++;
		return (-1);
	}
	sa = (struct pfsync_state *)(mp->m_data + offp);

	for (i = 0; i < count; i++) {
		sp = &sa[i];

		/* Check for invalid values. */
		if (sp->timeout >= PFTM_MAX ||
		    sp->src.state > PF_TCPS_PROXY_DST ||
		    sp->dst.state > PF_TCPS_PROXY_DST ||
		    sp->direction > PF_OUT ||
		    (sp->af != AF_INET && sp->af != AF_INET6)) {
			if (V_pf_status.debug >= PF_DEBUG_MISC)
				printf("%s: invalid value\n", __func__);
			V_pfsyncstats.pfsyncs_badval++;
			continue;
		}

		if (pfsync_state_import(sp, pkt->flags) == ENOMEM)
			/* Drop out, but process the rest of the actions. */
			break;
	}

	return (len);
}

static int
pfsync_in_iack(struct pfsync_pkt *pkt, struct mbuf *m, int offset, int count)
{
	struct pfsync_ins_ack *ia, *iaa;
	struct pf_state *st;

	struct mbuf *mp;
	int len = count * sizeof(*ia);
	int offp, i;

	mp = m_pulldown(m, offset, len, &offp);
	if (mp == NULL) {
		V_pfsyncstats.pfsyncs_badlen++;
		return (-1);
	}
	iaa = (struct pfsync_ins_ack *)(mp->m_data + offp);

	for (i = 0; i < count; i++) {
		ia = &iaa[i];

		st = pf_find_state_byid(ia->id, ia->creatorid);
		if (st == NULL)
			continue;

		if (st->state_flags & PFSTATE_ACK) {
			PFSYNC_LOCK(V_pfsyncif);
			pfsync_undefer_state(st, 0);
			PFSYNC_UNLOCK(V_pfsyncif);
		}
		PF_STATE_UNLOCK(st);
	}
	/*
	 * XXX this is not yet implemented, but we know the size of the
	 * message so we can skip it.
	 */

	return (count * sizeof(struct pfsync_ins_ack));
}

static int
pfsync_upd_tcp(struct pf_state *st, struct pfsync_state_peer *src,
    struct pfsync_state_peer *dst)
{
	int sync = 0;

	PF_STATE_LOCK_ASSERT(st);

	/*
	 * The state should never go backwards except
	 * for syn-proxy states.  Neither should the
	 * sequence window slide backwards.
	 */
	if ((st->src.state > src->state &&
	    (st->src.state < PF_TCPS_PROXY_SRC ||
	    src->state >= PF_TCPS_PROXY_SRC)) ||
	    SEQ_GT(st->src.seqlo, ntohl(src->seqlo)))
		sync++;
	else
		pf_state_peer_ntoh(src, &st->src);

	if (st->dst.state > dst->state ||
	    (st->dst.state >= TCPS_SYN_SENT &&
	    SEQ_GT(st->dst.seqlo, ntohl(dst->seqlo))))
		sync++;
	else
		pf_state_peer_ntoh(dst, &st->dst);

	return (sync);
}

static int
pfsync_in_upd(struct pfsync_pkt *pkt, struct mbuf *m, int offset, int count)
{
	struct pfsync_softc *sc = V_pfsyncif;
	struct pfsync_state *sa, *sp;
	struct pf_state *st;
	int sync;

	struct mbuf *mp;
	int len = count * sizeof(*sp);
	int offp, i;

	mp = m_pulldown(m, offset, len, &offp);
	if (mp == NULL) {
		V_pfsyncstats.pfsyncs_badlen++;
		return (-1);
	}
	sa = (struct pfsync_state *)(mp->m_data + offp);

	for (i = 0; i < count; i++) {
		sp = &sa[i];

		/* check for invalid values */
		if (sp->timeout >= PFTM_MAX ||
		    sp->src.state > PF_TCPS_PROXY_DST ||
		    sp->dst.state > PF_TCPS_PROXY_DST) {
			if (V_pf_status.debug >= PF_DEBUG_MISC) {
				printf("pfsync_input: PFSYNC_ACT_UPD: "
				    "invalid value\n");
			}
			V_pfsyncstats.pfsyncs_badval++;
			continue;
		}

		st = pf_find_state_byid(sp->id, sp->creatorid);
		if (st == NULL) {
			/* insert the update */
			if (pfsync_state_import(sp, 0))
				V_pfsyncstats.pfsyncs_badstate++;
			continue;
		}

		if (st->state_flags & PFSTATE_ACK) {
			PFSYNC_LOCK(sc);
			pfsync_undefer_state(st, 1);
			PFSYNC_UNLOCK(sc);
		}

		if (st->key[PF_SK_WIRE]->proto == IPPROTO_TCP)
			sync = pfsync_upd_tcp(st, &sp->src, &sp->dst);
		else {
			sync = 0;

			/*
			 * Non-TCP protocol state machine always go
			 * forwards
			 */
			if (st->src.state > sp->src.state)
				sync++;
			else
				pf_state_peer_ntoh(&sp->src, &st->src);
			if (st->dst.state > sp->dst.state)
				sync++;
			else
				pf_state_peer_ntoh(&sp->dst, &st->dst);
		}
		if (sync < 2) {
			pfsync_alloc_scrub_memory(&sp->dst, &st->dst);
			pf_state_peer_ntoh(&sp->dst, &st->dst);
			st->expire = time_uptime;
			st->timeout = sp->timeout;
		}
		st->pfsync_time = time_uptime;

		if (sync) {
			V_pfsyncstats.pfsyncs_stale++;

			pfsync_update_state(st);
			PF_STATE_UNLOCK(st);
			PFSYNC_LOCK(sc);
			pfsync_push(sc);
			PFSYNC_UNLOCK(sc);
			continue;
		}
		PF_STATE_UNLOCK(st);
	}

	return (len);
}

static int
pfsync_in_upd_c(struct pfsync_pkt *pkt, struct mbuf *m, int offset, int count)
{
	struct pfsync_softc *sc = V_pfsyncif;
	struct pfsync_upd_c *ua, *up;
	struct pf_state *st;
	int len = count * sizeof(*up);
	int sync;
	struct mbuf *mp;
	int offp, i;

	mp = m_pulldown(m, offset, len, &offp);
	if (mp == NULL) {
		V_pfsyncstats.pfsyncs_badlen++;
		return (-1);
	}
	ua = (struct pfsync_upd_c *)(mp->m_data + offp);

	for (i = 0; i < count; i++) {
		up = &ua[i];

		/* check for invalid values */
		if (up->timeout >= PFTM_MAX ||
		    up->src.state > PF_TCPS_PROXY_DST ||
		    up->dst.state > PF_TCPS_PROXY_DST) {
			if (V_pf_status.debug >= PF_DEBUG_MISC) {
				printf("pfsync_input: "
				    "PFSYNC_ACT_UPD_C: "
				    "invalid value\n");
			}
			V_pfsyncstats.pfsyncs_badval++;
			continue;
		}

		st = pf_find_state_byid(up->id, up->creatorid);
		if (st == NULL) {
			/* We don't have this state. Ask for it. */
			PFSYNC_LOCK(sc);
			pfsync_request_update(up->creatorid, up->id);
			PFSYNC_UNLOCK(sc);
			continue;
		}

		if (st->state_flags & PFSTATE_ACK) {
			PFSYNC_LOCK(sc);
			pfsync_undefer_state(st, 1);
			PFSYNC_UNLOCK(sc);
		}

		if (st->key[PF_SK_WIRE]->proto == IPPROTO_TCP)
			sync = pfsync_upd_tcp(st, &up->src, &up->dst);
		else {
			sync = 0;

			/*
			 * Non-TCP protocol state machine always go
			 * forwards
			 */
			if (st->src.state > up->src.state)
				sync++;
			else
				pf_state_peer_ntoh(&up->src, &st->src);
			if (st->dst.state > up->dst.state)
				sync++;
			else
				pf_state_peer_ntoh(&up->dst, &st->dst);
		}
		if (sync < 2) {
			pfsync_alloc_scrub_memory(&up->dst, &st->dst);
			pf_state_peer_ntoh(&up->dst, &st->dst);
			st->expire = time_uptime;
			st->timeout = up->timeout;
		}
		st->pfsync_time = time_uptime;

		if (sync) {
			V_pfsyncstats.pfsyncs_stale++;

			pfsync_update_state(st);
			PF_STATE_UNLOCK(st);
			PFSYNC_LOCK(sc);
			pfsync_push(sc);
			PFSYNC_UNLOCK(sc);
			continue;
		}
		PF_STATE_UNLOCK(st);
	}

	return (len);
}

static int
pfsync_in_ureq(struct pfsync_pkt *pkt, struct mbuf *m, int offset, int count)
{
	struct pfsync_upd_req *ur, *ura;
	struct mbuf *mp;
	int len = count * sizeof(*ur);
	int i, offp;

	struct pf_state *st;

	mp = m_pulldown(m, offset, len, &offp);
	if (mp == NULL) {
		V_pfsyncstats.pfsyncs_badlen++;
		return (-1);
	}
	ura = (struct pfsync_upd_req *)(mp->m_data + offp);

	for (i = 0; i < count; i++) {
		ur = &ura[i];

		if (ur->id == 0 && ur->creatorid == 0)
			pfsync_bulk_start();
		else {
			st = pf_find_state_byid(ur->id, ur->creatorid);
			if (st == NULL) {
				V_pfsyncstats.pfsyncs_badstate++;
				continue;
			}
			if (st->state_flags & PFSTATE_NOSYNC) {
				PF_STATE_UNLOCK(st);
				continue;
			}

			pfsync_update_state_req(st);
			PF_STATE_UNLOCK(st);
		}
	}

	return (len);
}

static int
pfsync_in_del(struct pfsync_pkt *pkt, struct mbuf *m, int offset, int count)
{
	struct mbuf *mp;
	struct pfsync_state *sa, *sp;
	struct pf_state *st;
	int len = count * sizeof(*sp);
	int offp, i;

	mp = m_pulldown(m, offset, len, &offp);
	if (mp == NULL) {
		V_pfsyncstats.pfsyncs_badlen++;
		return (-1);
	}
	sa = (struct pfsync_state *)(mp->m_data + offp);

	for (i = 0; i < count; i++) {
		sp = &sa[i];

		st = pf_find_state_byid(sp->id, sp->creatorid);
		if (st == NULL) {
			V_pfsyncstats.pfsyncs_badstate++;
			continue;
		}
		st->state_flags |= PFSTATE_NOSYNC;
		pf_unlink_state(st, PF_ENTER_LOCKED);
	}

	return (len);
}

static int
pfsync_in_del_c(struct pfsync_pkt *pkt, struct mbuf *m, int offset, int count)
{
	struct mbuf *mp;
	struct pfsync_del_c *sa, *sp;
	struct pf_state *st;
	int len = count * sizeof(*sp);
	int offp, i;

	mp = m_pulldown(m, offset, len, &offp);
	if (mp == NULL) {
		V_pfsyncstats.pfsyncs_badlen++;
		return (-1);
	}
	sa = (struct pfsync_del_c *)(mp->m_data + offp);

	for (i = 0; i < count; i++) {
		sp = &sa[i];

		st = pf_find_state_byid(sp->id, sp->creatorid);
		if (st == NULL) {
			V_pfsyncstats.pfsyncs_badstate++;
			continue;
		}

		st->state_flags |= PFSTATE_NOSYNC;
		pf_unlink_state(st, PF_ENTER_LOCKED);
	}

	return (len);
}

static int
pfsync_in_bus(struct pfsync_pkt *pkt, struct mbuf *m, int offset, int count)
{
	struct pfsync_softc *sc = V_pfsyncif;
	struct pfsync_bus *bus;
	struct mbuf *mp;
	int len = count * sizeof(*bus);
	int offp;

	PFSYNC_BLOCK(sc);

	/* If we're not waiting for a bulk update, who cares. */
	if (sc->sc_ureq_sent == 0) {
		PFSYNC_BUNLOCK(sc);
		return (len);
	}

	mp = m_pulldown(m, offset, len, &offp);
	if (mp == NULL) {
		PFSYNC_BUNLOCK(sc);
		V_pfsyncstats.pfsyncs_badlen++;
		return (-1);
	}
	bus = (struct pfsync_bus *)(mp->m_data + offp);

	switch (bus->status) {
	case PFSYNC_BUS_START:
		callout_reset(&sc->sc_bulkfail_tmo, 4 * hz +
		    V_pf_limits[PF_LIMIT_STATES].limit /
		    ((sc->sc_ifp->if_mtu - PFSYNC_MINPKT) /
		    sizeof(struct pfsync_state)),
		    pfsync_bulk_fail, sc);
		if (V_pf_status.debug >= PF_DEBUG_MISC)
			printf("pfsync: received bulk update start\n");
		break;

	case PFSYNC_BUS_END:
		if (time_uptime - ntohl(bus->endtime) >=
		    sc->sc_ureq_sent) {
			/* that's it, we're happy */
			sc->sc_ureq_sent = 0;
			sc->sc_bulk_tries = 0;
			callout_stop(&sc->sc_bulkfail_tmo);
			if (!(sc->sc_flags & PFSYNCF_OK) && carp_demote_adj_p)
				(*carp_demote_adj_p)(-V_pfsync_carp_adj,
				    "pfsync bulk done");
			sc->sc_flags |= PFSYNCF_OK;
			if (V_pf_status.debug >= PF_DEBUG_MISC)
				printf("pfsync: received valid "
				    "bulk update end\n");
		} else {
			if (V_pf_status.debug >= PF_DEBUG_MISC)
				printf("pfsync: received invalid "
				    "bulk update end: bad timestamp\n");
		}
		break;
	}
	PFSYNC_BUNLOCK(sc);

	return (len);
}

static int
pfsync_in_tdb(struct pfsync_pkt *pkt, struct mbuf *m, int offset, int count)
{
	int len = count * sizeof(struct pfsync_tdb);

#if defined(IPSEC)
	struct pfsync_tdb *tp;
	struct mbuf *mp;
	int offp;
	int i;
	int s;

	mp = m_pulldown(m, offset, len, &offp);
	if (mp == NULL) {
		V_pfsyncstats.pfsyncs_badlen++;
		return (-1);
	}
	tp = (struct pfsync_tdb *)(mp->m_data + offp);

	for (i = 0; i < count; i++)
		pfsync_update_net_tdb(&tp[i]);
#endif

	return (len);
}

#if defined(IPSEC)
/* Update an in-kernel tdb. Silently fail if no tdb is found. */
static void
pfsync_update_net_tdb(struct pfsync_tdb *pt)
{
	struct tdb		*tdb;
	int			 s;

	/* check for invalid values */
	if (ntohl(pt->spi) <= SPI_RESERVED_MAX ||
	    (pt->dst.sa.sa_family != AF_INET &&
	    pt->dst.sa.sa_family != AF_INET6))
		goto bad;

	tdb = gettdb(pt->spi, &pt->dst, pt->sproto);
	if (tdb) {
		pt->rpl = ntohl(pt->rpl);
		pt->cur_bytes = (unsigned long long)be64toh(pt->cur_bytes);

		/* Neither replay nor byte counter should ever decrease. */
		if (pt->rpl < tdb->tdb_rpl ||
		    pt->cur_bytes < tdb->tdb_cur_bytes) {
			goto bad;
		}

		tdb->tdb_rpl = pt->rpl;
		tdb->tdb_cur_bytes = pt->cur_bytes;
	}
	return;

bad:
	if (V_pf_status.debug >= PF_DEBUG_MISC)
		printf("pfsync_insert: PFSYNC_ACT_TDB_UPD: "
		    "invalid value\n");
	V_pfsyncstats.pfsyncs_badstate++;
	return;
}
#endif


static int
pfsync_in_eof(struct pfsync_pkt *pkt, struct mbuf *m, int offset, int count)
{
	/* check if we are at the right place in the packet */
	if (offset != m->m_pkthdr.len - sizeof(struct pfsync_eof))
		V_pfsyncstats.pfsyncs_badact++;

	/* we're done. free and let the caller return */
	m_freem(m);
	return (-1);
}

static int
pfsync_in_error(struct pfsync_pkt *pkt, struct mbuf *m, int offset, int count)
{
	V_pfsyncstats.pfsyncs_badact++;

	m_freem(m);
	return (-1);
}

static int
pfsyncoutput(struct ifnet *ifp, struct mbuf *m, struct sockaddr *dst,
	struct route *rt)
{
	m_freem(m);
	return (0);
}

/* ARGSUSED */
static int
pfsyncioctl(struct ifnet *ifp, u_long cmd, caddr_t data)
{
	struct pfsync_softc *sc = ifp->if_softc;
	struct ifreq *ifr = (struct ifreq *)data;
	struct pfsyncreq pfsyncr;
	int error;

	switch (cmd) {
	case SIOCSIFFLAGS:
		PFSYNC_LOCK(sc);
		if (ifp->if_flags & IFF_UP) {
			ifp->if_drv_flags |= IFF_DRV_RUNNING;
			PFSYNC_UNLOCK(sc);
			pfsync_pointers_init();
		} else {
			ifp->if_drv_flags &= ~IFF_DRV_RUNNING;
			PFSYNC_UNLOCK(sc);
			pfsync_pointers_uninit();
		}
		break;
	case SIOCSIFMTU:
		if (!sc->sc_sync_if ||
		    ifr->ifr_mtu <= PFSYNC_MINPKT ||
		    ifr->ifr_mtu > sc->sc_sync_if->if_mtu)
			return (EINVAL);
		if (ifr->ifr_mtu < ifp->if_mtu) {
			PFSYNC_LOCK(sc);
			if (sc->sc_len > PFSYNC_MINPKT)
				pfsync_sendout(1);
			PFSYNC_UNLOCK(sc);
		}
		ifp->if_mtu = ifr->ifr_mtu;
		break;
	case SIOCGETPFSYNC:
		bzero(&pfsyncr, sizeof(pfsyncr));
		PFSYNC_LOCK(sc);
		if (sc->sc_sync_if) {
			strlcpy(pfsyncr.pfsyncr_syncdev,
			    sc->sc_sync_if->if_xname, IFNAMSIZ);
		}
		pfsyncr.pfsyncr_syncpeer = sc->sc_sync_peer;
		pfsyncr.pfsyncr_maxupdates = sc->sc_maxupdates;
		pfsyncr.pfsyncr_defer = (PFSYNCF_DEFER ==
		    (sc->sc_flags & PFSYNCF_DEFER));
		PFSYNC_UNLOCK(sc);
		return (copyout(&pfsyncr, ifr->ifr_data, sizeof(pfsyncr)));

	case SIOCSETPFSYNC:
	    {
		struct ip_moptions *imo = &sc->sc_imo;
		struct ifnet *sifp;
		struct ip *ip;
		void *mship = NULL;

		if ((error = priv_check(curthread, PRIV_NETINET_PF)) != 0)
			return (error);
		if ((error = copyin(ifr->ifr_data, &pfsyncr, sizeof(pfsyncr))))
			return (error);

		if (pfsyncr.pfsyncr_maxupdates > 255)
			return (EINVAL);

		if (pfsyncr.pfsyncr_syncdev[0] == 0)
			sifp = NULL;
		else if ((sifp = ifunit_ref(pfsyncr.pfsyncr_syncdev)) == NULL)
			return (EINVAL);

		if (pfsyncr.pfsyncr_syncpeer.s_addr == 0 && sifp != NULL)
			mship = malloc((sizeof(struct in_multi *) *
			    IP_MIN_MEMBERSHIPS), M_PFSYNC, M_WAITOK | M_ZERO);

		PFSYNC_LOCK(sc);
		if (pfsyncr.pfsyncr_syncpeer.s_addr == 0)
			sc->sc_sync_peer.s_addr = htonl(INADDR_PFSYNC_GROUP);
		else
			sc->sc_sync_peer.s_addr =
			    pfsyncr.pfsyncr_syncpeer.s_addr;

		sc->sc_maxupdates = pfsyncr.pfsyncr_maxupdates;
		if (pfsyncr.pfsyncr_defer) {
			sc->sc_flags |= PFSYNCF_DEFER;
			pfsync_defer_ptr = pfsync_defer;
		} else {
			sc->sc_flags &= ~PFSYNCF_DEFER;
			pfsync_defer_ptr = NULL;
		}

		if (sifp == NULL) {
			if (sc->sc_sync_if)
				if_rele(sc->sc_sync_if);
			sc->sc_sync_if = NULL;
			if (imo->imo_membership)
				pfsync_multicast_cleanup(sc);
			PFSYNC_UNLOCK(sc);
			break;
		}

		if (sc->sc_len > PFSYNC_MINPKT &&
		    (sifp->if_mtu < sc->sc_ifp->if_mtu ||
		    (sc->sc_sync_if != NULL &&
		    sifp->if_mtu < sc->sc_sync_if->if_mtu) ||
		    sifp->if_mtu < MCLBYTES - sizeof(struct ip)))
			pfsync_sendout(1);

		if (imo->imo_membership)
			pfsync_multicast_cleanup(sc);

		if (sc->sc_sync_peer.s_addr == htonl(INADDR_PFSYNC_GROUP)) {
			error = pfsync_multicast_setup(sc, sifp, mship);
			if (error) {
				if_rele(sifp);
				free(mship, M_PFSYNC);
				return (error);
			}
		}
		if (sc->sc_sync_if)
			if_rele(sc->sc_sync_if);
		sc->sc_sync_if = sifp;

		ip = &sc->sc_template;
		bzero(ip, sizeof(*ip));
		ip->ip_v = IPVERSION;
		ip->ip_hl = sizeof(sc->sc_template) >> 2;
		ip->ip_tos = IPTOS_LOWDELAY;
		/* len and id are set later. */
		ip->ip_off = htons(IP_DF);
		ip->ip_ttl = PFSYNC_DFLTTL;
		ip->ip_p = IPPROTO_PFSYNC;
		ip->ip_src.s_addr = INADDR_ANY;
		ip->ip_dst.s_addr = sc->sc_sync_peer.s_addr;

		/* Request a full state table update. */
		if ((sc->sc_flags & PFSYNCF_OK) && carp_demote_adj_p)
			(*carp_demote_adj_p)(V_pfsync_carp_adj,
			    "pfsync bulk start");
		sc->sc_flags &= ~PFSYNCF_OK;
		if (V_pf_status.debug >= PF_DEBUG_MISC)
			printf("pfsync: requesting bulk update\n");
		pfsync_request_update(0, 0);
		PFSYNC_UNLOCK(sc);
		PFSYNC_BLOCK(sc);
		sc->sc_ureq_sent = time_uptime;
		callout_reset(&sc->sc_bulkfail_tmo, 5 * hz, pfsync_bulk_fail,
		    sc);
		PFSYNC_BUNLOCK(sc);

		break;
	    }
	default:
		return (ENOTTY);
	}

	return (0);
}

static void
pfsync_out_state(struct pf_state *st, void *buf)
{
	struct pfsync_state *sp = buf;

	pfsync_state_export(sp, st);
}

static void
pfsync_out_iack(struct pf_state *st, void *buf)
{
	struct pfsync_ins_ack *iack = buf;

	iack->id = st->id;
	iack->creatorid = st->creatorid;
}

static void
pfsync_out_upd_c(struct pf_state *st, void *buf)
{
	struct pfsync_upd_c *up = buf;

	bzero(up, sizeof(*up));
	up->id = st->id;
	pf_state_peer_hton(&st->src, &up->src);
	pf_state_peer_hton(&st->dst, &up->dst);
	up->creatorid = st->creatorid;
	up->timeout = st->timeout;
}

static void
pfsync_out_del(struct pf_state *st, void *buf)
{
	struct pfsync_del_c *dp = buf;

	dp->id = st->id;
	dp->creatorid = st->creatorid;
	st->state_flags |= PFSTATE_NOSYNC;
}

static void
pfsync_drop(struct pfsync_softc *sc)
{
	struct pf_state *st, *next;
	struct pfsync_upd_req_item *ur;
	int q;

	for (q = 0; q < PFSYNC_S_COUNT; q++) {
		if (TAILQ_EMPTY(&sc->sc_qs[q]))
			continue;

		TAILQ_FOREACH_SAFE(st, &sc->sc_qs[q], sync_list, next) {
			KASSERT(st->sync_state == q,
				("%s: st->sync_state == q",
					__func__));
			st->sync_state = PFSYNC_S_NONE;
			pf_release_state(st);
		}
		TAILQ_INIT(&sc->sc_qs[q]);
	}

	while ((ur = TAILQ_FIRST(&sc->sc_upd_req_list)) != NULL) {
		TAILQ_REMOVE(&sc->sc_upd_req_list, ur, ur_entry);
		free(ur, M_PFSYNC);
	}

	sc->sc_plus = NULL;
	sc->sc_len = PFSYNC_MINPKT;
}

static void
pfsync_sendout(int schedswi)
{
	struct pfsync_softc *sc = V_pfsyncif;
	struct ifnet *ifp = sc->sc_ifp;
	struct mbuf *m;
	struct ip *ip;
	struct pfsync_header *ph;
	struct pfsync_subheader *subh;
	struct pf_state *st;
	struct pfsync_upd_req_item *ur;
	int offset;
	int q, count = 0;

	KASSERT(sc != NULL, ("%s: null sc", __func__));
	KASSERT(sc->sc_len > PFSYNC_MINPKT,
	    ("%s: sc_len %zu", __func__, sc->sc_len));
	PFSYNC_LOCK_ASSERT(sc);

	if (ifp->if_bpf == NULL && sc->sc_sync_if == NULL) {
		pfsync_drop(sc);
		return;
	}

	m = m_get2(M_NOWAIT, MT_DATA, M_PKTHDR, max_linkhdr + sc->sc_len);
	if (m == NULL) {
		sc->sc_ifp->if_oerrors++;
		V_pfsyncstats.pfsyncs_onomem++;
		return;
	}
	m->m_data += max_linkhdr;
	m->m_len = m->m_pkthdr.len = sc->sc_len;

	/* build the ip header */
	ip = (struct ip *)m->m_data;
	bcopy(&sc->sc_template, ip, sizeof(*ip));
	offset = sizeof(*ip);

	ip->ip_len = htons(m->m_pkthdr.len);
	ip->ip_id = htons(ip_randomid());

	/* build the pfsync header */
	ph = (struct pfsync_header *)(m->m_data + offset);
	bzero(ph, sizeof(*ph));
	offset += sizeof(*ph);

	ph->version = PFSYNC_VERSION;
	ph->len = htons(sc->sc_len - sizeof(*ip));
	bcopy(V_pf_status.pf_chksum, ph->pfcksum, PF_MD5_DIGEST_LENGTH);

	/* walk the queues */
	for (q = 0; q < PFSYNC_S_COUNT; q++) {
		if (TAILQ_EMPTY(&sc->sc_qs[q]))
			continue;

		subh = (struct pfsync_subheader *)(m->m_data + offset);
		offset += sizeof(*subh);

		count = 0;
		TAILQ_FOREACH(st, &sc->sc_qs[q], sync_list) {
			KASSERT(st->sync_state == q,
				("%s: st->sync_state == q",
					__func__));
			if (st->timeout == PFTM_UNLINKED) {
				/*
				 * This happens if pfsync was once
				 * stopped, and then re-enabled
				 * after long time. Theoretically
				 * may happen at usual runtime, too.
				 */
				pf_release_state(st);
				continue;
			}
			/*
			 * XXXGL: some of write methods do unlocked reads
			 * of state data :(
			 */
			pfsync_qs[q].write(st, m->m_data + offset);
			offset += pfsync_qs[q].len;
			st->sync_state = PFSYNC_S_NONE;
			pf_release_state(st);
			count++;
		}
		TAILQ_INIT(&sc->sc_qs[q]);

		bzero(subh, sizeof(*subh));
		subh->action = pfsync_qs[q].action;
		subh->count = htons(count);
		V_pfsyncstats.pfsyncs_oacts[pfsync_qs[q].action] += count;
	}

	if (!TAILQ_EMPTY(&sc->sc_upd_req_list)) {
		subh = (struct pfsync_subheader *)(m->m_data + offset);
		offset += sizeof(*subh);

		count = 0;
		while ((ur = TAILQ_FIRST(&sc->sc_upd_req_list)) != NULL) {
			TAILQ_REMOVE(&sc->sc_upd_req_list, ur, ur_entry);

			bcopy(&ur->ur_msg, m->m_data + offset,
			    sizeof(ur->ur_msg));
			offset += sizeof(ur->ur_msg);
			free(ur, M_PFSYNC);
			count++;
		}

		bzero(subh, sizeof(*subh));
		subh->action = PFSYNC_ACT_UPD_REQ;
		subh->count = htons(count);
		V_pfsyncstats.pfsyncs_oacts[PFSYNC_ACT_UPD_REQ] += count;
	}

	/* has someone built a custom region for us to add? */
	if (sc->sc_plus != NULL) {
		bcopy(sc->sc_plus, m->m_data + offset, sc->sc_pluslen);
		offset += sc->sc_pluslen;

		sc->sc_plus = NULL;
	}

	subh = (struct pfsync_subheader *)(m->m_data + offset);
	offset += sizeof(*subh);

	bzero(subh, sizeof(*subh));
	subh->action = PFSYNC_ACT_EOF;
	subh->count = htons(1);
	V_pfsyncstats.pfsyncs_oacts[PFSYNC_ACT_EOF]++;

	/* XXX write checksum in EOF here */

	/* we're done, let's put it on the wire */
	if (ifp->if_bpf) {
		m->m_data += sizeof(*ip);
		m->m_len = m->m_pkthdr.len = sc->sc_len - sizeof(*ip);
		BPF_MTAP(ifp, m);
		m->m_data -= sizeof(*ip);
		m->m_len = m->m_pkthdr.len = sc->sc_len;
	}

	if (sc->sc_sync_if == NULL) {
		sc->sc_len = PFSYNC_MINPKT;
		m_freem(m);
		return;
	}

	sc->sc_ifp->if_opackets++;
	sc->sc_ifp->if_obytes += m->m_pkthdr.len;
	sc->sc_len = PFSYNC_MINPKT;

	if (!_IF_QFULL(&sc->sc_ifp->if_snd))
		_IF_ENQUEUE(&sc->sc_ifp->if_snd, m);
	else {
		m_freem(m);
		sc->sc_ifp->if_snd.ifq_drops++;
	}
	if (schedswi)
		swi_sched(V_pfsync_swi_cookie, 0);
}

static void
pfsync_insert_state(struct pf_state *st)
{
	struct pfsync_softc *sc = V_pfsyncif;

	if (st->state_flags & PFSTATE_NOSYNC)
		return;

	if ((st->rule.ptr->rule_flag & PFRULE_NOSYNC) ||
	    st->key[PF_SK_WIRE]->proto == IPPROTO_PFSYNC) {
		st->state_flags |= PFSTATE_NOSYNC;
		return;
	}

	KASSERT(st->sync_state == PFSYNC_S_NONE,
		("%s: st->sync_state == PFSYNC_S_NONE", __func__));

	PFSYNC_LOCK(sc);
	if (sc->sc_len == PFSYNC_MINPKT)
		callout_reset(&sc->sc_tmo, 1 * hz, pfsync_timeout, V_pfsyncif);

	pfsync_q_ins(st, PFSYNC_S_INS);
	PFSYNC_UNLOCK(sc);

	st->sync_updates = 0;
}

static int
pfsync_defer(struct pf_state *st, struct mbuf *m)
{
	struct pfsync_softc *sc = V_pfsyncif;
	struct pfsync_deferral *pd;

	if (m->m_flags & (M_BCAST|M_MCAST))
		return (0);

	PFSYNC_LOCK(sc);

	if (sc == NULL || !(sc->sc_ifp->if_flags & IFF_DRV_RUNNING) ||
	    !(sc->sc_flags & PFSYNCF_DEFER)) {
		PFSYNC_UNLOCK(sc);
		return (0);
	}

	 if (sc->sc_deferred >= 128)
		pfsync_undefer(TAILQ_FIRST(&sc->sc_deferrals), 0);

	pd = malloc(sizeof(*pd), M_PFSYNC, M_NOWAIT);
	if (pd == NULL)
		return (0);
	sc->sc_deferred++;

	m->m_flags |= M_SKIP_FIREWALL;
	st->state_flags |= PFSTATE_ACK;

	pd->pd_sc = sc;
	pd->pd_refs = 0;
	pd->pd_st = st;
	pf_ref_state(st);
	pd->pd_m = m;

	TAILQ_INSERT_TAIL(&sc->sc_deferrals, pd, pd_entry);
	callout_init_mtx(&pd->pd_tmo, &sc->sc_mtx, CALLOUT_RETURNUNLOCKED);
	callout_reset(&pd->pd_tmo, 10, pfsync_defer_tmo, pd);

	pfsync_push(sc);

	return (1);
}

static void
pfsync_undefer(struct pfsync_deferral *pd, int drop)
{
	struct pfsync_softc *sc = pd->pd_sc;
	struct mbuf *m = pd->pd_m;
	struct pf_state *st = pd->pd_st;

	PFSYNC_LOCK_ASSERT(sc);

	TAILQ_REMOVE(&sc->sc_deferrals, pd, pd_entry);
	sc->sc_deferred--;
	pd->pd_st->state_flags &= ~PFSTATE_ACK;	/* XXX: locking! */
	free(pd, M_PFSYNC);
	pf_release_state(st);

	if (drop)
		m_freem(m);
	else {
		_IF_ENQUEUE(&sc->sc_ifp->if_snd, m);
		pfsync_push(sc);
	}
}

static void
pfsync_defer_tmo(void *arg)
{
	struct pfsync_deferral *pd = arg;
	struct pfsync_softc *sc = pd->pd_sc;
	struct mbuf *m = pd->pd_m;
	struct pf_state *st = pd->pd_st;

	PFSYNC_LOCK_ASSERT(sc);

	CURVNET_SET(m->m_pkthdr.rcvif->if_vnet);

	TAILQ_REMOVE(&sc->sc_deferrals, pd, pd_entry);
	sc->sc_deferred--;
	pd->pd_st->state_flags &= ~PFSTATE_ACK;	/* XXX: locking! */
	if (pd->pd_refs == 0)
		free(pd, M_PFSYNC);
	PFSYNC_UNLOCK(sc);

	ip_output(m, NULL, NULL, 0, NULL, NULL);

	pf_release_state(st);

	CURVNET_RESTORE();
}

static void
pfsync_undefer_state(struct pf_state *st, int drop)
{
	struct pfsync_softc *sc = V_pfsyncif;
	struct pfsync_deferral *pd;

	PFSYNC_LOCK_ASSERT(sc);

	TAILQ_FOREACH(pd, &sc->sc_deferrals, pd_entry) {
		 if (pd->pd_st == st) {
			if (callout_stop(&pd->pd_tmo))
				pfsync_undefer(pd, drop);
			return;
		}
	}

	panic("%s: unable to find deferred state", __func__);
}

static void
pfsync_update_state(struct pf_state *st)
{
	struct pfsync_softc *sc = V_pfsyncif;
	int sync = 0;

	PF_STATE_LOCK_ASSERT(st);
	PFSYNC_LOCK(sc);

	if (st->state_flags & PFSTATE_ACK)
		pfsync_undefer_state(st, 0);
	if (st->state_flags & PFSTATE_NOSYNC) {
		if (st->sync_state != PFSYNC_S_NONE)
			pfsync_q_del(st);
		PFSYNC_UNLOCK(sc);
		return;
	}

	if (sc->sc_len == PFSYNC_MINPKT)
		callout_reset(&sc->sc_tmo, 1 * hz, pfsync_timeout, V_pfsyncif);

	switch (st->sync_state) {
	case PFSYNC_S_UPD_C:
	case PFSYNC_S_UPD:
	case PFSYNC_S_INS:
		/* we're already handling it */

		if (st->key[PF_SK_WIRE]->proto == IPPROTO_TCP) {
			st->sync_updates++;
			if (st->sync_updates >= sc->sc_maxupdates)
				sync = 1;
		}
		break;

	case PFSYNC_S_IACK:
		pfsync_q_del(st);
	case PFSYNC_S_NONE:
		pfsync_q_ins(st, PFSYNC_S_UPD_C);
		st->sync_updates = 0;
		break;

	default:
		panic("%s: unexpected sync state %d", __func__, st->sync_state);
	}

	if (sync || (time_uptime - st->pfsync_time) < 2)
		pfsync_push(sc);

	PFSYNC_UNLOCK(sc);
}

static void
pfsync_request_update(u_int32_t creatorid, u_int64_t id)
{
	struct pfsync_softc *sc = V_pfsyncif;
	struct pfsync_upd_req_item *item;
	size_t nlen = sizeof(struct pfsync_upd_req);

	PFSYNC_LOCK_ASSERT(sc);

	/*
	 * This code does a bit to prevent multiple update requests for the
	 * same state being generated. It searches current subheader queue,
	 * but it doesn't lookup into queue of already packed datagrams.
	 */
	TAILQ_FOREACH(item, &sc->sc_upd_req_list, ur_entry)
		if (item->ur_msg.id == id &&
		    item->ur_msg.creatorid == creatorid)
			return;

	item = malloc(sizeof(*item), M_PFSYNC, M_NOWAIT);
	if (item == NULL)
		return; /* XXX stats */

	item->ur_msg.id = id;
	item->ur_msg.creatorid = creatorid;

	if (TAILQ_EMPTY(&sc->sc_upd_req_list))
		nlen += sizeof(struct pfsync_subheader);

	if (sc->sc_len + nlen > sc->sc_ifp->if_mtu) {
		pfsync_sendout(1);

		nlen = sizeof(struct pfsync_subheader) +
		    sizeof(struct pfsync_upd_req);
	}

	TAILQ_INSERT_TAIL(&sc->sc_upd_req_list, item, ur_entry);
	sc->sc_len += nlen;
}

static void
pfsync_update_state_req(struct pf_state *st)
{
	struct pfsync_softc *sc = V_pfsyncif;

	PF_STATE_LOCK_ASSERT(st);
	PFSYNC_LOCK(sc);

	if (st->state_flags & PFSTATE_NOSYNC) {
		if (st->sync_state != PFSYNC_S_NONE)
			pfsync_q_del(st);
		PFSYNC_UNLOCK(sc);
		return;
	}

	switch (st->sync_state) {
	case PFSYNC_S_UPD_C:
	case PFSYNC_S_IACK:
		pfsync_q_del(st);
	case PFSYNC_S_NONE:
		pfsync_q_ins(st, PFSYNC_S_UPD);
		pfsync_push(sc);
		break;

	case PFSYNC_S_INS:
	case PFSYNC_S_UPD:
	case PFSYNC_S_DEL:
		/* we're already handling it */
		break;

	default:
		panic("%s: unexpected sync state %d", __func__, st->sync_state);
	}

	PFSYNC_UNLOCK(sc);
}

static void
pfsync_delete_state(struct pf_state *st)
{
	struct pfsync_softc *sc = V_pfsyncif;

	PFSYNC_LOCK(sc);
	if (st->state_flags & PFSTATE_ACK)
		pfsync_undefer_state(st, 1);
	if (st->state_flags & PFSTATE_NOSYNC) {
		if (st->sync_state != PFSYNC_S_NONE)
			pfsync_q_del(st);
		PFSYNC_UNLOCK(sc);
		return;
	}

	if (sc->sc_len == PFSYNC_MINPKT)
		callout_reset(&sc->sc_tmo, 1 * hz, pfsync_timeout, V_pfsyncif);

	switch (st->sync_state) {
	case PFSYNC_S_INS:
		/* We never got to tell the world so just forget about it. */
		pfsync_q_del(st);
		break;

	case PFSYNC_S_UPD_C:
	case PFSYNC_S_UPD:
	case PFSYNC_S_IACK:
		pfsync_q_del(st);
		/* FALLTHROUGH to putting it on the del list */

	case PFSYNC_S_NONE:
		pfsync_q_ins(st, PFSYNC_S_DEL);
		break;

	default:
		panic("%s: unexpected sync state %d", __func__, st->sync_state);
	}
	PFSYNC_UNLOCK(sc);
}

static void
pfsync_clear_states(u_int32_t creatorid, const char *ifname)
{
	struct pfsync_softc *sc = V_pfsyncif;
	struct {
		struct pfsync_subheader subh;
		struct pfsync_clr clr;
	} __packed r;

	bzero(&r, sizeof(r));

	r.subh.action = PFSYNC_ACT_CLR;
	r.subh.count = htons(1);
	V_pfsyncstats.pfsyncs_oacts[PFSYNC_ACT_CLR]++;

	strlcpy(r.clr.ifname, ifname, sizeof(r.clr.ifname));
	r.clr.creatorid = creatorid;

	PFSYNC_LOCK(sc);
	pfsync_send_plus(&r, sizeof(r));
	PFSYNC_UNLOCK(sc);
}

static void
pfsync_q_ins(struct pf_state *st, int q)
{
	struct pfsync_softc *sc = V_pfsyncif;
	size_t nlen = pfsync_qs[q].len;

	PFSYNC_LOCK_ASSERT(sc);

	KASSERT(st->sync_state == PFSYNC_S_NONE,
		("%s: st->sync_state == PFSYNC_S_NONE", __func__));
	KASSERT(sc->sc_len >= PFSYNC_MINPKT, ("pfsync pkt len is too low %zu",
	    sc->sc_len));

	if (TAILQ_EMPTY(&sc->sc_qs[q]))
		nlen += sizeof(struct pfsync_subheader);

	if (sc->sc_len + nlen > sc->sc_ifp->if_mtu) {
		pfsync_sendout(1);

		nlen = sizeof(struct pfsync_subheader) + pfsync_qs[q].len;
	}

	sc->sc_len += nlen;
	TAILQ_INSERT_TAIL(&sc->sc_qs[q], st, sync_list);
	st->sync_state = q;
	pf_ref_state(st);
}

static void
pfsync_q_del(struct pf_state *st)
{
	struct pfsync_softc *sc = V_pfsyncif;
	int q = st->sync_state;

	PFSYNC_LOCK_ASSERT(sc);
	KASSERT(st->sync_state != PFSYNC_S_NONE,
		("%s: st->sync_state != PFSYNC_S_NONE", __func__));

	sc->sc_len -= pfsync_qs[q].len;
	TAILQ_REMOVE(&sc->sc_qs[q], st, sync_list);
	st->sync_state = PFSYNC_S_NONE;
	pf_release_state(st);

	if (TAILQ_EMPTY(&sc->sc_qs[q]))
		sc->sc_len -= sizeof(struct pfsync_subheader);
}

static void
pfsync_bulk_start(void)
{
	struct pfsync_softc *sc = V_pfsyncif;

	if (V_pf_status.debug >= PF_DEBUG_MISC)
		printf("pfsync: received bulk update request\n");

	PFSYNC_BLOCK(sc);

	sc->sc_ureq_received = time_uptime;
	sc->sc_bulk_hashid = 0;
	sc->sc_bulk_stateid = 0;
	pfsync_bulk_status(PFSYNC_BUS_START);
	callout_reset(&sc->sc_bulk_tmo, 1, pfsync_bulk_update, sc);
	PFSYNC_BUNLOCK(sc);
}

static void
pfsync_bulk_update(void *arg)
{
	struct pfsync_softc *sc = arg;
	struct pf_state *s;
	int i, sent = 0;

	PFSYNC_BLOCK_ASSERT(sc);
	CURVNET_SET(sc->sc_ifp->if_vnet);

	/*
	 * Start with last state from previous invocation.
	 * It may had gone, in this case start from the
	 * hash slot.
	 */
	s = pf_find_state_byid(sc->sc_bulk_stateid, sc->sc_bulk_creatorid);

	if (s != NULL)
		i = PF_IDHASH(s);
	else
		i = sc->sc_bulk_hashid;

	for (; i <= V_pf_hashmask; i++) {
		struct pf_idhash *ih = &V_pf_idhash[i];

		if (s != NULL)
			PF_HASHROW_ASSERT(ih);
		else {
			PF_HASHROW_LOCK(ih);
			s = LIST_FIRST(&ih->states);
		}

		for (; s; s = LIST_NEXT(s, entry)) {

			if (sent > 1 && (sc->sc_ifp->if_mtu - sc->sc_len) <
			    sizeof(struct pfsync_state)) {
				/* We've filled a packet. */
				sc->sc_bulk_hashid = i;
				sc->sc_bulk_stateid = s->id;
				sc->sc_bulk_creatorid = s->creatorid;
				PF_HASHROW_UNLOCK(ih);
				callout_reset(&sc->sc_bulk_tmo, 1,
				    pfsync_bulk_update, sc);
				goto full;
			}

			if (s->sync_state == PFSYNC_S_NONE &&
			    s->timeout < PFTM_MAX &&
			    s->pfsync_time <= sc->sc_ureq_received) {
				PFSYNC_LOCK(sc);
				pfsync_update_state_req(s);
				PFSYNC_UNLOCK(sc);
				sent++;
			}
		}
		PF_HASHROW_UNLOCK(ih);
	}

	/* We're done. */
	pfsync_bulk_status(PFSYNC_BUS_END);

full:
	CURVNET_RESTORE();
}

static void
pfsync_bulk_status(u_int8_t status)
{
	struct {
		struct pfsync_subheader subh;
		struct pfsync_bus bus;
	} __packed r;

	struct pfsync_softc *sc = V_pfsyncif;

	bzero(&r, sizeof(r));

	r.subh.action = PFSYNC_ACT_BUS;
	r.subh.count = htons(1);
	V_pfsyncstats.pfsyncs_oacts[PFSYNC_ACT_BUS]++;

	r.bus.creatorid = V_pf_status.hostid;
	r.bus.endtime = htonl(time_uptime - sc->sc_ureq_received);
	r.bus.status = status;

	PFSYNC_LOCK(sc);
	pfsync_send_plus(&r, sizeof(r));
	PFSYNC_UNLOCK(sc);
}

static void
pfsync_bulk_fail(void *arg)
{
	struct pfsync_softc *sc = arg;

	CURVNET_SET(sc->sc_ifp->if_vnet);

	PFSYNC_BLOCK_ASSERT(sc);

	if (sc->sc_bulk_tries++ < PFSYNC_MAX_BULKTRIES) {
		/* Try again */
		callout_reset(&sc->sc_bulkfail_tmo, 5 * hz,
		    pfsync_bulk_fail, V_pfsyncif);
		PFSYNC_LOCK(sc);
		pfsync_request_update(0, 0);
		PFSYNC_UNLOCK(sc);
	} else {
		/* Pretend like the transfer was ok. */
		sc->sc_ureq_sent = 0;
		sc->sc_bulk_tries = 0;
		PFSYNC_LOCK(sc);
		if (!(sc->sc_flags & PFSYNCF_OK) && carp_demote_adj_p)
			(*carp_demote_adj_p)(-V_pfsync_carp_adj,
			    "pfsync bulk fail");
		sc->sc_flags |= PFSYNCF_OK;
		PFSYNC_UNLOCK(sc);
		if (V_pf_status.debug >= PF_DEBUG_MISC)
			printf("pfsync: failed to receive bulk update\n");
	}

	CURVNET_RESTORE();
}

static void
pfsync_send_plus(void *plus, size_t pluslen)
{
	struct pfsync_softc *sc = V_pfsyncif;

	PFSYNC_LOCK_ASSERT(sc);

	if (sc->sc_len + pluslen > sc->sc_ifp->if_mtu)
		pfsync_sendout(1);

	sc->sc_plus = plus;
	sc->sc_len += (sc->sc_pluslen = pluslen);

	pfsync_sendout(1);
}

static void
pfsync_timeout(void *arg)
{
	struct pfsync_softc *sc = arg;

	CURVNET_SET(sc->sc_ifp->if_vnet);
	PFSYNC_LOCK(sc);
	pfsync_push(sc);
	PFSYNC_UNLOCK(sc);
	CURVNET_RESTORE();
}

static void
pfsync_push(struct pfsync_softc *sc)
{

	PFSYNC_LOCK_ASSERT(sc);

	sc->sc_flags |= PFSYNCF_PUSH;
	swi_sched(V_pfsync_swi_cookie, 0);
}

static void
pfsyncintr(void *arg)
{
	struct pfsync_softc *sc = arg;
	struct mbuf *m, *n;

	CURVNET_SET(sc->sc_ifp->if_vnet);

	PFSYNC_LOCK(sc);
	if ((sc->sc_flags & PFSYNCF_PUSH) && sc->sc_len > PFSYNC_MINPKT) {
		pfsync_sendout(0);
		sc->sc_flags &= ~PFSYNCF_PUSH;
	}
	_IF_DEQUEUE_ALL(&sc->sc_ifp->if_snd, m);
	PFSYNC_UNLOCK(sc);

	for (; m != NULL; m = n) {

		n = m->m_nextpkt;
		m->m_nextpkt = NULL;

		/*
		 * We distinguish between a deferral packet and our
		 * own pfsync packet based on M_SKIP_FIREWALL
		 * flag. This is XXX.
		 */
		if (m->m_flags & M_SKIP_FIREWALL)
			ip_output(m, NULL, NULL, 0, NULL, NULL);
		else if (ip_output(m, NULL, NULL, IP_RAWOUTPUT, &sc->sc_imo,
		    NULL) == 0)
			V_pfsyncstats.pfsyncs_opackets++;
		else
			V_pfsyncstats.pfsyncs_oerrors++;
	}
	CURVNET_RESTORE();
}

static int
pfsync_multicast_setup(struct pfsync_softc *sc, struct ifnet *ifp, void *mship)
{
	struct ip_moptions *imo = &sc->sc_imo;
	int error;

	if (!(ifp->if_flags & IFF_MULTICAST))
		return (EADDRNOTAVAIL);

	imo->imo_membership = (struct in_multi **)mship;
	imo->imo_max_memberships = IP_MIN_MEMBERSHIPS;
	imo->imo_multicast_vif = -1;

	if ((error = in_joingroup(ifp, &sc->sc_sync_peer, NULL,
	    &imo->imo_membership[0])) != 0) {
		imo->imo_membership = NULL;
		return (error);
	}
	imo->imo_num_memberships++;
	imo->imo_multicast_ifp = ifp;
	imo->imo_multicast_ttl = PFSYNC_DFLTTL;
	imo->imo_multicast_loop = 0;

	return (0);
}

static void
pfsync_multicast_cleanup(struct pfsync_softc *sc)
{
	struct ip_moptions *imo = &sc->sc_imo;

	in_leavegroup(imo->imo_membership[0], NULL);
	free(imo->imo_membership, M_PFSYNC);
	imo->imo_membership = NULL;
	imo->imo_multicast_ifp = NULL;
}

#ifdef INET
extern  struct domain inetdomain;
static struct protosw in_pfsync_protosw = {
	.pr_type =		SOCK_RAW,
	.pr_domain =		&inetdomain,
	.pr_protocol =		IPPROTO_PFSYNC,
	.pr_flags =		PR_ATOMIC|PR_ADDR,
	.pr_input =		pfsync_input,
	.pr_output =		(pr_output_t *)rip_output,
	.pr_ctloutput =		rip_ctloutput,
	.pr_usrreqs =		&rip_usrreqs
};
#endif

static void
pfsync_pointers_init()
{

	PF_RULES_WLOCK();
	pfsync_state_import_ptr = pfsync_state_import;
	pfsync_insert_state_ptr = pfsync_insert_state;
	pfsync_update_state_ptr = pfsync_update_state;
	pfsync_delete_state_ptr = pfsync_delete_state;
	pfsync_clear_states_ptr = pfsync_clear_states;
	pfsync_defer_ptr = pfsync_defer;
	PF_RULES_WUNLOCK();
}

static void
pfsync_pointers_uninit()
{

	PF_RULES_WLOCK();
	pfsync_state_import_ptr = NULL;
	pfsync_insert_state_ptr = NULL;
	pfsync_update_state_ptr = NULL;
	pfsync_delete_state_ptr = NULL;
	pfsync_clear_states_ptr = NULL;
	pfsync_defer_ptr = NULL;
	PF_RULES_WUNLOCK();
}

static int
pfsync_init()
{
	VNET_ITERATOR_DECL(vnet_iter);
	int error = 0;

	VNET_LIST_RLOCK();
	VNET_FOREACH(vnet_iter) {
		CURVNET_SET(vnet_iter);
		V_pfsync_cloner = if_clone_simple(pfsyncname,
		    pfsync_clone_create, pfsync_clone_destroy, 1);
		error = swi_add(NULL, pfsyncname, pfsyncintr, V_pfsyncif,
		    SWI_NET, INTR_MPSAFE, &V_pfsync_swi_cookie);
		CURVNET_RESTORE();
		if (error)
			goto fail_locked;
	}
	VNET_LIST_RUNLOCK();
#ifdef INET
	error = pf_proto_register(PF_INET, &in_pfsync_protosw);
	if (error)
		goto fail;
	error = ipproto_register(IPPROTO_PFSYNC);
	if (error) {
		pf_proto_unregister(PF_INET, IPPROTO_PFSYNC, SOCK_RAW);
		goto fail;
	}
#endif
	pfsync_pointers_init();

	return (0);

fail:
	VNET_LIST_RLOCK();
fail_locked:
	VNET_FOREACH(vnet_iter) {
		CURVNET_SET(vnet_iter);
		if (V_pfsync_swi_cookie) {
			swi_remove(V_pfsync_swi_cookie);
			if_clone_detach(V_pfsync_cloner);
		}
		CURVNET_RESTORE();
	}
	VNET_LIST_RUNLOCK();

	return (error);
}

static void
pfsync_uninit()
{
	VNET_ITERATOR_DECL(vnet_iter);

	pfsync_pointers_uninit();

	ipproto_unregister(IPPROTO_PFSYNC);
	pf_proto_unregister(PF_INET, IPPROTO_PFSYNC, SOCK_RAW);
	VNET_LIST_RLOCK();
	VNET_FOREACH(vnet_iter) {
		CURVNET_SET(vnet_iter);
		if_clone_detach(V_pfsync_cloner);
		swi_remove(V_pfsync_swi_cookie);
		CURVNET_RESTORE();
	}
	VNET_LIST_RUNLOCK();
}

static int
pfsync_modevent(module_t mod, int type, void *data)
{
	int error = 0;

	switch (type) {
	case MOD_LOAD:
		error = pfsync_init();
		break;
	case MOD_QUIESCE:
		/*
		 * Module should not be unloaded due to race conditions.
		 */
		error = EBUSY;
		break;
	case MOD_UNLOAD:
		pfsync_uninit();
		break;
	default:
		error = EINVAL;
		break;
	}

	return (error);
}

static moduledata_t pfsync_mod = {
	pfsyncname,
	pfsync_modevent,
	0
};

#define PFSYNC_MODVER 1

DECLARE_MODULE(pfsync, pfsync_mod, SI_SUB_PROTO_DOMAIN, SI_ORDER_ANY);
MODULE_VERSION(pfsync, PFSYNC_MODVER);
MODULE_DEPEND(pfsync, pf, PF_MODVER, PF_MODVER, PF_MODVER);