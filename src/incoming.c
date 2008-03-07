/*
 *	aprsc
 *
 *	(c) Heikki Hannikainen, OH7LZB <hessu@hes.iki.fi>
 *
 *    This program is free software; you can redistribute it and/or modify
 *    it under the terms of the GNU General Public License as published by
 *    the Free Software Foundation; either version 2 of the License, or
 *    (at your option) any later version.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU General Public License for more details.
 *
 *    You should have received a copy of the GNU General Public License
 *    along with this program; if not, write to the Free Software
 *    Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *	
 */

/*
 *	incoming.c: processes incoming data within the worker thread
 */

#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <alloca.h>

#include "incoming.h"
#include "hlog.h"
#include "parse_aprs.h"

#include "cellmalloc.h"

/* global packet buffer freelists */

cellarena_t *pbuf_cells_small;
cellarena_t *pbuf_cells_large;
cellarena_t *pbuf_cells_huge;


/*
 *	Get a buffer for a packet
 *
 *	pbuf_t buffers are accumulated into each worker local buffer in small sets,
 *	and then used from there.  The buffers are returned into global pools.
 */

void pbuf_init(void)
{
	pbuf_cells_small = cellinit(sizeof(struct pbuf_t) + PACKETLEN_MAX_SMALL,
				    __alignof__(struct pbuf_t), CELLMALLOC_POLICY_FIFO,
				    1024 /* 1 MB at the time */, 0 /* minfree */);
	pbuf_cells_large = cellinit(sizeof(struct pbuf_t) + PACKETLEN_MAX_LARGE,
				    __alignof__(struct pbuf_t), CELLMALLOC_POLICY_FIFO,
				    1024 /* 1 MB at the time */, 0 /* minfree */);
	pbuf_cells_huge  = cellinit(sizeof(struct pbuf_t) + PACKETLEN_MAX_HUGE,
				    __alignof__(struct pbuf_t), CELLMALLOC_POLICY_FIFO,
				    1024 /* 1 MB at the time */, 0 /* minfree */);
}

/*
 *	pbuf_free  sends buffer back to worker local pool, or when invoked
 *	without 'self' pointer, like in final history buffer cleanup,
 *	to the global pool.
 */

void pbuf_free(struct worker_t *self, struct pbuf_t *p)
{
	if (self) { /* Return to worker local pool */
		switch (p->buf_len) {
		case PACKETLEN_MAX_SMALL:
			p->next = self->pbuf_free_small;
			self->pbuf_free_small = p;
			return;
		case PACKETLEN_MAX_LARGE:
			p->next = self->pbuf_free_large;
			self->pbuf_free_large = p;
			return;
		case PACKETLEN_MAX_HUGE:
			p->next = self->pbuf_free_huge;
			self->pbuf_free_huge = p;
			return;
		default:
			break;
		}
	}

	/* Not worker local processing then, return to global pools. */

	switch (p->buf_len) {
	case PACKETLEN_MAX_SMALL:
		cellfree(pbuf_cells_small, p);
		break;
	case PACKETLEN_MAX_LARGE:
		cellfree(pbuf_cells_large, p);
		break;
	case PACKETLEN_MAX_HUGE:
		cellfree(pbuf_cells_huge, p);
		break;
	default:
		hlog(LOG_ERR, "pbuf_free(%p) - packet length not known: %d", p, p->buf_len);
		break;
	}
}

/*
 *	pbuf_free_many  sends buffers back to the global pool in groups
 *                      after size sorting them...  
 *			Multiple cells are returned with single mutex.
 */

void pbuf_free_many(struct pbuf_t **array, int numbufs)
{
	void **arraysmall  = alloca(sizeof(void*)*numbufs);
	void **arraylarge  = alloca(sizeof(void*)*numbufs);
	void **arrayhuge   = alloca(sizeof(void*)*numbufs);
	int i;
	int smallcnt = 0, largecnt = 0, hugecnt = 0;

	for (i = 0; i < numbufs; ++i) {
		switch (array[i]->buf_len) {
		case PACKETLEN_MAX_SMALL:
			arraysmall[smallcnt++] = array[i];
			break;
		case PACKETLEN_MAX_LARGE:
			arraylarge[largecnt++] = array[i];
			break;
		case PACKETLEN_MAX_HUGE:
			arrayhuge[hugecnt++]   = array[i];
			break;
		default:
			hlog(LOG_ERR, "pbuf_free(%p) - packet length not known: %d", array[i], array[i]->buf_len);
			break;
		}
	}
	if (smallcnt > 0)
		cellfreemany(pbuf_cells_small, arraysmall, smallcnt);
	if (largecnt > 0)
		cellfreemany(pbuf_cells_large, arraylarge, largecnt);
	if (hugecnt > 0)
		cellfreemany(pbuf_cells_huge,  arrayhuge,   hugecnt);
}

