/* LWIP service - ndev.c - network driver communication module */
/*
 * There is almost a one-to-one mapping between network device driver (ndev)
 * objects and ethernet interface (ethif) objects, with as major difference
 * that there may be an ndev object but not an ethif object for a driver that
 * is known to exist but has not yet replied to our initialization request:
 * without the information from the initialization request, there is no point
 * creating an ethif object just yet, while we do need to track the driver
 * process.  TODO: it would be nice if unanswered init requests timed out and
 * caused the removal of the ndev object after a while.
 *
 * Beyond that, this module aims to abstract away the low-level details of
 * communication, memory grants, and driver restarts.  Driver restarts are not
 * fully transparent to the ethif module because it needs to reinitialize
 * driver state only it knows about after a restart.  Drivers that are in the
 * process of restarting and therefore not operational are said to be disabled.
 *
 * From this module's point of view, a network driver is one of two states:
 * initializing, where it has yet to respond to our initialization request, and
 * active, where it is expected to accept and respond to all other requests.
 * This module does not keep track of higher-level states and rules however;
 * that is left to the ethif layer on one side, and the network driver itself
 * on the other side.  One important example is the interface being up or down:
 * the ndev layer will happily forward send and receive requests when the
 * interface is down, but these requests will be (resp.) dropped and rejected
 * by the network driver in that state, and will not be generated by the ethif
 * layer when the layer is down.  Imposing barriers between configure and send
 * requests is also left to the other parties.
 *
 * In this module, each active network driver has a send queue and a receive
 * queue.  The send queue is shared for packet send requests and configuration
 * change requests.  The receive queue is used for packet receive requests
 * only.  Each queue has a maximum depth, which is the minimum of a value
 * provided by the network driver during initialization and local restrictions.
 * These local restrictions are different for the two queue types: the receive
 * queue is always bounded to a hardcoded value, while the send queue has a
 * guaranteed minimum depth but may use up to the driver's maximum using spare
 * entries.  For both, a minimum depth is always available, since it is not
 * possible to cancel individual send or receive requests after they have been
 * sent to a particular driver.  This does mean that we necessarily waste a
 * large number of request structures in the common case.
 *
 * The general API model does not support the notion of blocking calls.  While
 * it would make sense to retrieve e.g. error statistics from the driver only
 * when requested by userland, implementing this without threads would be
 * seriously complicated, because such requests can have many origins (ioctl,
 * PF_ROUTE message, sysctl).  Instead, we rely on drivers updating us with the
 * latest information on everything at all times, so that we can hand over a
 * cached copy of (e.g.) those error statistics right away.  We provide a means
 * for drivers to perform rate limiting of such status updates (to prevent
 * overflowing asynsend queues), by replying to these status messages.  That
 * means that there is a request-response combo going in the opposite direction
 * of the regular messages.
 *
 * TODO: in the future we will want to obtain the list of supported media modes
 * (IFM_) from drivers, so that userland can view the list.  Given the above
 * model, the easiest way would be to obtain a copy of the full list, limited
 * to a configured number of entries, at driver initialization time.  This
 * would require that the initialization request also involve a memory grant.
 *
 * If necessary, it would not be too much work to split off this module into
 * its own libndev library.  For now, there is no point in doing this and the
 * tighter coupling allows us to optimize just a little but (see pbuf usage).
 */

#include "lwip.h"
#include "ndev.h"
#include "ethif.h"

#define LABEL_MAX	16	/* FIXME: this should be in a system header */

#define NDEV_SENDQ	2	/* minimum guaranteed send queue depth */
#define NDEV_RECVQ	2	/* guaranteed receive queue depth */
#define NREQ_SPARES	8	/* spare send queue (request) objects */
#define NR_NREQ		((NDEV_SENDQ + NDEV_RECVQ) * NR_NDEV + NREQ_SPARES)

static SIMPLEQ_HEAD(, ndev_req) nreq_freelist;

static struct ndev_req {
	SIMPLEQ_ENTRY(ndev_req) nreq_next;	/* next request in queue */
	int nreq_type;				/* type of request message */
	cp_grant_id_t nreq_grant[NDEV_IOV_MAX];	/* grants for request */
} nreq_array[NR_NREQ];

static unsigned int nreq_spares;	/* number of free spare objects */

