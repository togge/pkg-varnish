/*
 * $Id$
 */

#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <limits.h>

#include "shmlog.h"
#include "cache.h"
#include "libvarnish.h"
#include "heritage.h"


/*--------------------------------------------------------------------
 * TTL and Age calculation in Varnish
 *
 * RFC2616 has a lot to say about how caches should calculate the TTL
 * and expiry times of objects, but it sort of misses the case that
 * applies to Varnish:  the server-side cache.
 *
 * A normal cache, shared or single-client, has no symbiotic relationship
 * with the server, and therefore must take a very defensive attitude
 * if the Data/Expiry/Age/max-age data does not make sense.  Overall
 * the policy described in section 13 of RFC 2616 results in no caching
 * happening on the first little sign of trouble.
 *
 * Varnish on the other hand tries to offload as many transactions from
 * the backend as possible, and therefore just passing through everything
 * if there is a clock-skew between backend and Varnish is not a workable
 * choice.
 *
 * Varnish implements a policy which is RFC2616 compliant when there
 * is no clockskew, and falls back to a new "clockless cache" mode otherwise.
 * Our "clockless cache" model is syntehsized from the bits of RFC2616
 * that talks about how a cache should react to a clockless origin server,
 * and more or uses the inverse logic for the opposite relationship.
 *
 */

#if PSEUDO_CODE
 	/* Marker for no retirement age determined */
 	retirement_age = INT_MAX

 	/* If we have a max-age directive, respect it */
 	if (max-age)
 		retirement_age = max(0,min(retirement_age, max-age - Age:))
 
 	/* If Date: is not in future and Expires: looks sensible, use it */
 	if ((!date || date < our_clock) && expires > our_clock) {
 		ttd = min(our_clock + retirement_age, Expires:)
  
 	/* Otherwise we have clock-skew */
 	} else {
 		/* If we have both date and expires, infer max-age */
 		if (date && expires)
 			retirement_age =
 			    max(0, min(retirement_age, Expires: - Date:)
  
 		/* Apply default_ttl if nothing better found */
 		if (retirement_age == INT_MAX)
 			retirement_age = default_ttl
  
 		/* Apply the max-age we can up with */
 		ttd = our_clock + retirement_age
 	}
  
 	/* Apply hard limits */
 	ttd = max(ttd, our_clock + hard_lower_ttl)
 	ttd = min(ttd, our_clock + hard_upper_ttl)
#endif

static time_t
RFC2616_Ttl(struct http *hp, time_t t_req, time_t t_resp, struct object *obj)
{
	int retirement_age;
	unsigned u1, u2;
	time_t h_date, h_expires, ttd;
	char *p;
	
	retirement_age = INT_MAX;

	u1 = u2 = 0;
	if (http_GetHdrField(hp, "Cache-Control", "max-age", &p)) {
		u1 = strtoul(p, NULL, 0);
		u2 = 0;
		if (http_GetHdr(hp, "Age", &p)) {
			u2 = strtoul(p, NULL, 0);
			obj->age = u2;
		}
		if (u2 <= u1)
			retirement_age = u1 - u2;
	}

	h_date = 0;
	if (http_GetHdr(hp, "Date", &p))
		h_date = TIM_parse(p);

	h_expires = 0;
	if (http_GetHdr(hp, "Expires", &p))
		h_expires = TIM_parse(p);

	if (h_date < t_req && h_expires > t_req) {
		ttd = h_expires;
		if (retirement_age != INT_MAX && t_req + retirement_age < ttd)
			ttd = t_req + retirement_age;
	} else {
		if (h_date != 0 && h_expires != 0) {
			if (h_date < h_expires &&
			    h_expires - h_date < retirement_age)
				retirement_age = h_expires - h_date;
		}
		if (retirement_age == INT_MAX)
			retirement_age = heritage.default_ttl;

		ttd = t_req + retirement_age;
	}
	VSL(SLT_Debug, 0,
	    "TTD: max-age %u Age: %u Date: %d (%d) Expires %d (%d) our_clock %d"
	    " -> ttd %d (%d)",
	    u1, u2, h_date, h_date - t_req, h_expires, h_expires - t_req, t_req, ttd, ttd - t_req);
	return (ttd);
}

int
RFC2616_cache_policy(struct sess *sp, struct http *hp)
{
	int body = 0;

	/*
	 * Initial cacheability determination per [RFC2616, 13.4]
	 * We do not support ranges yet, so 206 is out.
	 */
	sp->obj->response = http_GetStatus(hp);
	switch (sp->obj->response) {
	case 200: /* OK */
	case 203: /* Non-Authoritative Information */
	case 300: /* Multiple Choices */
	case 301: /* Moved Permanently */
	case 410: /* Gone */
	case 404: /* Not Found */
		sp->obj->cacheable = 1;
		sp->obj->valid = 1;
		body = 1;
		break;
	default:
		sp->obj->cacheable = 0;
		body = 0;
		break;
	}

	sp->obj->ttl = RFC2616_Ttl(hp, sp->t_req, sp->t_resp, sp->obj);
	sp->obj->entered = sp->t_req;
	if (sp->obj->ttl == 0) {
		sp->obj->cacheable = 0;
	}

	return (body);
}