struct pbuf_t *pbuf_get_real(struct pbuf_t **pool, cellarena_t *global_pool,
			     int len, int bunchlen)
{
	struct pbuf_t *pb;
	int i;
	struct pbuf_t **allocarray = alloca(bunchlen * sizeof(void*));
	
	if (*pool) {
		/* fine, just get the first buffer from the pool...
		 * the pool is not doubly linked (not necessary)
		 */
		pb = *pool;
		*pool = pb->next;
		return pb;
	}
	
	/* The local list is empty... get buffers from the global list. */

	bunchlen = cellmallocmany( global_pool, (void**)allocarray, bunchlen );

	for ( i = 0;  i < bunchlen; ++i ) {
		if (i > 0)
		  (*pool)->next = allocarray[i];
		*pool = allocarray[i];
	}
	if (*pool)
		(*pool)->next = NULL;

	hlog(LOG_DEBUG, "pbuf_get_real(%d): got %d bufs from global pool",
	     len, bunchlen);
	
	
	/* ok, return the first buffer from the pool */
	pb = *pool;
	if (!pb) return NULL;
	*pool = pb->next;

	/* zero all header fields */
	memset(pb, 0, sizeof(*pb));

	/* we know the length in this sub-pool, set it */
	pb->buf_len = len;
	
	return pb;
}

struct pbuf_t *pbuf_get(struct worker_t *self, int len)
{
	/* select which thread-local freelist to use */
	if (len <= PACKETLEN_MAX_SMALL) {
		//hlog(LOG_DEBUG, "pbuf_get: Allocating small buffer for a packet of %d bytes", len);
		return pbuf_get_real(&self->pbuf_free_small, pbuf_cells_small,
				     PACKETLEN_MAX_SMALL, PBUF_ALLOCATE_BUNCH_SMALL);
	} else if (len <= PACKETLEN_MAX_LARGE) {
		//hlog(LOG_DEBUG, "pbuf_get: Allocating large buffer for a packet of %d bytes", len);
		return pbuf_get_real(&self->pbuf_free_large, pbuf_cells_large,
				     PACKETLEN_MAX_LARGE, PBUF_ALLOCATE_BUNCH_LARGE);
	} else if (len <= PACKETLEN_MAX_HUGE) {
		//hlog(LOG_DEBUG, "pbuf_get: Allocating huge buffer for a packet of %d bytes", len);
		return pbuf_get_real(&self->pbuf_free_huge, pbuf_cells_huge,
				     PACKETLEN_MAX_HUGE, PBUF_ALLOCATE_BUNCH_HUGE);
	} else { /* too large! */
		hlog(LOG_ERR, "pbuf_get: Not allocating a buffer for a packet of %d bytes!", len);
		return NULL;
	}
}

/*
 *	Move incoming packets from the thread-local incoming buffer
 *	(self->pbuf_incoming_local) to self->incoming local for the
 *	dupecheck thread to catch them
 */

void incoming_flush(struct worker_t *self)
{
	/* try grab the lock.. if it fails, we'll try again, either
	 * in 200 milliseconds or after next input
	 */
	if (pthread_mutex_trylock(&self->pbuf_incoming_mutex) != 0)
		return;
		
	*self->pbuf_incoming_last = self->pbuf_incoming_local;
	pthread_mutex_unlock(&self->pbuf_incoming_mutex);
	
	/* clean the local lockfree queue */
	self->pbuf_incoming_local = NULL;
	self->pbuf_incoming_local_last = &self->pbuf_incoming_local;
}

/*
 *	Parse an incoming packet
 */