struct ndev_queue {
	uint32_t nq_head;		/* ID of oldest pending request */
	uint8_t nq_count;		/* current nr of pending requests */
	uint8_t nq_max;			/* maximum nr of pending requests */
	SIMPLEQ_HEAD(, ndev_req) nq_req; /* queue of pending requests */
};

static struct ndev {
	endpoint_t ndev_endpt;		/* driver endpoint */
	char ndev_label[LABEL_MAX];	/* driver label */
	struct ethif *ndev_ethif;	/* ethif object, or NULL if init'ing */
	struct ndev_queue ndev_sendq;	/* packet send and configure queue */
	struct ndev_queue ndev_recvq;	/* packet receive queue */
} ndev_array[NR_NDEV];

static ndev_id_t ndev_max;		/* highest driver count ever seen */

/*
 * This macro checks whether the network driver is active rather than
 * initializing.  See above for more information.
 */
#define NDEV_ACTIVE(ndev)	((ndev)->ndev_sendq.nq_max > 0)

static int ndev_pending;		/* number of initializing drivers */

/* The CTL_MINIX MINIX_LWIP "drivers" subtree.  Dynamically numbered. */
static struct rmib_node minix_lwip_drivers_table[] = {
	RMIB_INTPTR(RMIB_RO, &ndev_pending, "pending",
	    "Number of drivers currently initializing"),
};

static struct rmib_node minix_lwip_drivers_node =
    RMIB_NODE(RMIB_RO, minix_lwip_drivers_table, "drivers",
	"Network driver information");

/*
 * Initialize the network driver communication module.
 */
void
ndev_init(void)
{
	unsigned int slot;
	int r;

	/* Initialize local variables. */
	ndev_max = 0;

	SIMPLEQ_INIT(&nreq_freelist);

	for (slot = 0; slot < __arraycount(nreq_array); slot++)
		SIMPLEQ_INSERT_TAIL(&nreq_freelist, &nreq_array[slot],
		    nreq_next);

	nreq_spares = NREQ_SPARES;

	/*
	 * Preallocate the total number of grants that we could possibly need
	 * concurrently.  Even though it is extremely unlikely that we will
	 * ever need that many grants in practice, the alternative is runtime
	 * dynamic memory (re)allocation which is something we prefer to avoid
	 * altogether.  At time of writing, we end up preallocating 320 grants
	 * using up a total of a bit under 9KB of memory.
	 */
	cpf_prealloc(NR_NREQ * NDEV_IOV_MAX);


	/*
	 * Not needed, just for ultimate safety: start off all queues with
	 * wildly different request sequence numbers, to minimize the chance
	 * that any two replies will ever be confused.
	 */
	for (slot = 0; slot < __arraycount(ndev_array); slot++) {
		ndev_array[slot].ndev_sendq.nq_head = slot << 21;
		ndev_array[slot].ndev_recvq.nq_head = (slot * 2 + 1) << 20;
	}

	/* Subscribe to Data Store (DS) events from network drivers. */
	if ((r = ds_subscribe("drv\\.net\\..*",
	    DSF_INITIAL | DSF_OVERWRITE)) != OK)
		panic("unable to subscribe to driver events: %d", r);

	/*
	 * Keep track of how many drivers are in "pending" state, which means
	 * that they have not yet replied to our initialization request.
	 */
	ndev_pending = 0;

	/* Register the minix.lwip.drivers subtree. */
	mibtree_register_lwip(&minix_lwip_drivers_node);
}

/*
 * Initialize a queue for first use.
 */
static void
ndev_queue_init(struct ndev_queue * nq)
{

	/*
	 * Only ever increase sequence numbers, to minimize the chance that
	 * two (e.g. from different driver instances) happen to be the same.
	 */
	nq->nq_head++;

	nq->nq_count = 0;
	nq->nq_max = 0;
	SIMPLEQ_INIT(&nq->nq_req);
}

/*
 * Advance the given request queue, freeing up the request at the head of the
 * queue including any grants in use for it.
 */
static void
ndev_queue_advance(struct ndev_queue * nq)
{
	struct ndev_req * nreq;
	cp_grant_id_t grant;
	unsigned int i;

	nreq = SIMPLEQ_FIRST(&nq->nq_req);

	for (i = 0; i < __arraycount(nreq->nreq_grant); i++) {
		grant = nreq->nreq_grant[i];

		if (!GRANT_VALID(grant))
			break;

		/* TODO: make the safecopies code stop using errno. */
		if (cpf_revoke(grant) != 0)
			panic("unable to revoke grant: %d", -errno);
	}

	if (nreq->nreq_type != NDEV_RECV && nq->nq_count > NDEV_SENDQ) {
		nreq_spares++;

		assert(nreq_spares <= NREQ_SPARES);
	}

	SIMPLEQ_REMOVE_HEAD(&nq->nq_req, nreq_next);

	SIMPLEQ_INSERT_HEAD(&nreq_freelist, nreq, nreq_next);

	nq->nq_head++;
	nq->nq_count--;
}

/*
 * Clear any outstanding requests from the given queue and reset it to a
 * pre-initialization state.
 */
static void
ndev_queue_reset(struct ndev_queue * nq)
{

	while (nq->nq_count > 0) {
		assert(!SIMPLEQ_EMPTY(&nq->nq_req));

		ndev_queue_advance(nq);
	}

	nq->nq_max = 0;
}

/*
 * Obtain a request object for use in a new request.  Return the request
 * object, with its request type field set to 'type', and with the request
 * sequence ID returned in 'seq'.  Return NULL if no request objects are
 * available for the given request type.  If the caller does send off the
 * request, a call to ndev_queue_add() must follow immediately after.  If the
 * caller fails to send off the request for other reasons, it need not do
 * anything: this function does not perform any actions that need to be undone.
 */
static struct ndev_req *
ndev_queue_get(struct ndev_queue * nq, int type, uint32_t * seq)
{
	struct ndev_req *nreq;

	/* Has the hard queue depth limit been reached? */
	if (nq->nq_count == nq->nq_max)
		return NULL;

	/*
	 * For send requests, we may use request objects from a shared "spares"
	 * pool, if available.
	 */
	if (type != NDEV_RECV && nq->nq_count >= NDEV_SENDQ &&
	    nreq_spares == 0)
		return NULL;

	assert(!SIMPLEQ_EMPTY(&nreq_freelist));
	nreq = SIMPLEQ_FIRST(&nreq_freelist);

	nreq->nreq_type = type;

	*seq = nq->nq_head + nq->nq_count;

	return nreq;
}

/*
 * Add a successfully sent request to the given queue.  The request must have
 * been obtained using ndev_queue_get() directly before the call to this
 * function.  This function never fails.
 */
static void
ndev_queue_add(struct ndev_queue * nq, struct ndev_req * nreq)
{

	if (nreq->nreq_type != NDEV_RECV && nq->nq_count >= NDEV_SENDQ) {
		assert(nreq_spares > 0);

		nreq_spares--;
	}

	SIMPLEQ_REMOVE_HEAD(&nreq_freelist, nreq_next);

	SIMPLEQ_INSERT_TAIL(&nq->nq_req, nreq, nreq_next);

	nq->nq_count++;
}

/*
 * Remove the head of the given request queue, but only if it matches the given
 * request type and sequence ID.  Return TRUE if the head was indeed removed,
 * or FALSE if the head of the request queue (if any) did not match the given
 * type and/or sequence ID.
 */
static int
ndev_queue_remove(struct ndev_queue * nq, int type, uint32_t seq)
{
	struct ndev_req *nreq;

	if (nq->nq_count < 1 || nq->nq_head != seq)
		return FALSE;

	assert(!SIMPLEQ_EMPTY(&nq->nq_req));
	nreq = SIMPLEQ_FIRST(&nq->nq_req);

	if (nreq->nreq_type != type)
		return FALSE;

	ndev_queue_advance(nq);

	return TRUE;
}

/*
 * Send an initialization request to a driver.  If this is a new driver, the
 * ethif module does not get to know about the driver until it answers to this
 * request, as the ethif module needs much of what the reply contains.  On the
 * other hand, if this is a restarted driver, it will stay disabled until the
 * init reply comes in.
 */
static void
ndev_send_init(struct ndev * ndev)
{
	message m;
	int r;

	memset(&m, 0, sizeof(m));
	m.m_type = NDEV_INIT;
	m.m_ndev_netdriver_init.id = ndev->ndev_sendq.nq_head;

	if ((r = asynsend3(ndev->ndev_endpt, &m, AMF_NOREPLY)) != OK)
		panic("asynsend to driver failed: %d", r);
}

/*
 * A network device driver has been started or restarted.
 */