int incoming_parse(struct worker_t *self, struct client_t *c, char *s, int len)
{
	struct pbuf_t *pb;
	const char *src_end; /* pointer to the > after srccall */
	const char *path_start; /* pointer to the start of the path */
	const char *path_end; /* pointer to the : after the path */
	const char *packet_end; /* pointer to the end of the packet */
	const char *info_start; /* pointer to the beginning of the info */
	const char *info_end; /* end of the info */
	const char *dstcall_end; /* end of dstcall ([:,]) */
	const char *via_start;
	const char *qcons_start;
	const char *data;	  /* points to original incoming path/payload separating ':' character */
	int datalen;		  /* length of the data block excluding tail \r\n */
	int pathlen;		  /* length of the path  ==  data-s  */
	int qcons_len = 0;
	int rc;
	char qcons[20];
	
	/* a packet looks like:
	 * SRCCALL>DSTCALL,PATH,PATH:INFO\r\n
	 * (we have normalized the \r\n by now)
	 */


	path_end = memchr(s, ':', len);
	if (!path_end)
		return -1; // No ":" in the packet
	pathlen = path_end - s;

	data = path_end;            // Begins with ":"
	datalen = len - pathlen;    // Not including line end \r\n

	packet_end = s + len;	    // Just to compare against far end..

	/* look for the '>' */
	src_end = memchr(s, '>', pathlen < CALLSIGNLEN_MAX+1 ? pathlen : CALLSIGNLEN_MAX+1);
	if (!src_end)
		return -1;	// No ">" in packet start..
	
	path_start = src_end+1;
	if (path_start >= packet_end)
		return -1;
	
	if (src_end - s > CALLSIGNLEN_MAX)
		return -1; /* too long source callsign */
	
	info_start = path_end+1;	// @":"+1 - first char of the payload
	if (info_start >= packet_end)
		return -1;
	
	/* see that there is at least some data in the packet */
	info_end = packet_end;
	if (info_end <= info_start)
		return -1;
	
	/* look up end of dstcall (excluding SSID - this is the way dupecheck and
	 * mic-e parser wants it)
	 */

	dstcall_end = path_start;
	while (dstcall_end < path_end && *dstcall_end != '-' && *dstcall_end != ',' && *dstcall_end != ':')
		dstcall_end++;
	
	if (dstcall_end - path_start > CALLSIGNLEN_MAX)
		return -1; /* too long for destination callsign */
	

	via_start = dstcall_end;
	while (via_start < path_end && (*via_start != ',' && *via_start != ':'))
		++via_start;
	if (*via_start == ',')
		++via_start;

	// FIXME: add qConstruct production (if needed)!
	// FIXME: this may alter packet length, and address header!

	if (pathlen > 2 && path_end[-1] == 'I' && path_end[-2] == ',') {
	  // Possibly  ... ",call,I:" type of injection
		const char *p = path_end-3;
		while (s < p && *p != ',')
			--p;
		if ((path_end - p) > (CALLSIGNLEN_MAX + 3))
			return -1; // Bad form..
		if (*p == ',') ++p; // should always happen
		pathlen    = p - s; // Keep this much off the start

		memcpy(qcons,"qA#,",4);        // FIXME
		memcpy(qcons+4,p,path_end-p-2);
		qcons_len   = path_end-p+2;
		qcons_start = qcons;

	} else {
	  qcons_start = via_start;
	  while (qcons_start < path_end) {
	    if (qcons_start[0] == 'q' &&
		qcons_start[1] == 'A') {
	      break;
	    }
	    // Scan for next comma+1..
	    while (qcons_start < path_end) {
	      if (*qcons_start != ':' &&
		  *qcons_start != ',')
		++qcons_start;
	    }
	    if (*qcons_start == ',')
	      ++qcons_start;
	  }
	  if (*qcons_start == ':' ||
	      qcons_start >= path_end)
	    ;  // No  ,qA#,callsign:  ??
	  // qcons_start = NULL;

	  // FIXME: FIXME!
	}



	/* get a packet buffer */
	pb = pbuf_get(self, len+14); // reserve room for adding ",qA#,callsign"
	if (!pb)
		return -1; // No room :-(
	
	/* store the source reference */
	pb->origin = c;
	
	/* when it was received ? */
	pb->t = now;

	/* Actual data - start with path prefix */
	memcpy(pb->data, s, pathlen);
	pb->data[pathlen] = 0;

	/* Actual data */
	memcpy(pb->data+pathlen, qcons, qcons_len);
	memcpy(pb->data+pathlen+qcons_len, s, datalen);
	memcpy(pb->data+pathlen+qcons_len+datalen, "\r\n", 2); /* append missing CRLF */

	/* How much there really is data ? */
	pb->packet_len = pathlen+qcons_len+datalen+2;

	packet_end = pb->data + pb->packet_len; /* for easier overflow checking expressions */
	/* fill necessary info for parsing and dupe checking in the packet buffer */
	pb->srccall_end = pb->data + (src_end - s);
	pb->dstcall_end = pb->data + (dstcall_end - s);
	pb->info_start  = pb->data + (info_start - s);

	/* just try APRS parsing */
	rc = parse_aprs(self, pb);

	/* put the buffer in the thread's incoming queue */
	pb->next = NULL;
	*self->pbuf_incoming_local_last = pb;
	self->pbuf_incoming_local_last = &pb->next;

	return rc;
}

/*
 *	Handler called by the socket reading function for normal APRS-IS traffic
 */

int incoming_handler(struct worker_t *self, struct client_t *c, char *s, int len)
{
	int e;
	
	/* note: len does not include CRLF, it's reconstructed here... we accept
	 * CR, LF or CRLF on input but make sure to use CRLF on output.
	 */
	
	/* make sure it looks somewhat like an APRS-IS packet */
	if (len < PACKETLEN_MIN || len+2 > PACKETLEN_MAX) {
		hlog(LOG_WARNING, "Packet size out of bounds (%d): %s", len, s);
		return 0;
	}
	
	 /* starts with # => a comment packet, timestamp or something */
	if (*s == '#')
		return 0;

	/* do some parsing */
	e = incoming_parse(self, c, s, len);
	if (e < 0) {
		/* failed parsing */
		fprintf(stderr, "Failed parsing (%d):\n", e);
		fwrite(s, len, 1, stderr);
		fprintf(stderr, "\n");
		
		// So it failed, do send it out anyway....
		// pbuf_free(self, pb);
		// return 0;
		// FIXME: if it's COMPLETELY garbled, ie. not SRC>DST:DATA, do not send it out
		// - successful APRS parsing is not required.
	}
	
	return 0;
}