static void
ndev_up(const char * label, endpoint_t endpt)
{
	static int reported = FALSE;
	struct ndev *ndev;
	ndev_id_t slot;

	/*
	 * First see if we already had an entry for this driver.  If so, it has
	 * been restarted, and we need to report it as not running to ethif.
	 */
	ndev = NULL;

	for (slot = 0; slot < ndev_max; slot++) {
		if (ndev_array[slot].ndev_endpt == NONE) {
			if (ndev == NULL)
				ndev = &ndev_array[slot];

			continue;
		}

		if (!strcmp(ndev_array[slot].ndev_label, label)) {
			/* Cancel any ongoing requests. */
			ndev_queue_reset(&ndev_array[slot].ndev_sendq);
			ndev_queue_reset(&ndev_array[slot].ndev_recvq);

			if (ndev_array[slot].ndev_ethif != NULL) {
				ethif_disable(ndev_array[slot].ndev_ethif);

				ndev_pending++;
			}

			ndev_array[slot].ndev_endpt = endpt;

			/* Attempt to resume communication. */
			ndev_send_init(&ndev_array[slot]);

			return;
		}
	}

	if (ndev == NULL) {
		/*
		 * If there is no free slot for this driver in our table, we
		 * necessarily have to ignore the driver altogether.  We report
		 * such cases once, so that the user can recompile if desired.
		 */
		if (ndev_max == __arraycount(ndev_array)) {
			if (!reported) {
				printf("LWIP: not enough ndev slots!\n");

				reported = TRUE;
			}
			return;
		}

		ndev = &ndev_array[ndev_max++];
	}

	/* Initialize the slot. */
	ndev->ndev_endpt = endpt;
	strlcpy(ndev->ndev_label, label, sizeof(ndev->ndev_label));
	ndev->ndev_ethif = NULL;
	ndev_queue_init(&ndev->ndev_sendq);
	ndev_queue_init(&ndev->ndev_recvq);

	ndev_send_init(ndev);

	ndev_pending++;
}

/*
 * A network device driver has been terminated.
 */
static void
ndev_down(struct ndev * ndev)
{

	/* Cancel any ongoing requests. */
	ndev_queue_reset(&ndev->ndev_sendq);
	ndev_queue_reset(&ndev->ndev_recvq);

	/*
	 * If this ndev object had a corresponding ethif object, tell the ethif
	 * layer that the device is really gone now.
	 */
	if (ndev->ndev_ethif != NULL)
		ethif_remove(ndev->ndev_ethif);
	else
		ndev_pending--;

	/* Remove the driver from our own administration. */
	ndev->ndev_endpt = NONE;

	while (ndev_max > 0 && ndev_array[ndev_max - 1].ndev_endpt == NONE)
		ndev_max--;
}

/*
 * The DS service has notified us of changes to our subscriptions.  That means
 * that network drivers may have been started, restarted, and/or shut down.
 * Find out what has changed, and act accordingly.
 */
void
ndev_check(void)
{
	static const char *prefix = "drv.net.";
	char key[DS_MAX_KEYLEN], *label;
	size_t prefixlen;
	endpoint_t endpt;
	uint32_t val;
	ndev_id_t slot;
	int r;

	prefixlen = strlen(prefix);

	/* Check whether any drivers have been (re)started. */
	while ((r = ds_check(key, NULL, &endpt)) == OK) {
		if (strncmp(key, prefix, prefixlen) != 0 || endpt == NONE)
			continue;

		if (ds_retrieve_u32(key, &val) != OK || val != DS_DRIVER_UP)
			continue;

		label = &key[prefixlen];
		if (label[0] == '\0' || memchr(label, '\0', LABEL_MAX) == NULL)
			continue;

		ndev_up(label, endpt);
	}

	if (r != ENOENT)
		printf("LWIP: DS check failed (%d)\n", r);

	/*
	 * Check whether the drivers we currently know about are still up.  The
	 * ones that are not are really gone.  It is no problem that we recheck
	 * any drivers that have just been reported by ds_check() above.
	 * However, we cannot check the same key: while the driver is being
	 * restarted, its driver status is already gone from DS. Instead, see
	 * if there is still an entry for its label, as that entry remains in
	 * existence during the restart.  The associated endpoint may still
	 * change however, so do not check that part: in such cases we will get
	 * a driver-up announcement later anyway.
	 */
	for (slot = 0; slot < ndev_max; slot++) {
		if (ndev_array[slot].ndev_endpt == NONE)
			continue;

		if (ds_retrieve_label_endpt(ndev_array[slot].ndev_label,
		    &endpt) != OK)
			ndev_down(&ndev_array[slot]);
	}
}

/*
 * A network device driver has sent a reply to our initialization request.
 */
static void
ndev_init_reply(struct ndev * ndev, const message * m_ptr)
{
	struct ndev_hwaddr hwaddr;
	uint8_t hwaddr_len, max_send, max_recv;
	const char *name;
	int enabled;

	/*
	 * Make sure that we were waiting for a reply to an initialization
	 * request, and that this is the reply to that request.
	 */
	if (NDEV_ACTIVE(ndev) ||
	    m_ptr->m_netdriver_ndev_init_reply.id != ndev->ndev_sendq.nq_head)
		return;

	/*
	 * Do just enough sanity checking on the data to pass it up to the
	 * ethif layer, which will check the rest (e.g., name duplicates).
	 */
	if (memchr(m_ptr->m_netdriver_ndev_init_reply.name, '\0',
	    sizeof(m_ptr->m_netdriver_ndev_init_reply.name)) == NULL ||
	    m_ptr->m_netdriver_ndev_init_reply.name[0] == '\0') {
		printf("LWIP: driver %d provided invalid name\n",
		    m_ptr->m_source);

		ndev_down(ndev);

		return;
	}

	hwaddr_len = m_ptr->m_netdriver_ndev_init_reply.hwaddr_len;
	if (hwaddr_len < 1 || hwaddr_len > __arraycount(hwaddr.nhwa_addr)) {
		printf("LWIP: driver %d provided invalid HW-addr length\n",
		    m_ptr->m_source);

		ndev_down(ndev);

		return;
	}

	if ((max_send = m_ptr->m_netdriver_ndev_init_reply.max_send) < 1 ||
	    (max_recv = m_ptr->m_netdriver_ndev_init_reply.max_recv) < 1) {
		printf("LWIP: driver %d provided invalid queue maximum\n",
		    m_ptr->m_source);

		ndev_down(ndev);

		return;
	}

	/*
	 * If the driver is new, allocate a new ethif object for it.  On
	 * success, or if the driver was restarted, (re)enable the interface.
	 * Both calls may fail, in which case we should forget about the
	 * driver.  It may continue to send us messages, which we should then
	 * discard.
	 */
	name = m_ptr->m_netdriver_ndev_init_reply.name;

	if (ndev->ndev_ethif == NULL) {
		ndev->ndev_ethif = ethif_add((ndev_id_t)(ndev - ndev_array),
		    name, m_ptr->m_netdriver_ndev_init_reply.caps);
		name = NULL;
	}

	if (ndev->ndev_ethif != NULL) {
		/*
		 * Set the maximum numbers of pending requests (for each
		 * direction) first, because enabling the interface may cause
		 * the ethif layer to start sending requests immediately.
		 */
		ndev->ndev_sendq.nq_max = max_send;
		ndev->ndev_sendq.nq_head++;

		/*
		 * Limit the maximum number of concurrently pending receive
		 * requests to our configured maximum.  For send requests, we
		 * use a more dynamic approach with spare request objects.
		 */
		if (max_recv > NDEV_RECVQ)
			max_recv = NDEV_RECVQ;
		ndev->ndev_recvq.nq_max = max_recv;
		ndev->ndev_recvq.nq_head++;

		memset(&hwaddr, 0, sizeof(hwaddr));
		memcpy(hwaddr.nhwa_addr,
		    m_ptr->m_netdriver_ndev_init_reply.hwaddr, hwaddr_len);

		/*
		 * Provide a NULL pointer for the name if we have only just
		 * added the interface at all.  The callee may use this to
		 * determine whether the driver is new or has been restarted.
		 */
		enabled = ethif_enable(ndev->ndev_ethif, name, &hwaddr,
		    m_ptr->m_netdriver_ndev_init_reply.hwaddr_len,
		    m_ptr->m_netdriver_ndev_init_reply.caps,
		    m_ptr->m_netdriver_ndev_init_reply.link,
		    m_ptr->m_netdriver_ndev_init_reply.media);
	} else
		enabled = FALSE;

	/*
	 * If we did not manage to enable the interface, remove it again,
	 * possibly also from the ethif layer.
	 */
	if (!enabled)
		ndev_down(ndev);
	else
		ndev_pending--;
}

/*
 * Request that a network device driver change its configuration.  This
 * function allows for configuration of various different driver and device
 * aspects: the I/O mode (and multicast receipt list), the enabled (sub)set of
 * capabilities, the driver-specific flags, and the hardware address.  Each of
 * these settings may be changed by setting the corresponding NDEV_SET_ flag in
 * the 'set' field of the given configuration structure.  It is explicitly
 * allowed to generate a request with no NDEV_SET_ flags; such a request will
 * be sent to the driver and ultimately generate a response.  Return OK if the
 * configuration request was sent to the driver, EBUSY if no (more) requests
 * can be sent to the driver right now, or ENOMEM on grant allocation failure.
 */
int
ndev_conf(ndev_id_t id, const struct ndev_conf * nconf)
{
	struct ndev *ndev;
	struct ndev_req *nreq;
	uint32_t seq;
	message m;
	cp_grant_id_t grant;
	int r;

	assert(id < __arraycount(ndev_array));
	ndev = &ndev_array[id];

	assert(ndev->ndev_endpt != NONE);
	assert(NDEV_ACTIVE(ndev));

	if ((nreq = ndev_queue_get(&ndev->ndev_sendq, NDEV_CONF,
	    &seq)) == NULL)
		return EBUSY;

	memset(&m, 0, sizeof(m));
	m.m_type = NDEV_CONF;
	m.m_ndev_netdriver_conf.id = seq;
	m.m_ndev_netdriver_conf.set = nconf->nconf_set;

	grant = GRANT_INVALID;

	if (nconf->nconf_set & NDEV_SET_MODE) {
		m.m_ndev_netdriver_conf.mode = nconf->nconf_mode;

		if (nconf->nconf_mode & NDEV_MODE_MCAST_LIST) {
			assert(nconf->nconf_mclist != NULL);
			assert(nconf->nconf_mccount != 0);

			grant = cpf_grant_direct(ndev->ndev_endpt,
			    (vir_bytes)nconf->nconf_mclist,
			    sizeof(nconf->nconf_mclist[0]) *
			    nconf->nconf_mccount, CPF_READ);

			if (!GRANT_VALID(grant))
				return ENOMEM;

			m.m_ndev_netdriver_conf.mcast_count =
			    nconf->nconf_mccount;
		}
	}

	m.m_ndev_netdriver_conf.mcast_grant = grant;

	if (nconf->nconf_set & NDEV_SET_CAPS)
		m.m_ndev_netdriver_conf.caps = nconf->nconf_caps;

	if (nconf->nconf_set & NDEV_SET_FLAGS)
		m.m_ndev_netdriver_conf.flags = nconf->nconf_flags;

	if (nconf->nconf_set & NDEV_SET_MEDIA)
		m.m_ndev_netdriver_conf.media = nconf->nconf_media;

	if (nconf->nconf_set & NDEV_SET_HWADDR)
		memcpy(m.m_ndev_netdriver_conf.hwaddr,
		    nconf->nconf_hwaddr.nhwa_addr,
		    __arraycount(m.m_ndev_netdriver_conf.hwaddr));

	if ((r = asynsend3(ndev->ndev_endpt, &m, AMF_NOREPLY)) != OK)
		panic("asynsend to driver failed: %d", r);

	nreq->nreq_grant[0] = grant; /* may also be invalid */
	nreq->nreq_grant[1] = GRANT_INVALID;

	ndev_queue_add(&ndev->ndev_sendq, nreq);

	return OK;
}

/*
 * The network device driver has sent a reply to a configuration request.
 */
static void
ndev_conf_reply(struct ndev * ndev, const message * m_ptr)
{

	/*
	 * Was this the request we were waiting for?  If so, remove it from the
	 * send queue.  Otherwise, ignore this reply message.
	 */
	if (!NDEV_ACTIVE(ndev) || !ndev_queue_remove(&ndev->ndev_sendq,
	    NDEV_CONF, m_ptr->m_netdriver_ndev_reply.id))
		return;

	/* Tell the ethif layer about the updated configuration. */
	assert(ndev->ndev_ethif != NULL);

	ethif_configured(ndev->ndev_ethif,
	    m_ptr->m_netdriver_ndev_reply.result);
}

/*
 * Construct a packet send or receive request and send it off to a network
 * driver.  The given pbuf chain may be part of a queue.  Return OK if the
 * request was successfully sent, or ENOMEM on grant allocation failure.
 */
static int
ndev_transfer(struct ndev * ndev, const struct pbuf * pbuf, int do_send,
	uint32_t seq, struct ndev_req * nreq)
{
	cp_grant_id_t grant;
	message m;
	unsigned int i;
	size_t left;
	int r;

	memset(&m, 0, sizeof(m));
	m.m_type = (do_send) ? NDEV_SEND : NDEV_RECV;
	m.m_ndev_netdriver_transfer.id = seq;

	left = pbuf->tot_len;

	for (i = 0; left > 0; i++) {
		assert(i < NDEV_IOV_MAX);

		grant = cpf_grant_direct(ndev->ndev_endpt,
		    (vir_bytes)pbuf->payload, pbuf->len,
		    (do_send) ? CPF_READ : CPF_WRITE);

		if (!GRANT_VALID(grant)) {
			while (i-- > 0)
				(void)cpf_revoke(nreq->nreq_grant[i]);

			return ENOMEM;
		}

		m.m_ndev_netdriver_transfer.grant[i] = grant;
		m.m_ndev_netdriver_transfer.len[i] = pbuf->len;

		nreq->nreq_grant[i] = grant;

		assert(left >= pbuf->len);
		left -= pbuf->len;
		pbuf = pbuf->next;
	}

	m.m_ndev_netdriver_transfer.count = i;

	/*
	 * Unless the array is full, an invalid grant marks the end of the list
	 * of invalid grants.
	 */
	if (i < __arraycount(nreq->nreq_grant))
		nreq->nreq_grant[i] = GRANT_INVALID;

	if ((r = asynsend3(ndev->ndev_endpt, &m, AMF_NOREPLY)) != OK)
		panic("asynsend to driver failed: %d", r);

	return OK;
}

/*
 * Send a packet to the given network driver.  Return OK if the packet is sent
 * off to the driver, EBUSY if no (more) packets can be sent to the driver at
 * this time, or ENOMEM on grant allocation failure.
 *
 * The use of 'pbuf' in this interface is a bit ugly, but it saves us from
 * having to go through an intermediate representation (e.g. an iovec array)
 * for the data being sent.  The same applies to ndev_receive().
 */
int
ndev_send(ndev_id_t id, const struct pbuf * pbuf)
{
	struct ndev *ndev;
	struct ndev_req *nreq;
	uint32_t seq;
	int r;

	assert(id < __arraycount(ndev_array));
	ndev = &ndev_array[id];

	assert(ndev->ndev_endpt != NONE);
	assert(NDEV_ACTIVE(ndev));

	if ((nreq = ndev_queue_get(&ndev->ndev_sendq, NDEV_SEND,
	    &seq)) == NULL)
		return EBUSY;

	if ((r = ndev_transfer(ndev, pbuf, TRUE /*do_send*/, seq, nreq)) != OK)
		return r;

	ndev_queue_add(&ndev->ndev_sendq, nreq);

	return OK;
}

/*
 * The network device driver has sent a reply to a send request.
 */
static void
ndev_send_reply(struct ndev * ndev, const message * m_ptr)
{

	/*
	 * Was this the request we were waiting for?  If so, remove it from the
	 * send queue.  Otherwise, ignore this reply message.
	 */
	if (!NDEV_ACTIVE(ndev) || !ndev_queue_remove(&ndev->ndev_sendq,
	    NDEV_SEND, m_ptr->m_netdriver_ndev_reply.id))
		return;

	/* Tell the ethif layer about the result of the transmission. */
	assert(ndev->ndev_ethif != NULL);

	ethif_sent(ndev->ndev_ethif,
	    m_ptr->m_netdriver_ndev_reply.result);
}

/*
 * Return TRUE if a new receive request can be spawned for a particular network
 * driver, or FALSE if its queue of receive requests is full.  This call exists
 * merely to avoid needless buffer allocatin in the case that ndev_recv() is
 * going to return EBUSY anyway.
 */
int
ndev_can_recv(ndev_id_t id)
{
	struct ndev *ndev;

	assert(id < __arraycount(ndev_array));
	ndev = &ndev_array[id];

	assert(ndev->ndev_endpt != NONE);
	assert(NDEV_ACTIVE(ndev));

	return (ndev->ndev_recvq.nq_count < ndev->ndev_recvq.nq_max);
}

/*
 * Start the process of receiving a packet from a network driver.  The packet
 * will be stored in the given pbuf chain upon completion.  Return OK if the
 * receive request is sent to the driver, EBUSY if the maximum number of
 * concurrent receive requests has been reached for this driver, or ENOMEM on
 * grant allocation failure.
 */
int
ndev_recv(ndev_id_t id, struct pbuf * pbuf)
{
	struct ndev *ndev;
	struct ndev_req *nreq;
	uint32_t seq;
	int r;

	assert(id < __arraycount(ndev_array));
	ndev = &ndev_array[id];

	assert(ndev->ndev_endpt != NONE);
	assert(NDEV_ACTIVE(ndev));

	if ((nreq = ndev_queue_get(&ndev->ndev_recvq, NDEV_RECV,
	    &seq)) == NULL)
		return EBUSY;

	if ((r = ndev_transfer(ndev, pbuf, FALSE /*do_send*/, seq,
	    nreq)) != OK)
		return r;

	ndev_queue_add(&ndev->ndev_recvq, nreq);

	return OK;
}

/*
 * The network device driver has sent a reply to a receive request.
 */
static void
ndev_recv_reply(struct ndev * ndev, const message * m_ptr)
{

	/*
	 * Was this the request we were waiting for?  If so, remove it from the
	 * receive queue.  Otherwise, ignore this reply message.
	 */
	if (!NDEV_ACTIVE(ndev) || !ndev_queue_remove(&ndev->ndev_recvq,
	    NDEV_RECV, m_ptr->m_netdriver_ndev_reply.id))
		return;

	/* Tell the ethif layer about the result of the receipt. */
	assert(ndev->ndev_ethif != NULL);

	ethif_received(ndev->ndev_ethif,
	    m_ptr->m_netdriver_ndev_reply.result);
}

/*
 * A network device driver sent a status report to us.  Process it and send a
 * reply.
 */
static void
ndev_status(struct ndev * ndev, const message * m_ptr)
{
	message m;
	int r;

	if (!NDEV_ACTIVE(ndev))
		return;

	/* Tell the ethif layer about the status update. */
	assert(ndev->ndev_ethif != NULL);

	ethif_status(ndev->ndev_ethif, m_ptr->m_netdriver_ndev_status.link,
	    m_ptr->m_netdriver_ndev_status.media,
	    m_ptr->m_netdriver_ndev_status.oerror,
	    m_ptr->m_netdriver_ndev_status.coll,
	    m_ptr->m_netdriver_ndev_status.ierror,
	    m_ptr->m_netdriver_ndev_status.iqdrop);

	/*
	 * Send a reply, so that the driver knows it can send a new status
	 * update without risking asynsend queue overflows.  The ID of these
	 * messages is chosen by the driver and and we simply echo it.
	 */
	memset(&m, 0, sizeof(m));
	m.m_type = NDEV_STATUS_REPLY;
	m.m_ndev_netdriver_status_reply.id = m_ptr->m_netdriver_ndev_status.id;

	if ((r = asynsend(m_ptr->m_source, &m)) != OK)
		panic("asynsend to driver failed: %d", r);
}

/*
 * Process a network driver reply message.
 */
void
ndev_process(const message * m_ptr, int ipc_status)
{
	struct ndev *ndev;
	endpoint_t endpt;
	ndev_id_t slot;

	/* Find the slot of the driver that sent the message, if any. */
	endpt = m_ptr->m_source;

	for (slot = 0, ndev = ndev_array; slot < ndev_max; slot++, ndev++)
		if (ndev->ndev_endpt == endpt)
			break;

	/*
	 * If we cannot find a slot for the driver, drop the message.  We may
	 * be ignoring the driver because it misbehaved or we are out of slots.
	 */
	if (slot == ndev_max)
		return;

	/*
	 * Process the reply message.  For future compatibility, ignore any
	 * unrecognized message types.
	 */
	switch (m_ptr->m_type) {
	case NDEV_INIT_REPLY:
		ndev_init_reply(ndev, m_ptr);

		break;

	case NDEV_CONF_REPLY:
		ndev_conf_reply(ndev, m_ptr);

		break;

	case NDEV_SEND_REPLY:
		ndev_send_reply(ndev, m_ptr);

		break;

	case NDEV_RECV_REPLY:
		ndev_recv_reply(ndev, m_ptr);

		break;

	case NDEV_STATUS:
		ndev_status(ndev, m_ptr);

		break;
	}
}