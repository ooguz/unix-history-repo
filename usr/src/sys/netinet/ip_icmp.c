/*	ip_icmp.c	4.14	82/04/24	*/

#include "../h/param.h"
#include "../h/systm.h"
#include "../h/mbuf.h"
#include "../h/protosw.h"
#include "../h/socket.h"
#include "../h/clock.h"
#include "../net/in.h"
#include "../net/in_systm.h"
#include "../net/ip.h"
#include "../net/ip_icmp.h"

/*
 * ICMP routines: error generation, receive packet processing, and
 * routines to turnaround packets back to the originator, and
 * host table maintenance routines.
 */

/*
 * Generate an error packet of type error
 * in response to bad packet ip.
 */
icmp_error(oip, type, code)
	struct ip *oip;
	int type, code;
{
	register unsigned oiplen = oip->ip_hl << 2;
	register struct icmp *icp;
	struct mbuf *m;
	struct ip *nip;
COUNT(ICMP_ERROR);

	/*
	 * Make sure that the old IP packet had 8 bytes of data to return;
	 * if not, don't bother.  Also don't EVER error if the old
	 * packet protocol was ICMP.
	 */
	if (oip->ip_len - oiplen < 8 || oip->ip_p == IPPROTO_ICMP)
		goto free;

	/*
	 * First, formulate icmp message
	 */
	m = m_get(M_DONTWAIT);
	if (m == 0)
		goto free;
	m->m_len = oiplen + 8 + ICMP_MINLEN;
	m->m_off = MMAXOFF - m->m_len;
	icp = mtod(m, struct icmp *);
	icp->icmp_type = type;
	icp->icmp_void = 0;
	if (type == ICMP_PARAMPROB) {
		icp->icmp_pptr = code;
		code = 0;
	}
	icp->icmp_code = code;
	bcopy((caddr_t)oip, (caddr_t)&icp->icmp_ip, oiplen + 8);

	/*
	 * Now, copy old ip header in front of icmp
	 * message.  This allows us to reuse any source
	 * routing info present.
	 */
	m->m_off -= oiplen;
	nip = mtod(m, struct ip *);
	bcopy((caddr_t)oip, (caddr_t)nip, oiplen);
	nip->ip_len = m->m_len + oiplen;
	nip->ip_p = IPPROTO_ICMP;
	/* icmp_send adds ip header to m_off and m_len, so we deduct here */
	m->m_off += oiplen;
	icmp_reflect(nip);

free:
	m_freem(dtom(oip));
}

static char icmpmap[] = {
	-1,		 -1,		-1,
	PRC_UNREACH_NET, PRC_QUENCH, 	PRC_REDIRECT_NET,
	-1,		 -1,		-1,
	-1,		 -1,		PRC_TIMXCEED_INTRANS,
	PRC_PARAMPROB,	 -1,		-1,
	-1,		 -1
};

static struct sockproto icmproto = { AF_INET, IPPROTO_ICMP };
static struct sockaddr_in icmpsrc = { AF_INET };
static struct sockaddr_in icmpdst = { AF_INET };

/*
 * Process a received ICMP message.
 */
icmp_input(m)
	struct mbuf *m;
{
	register struct icmp *icp;
	register struct ip *ip = mtod(m, struct ip *);
	int icmplen = ip->ip_len, hlen = ip->ip_hl << 2, i;
	extern u_char ip_protox[];
COUNT(ICMP_INPUT);

	/*
	 * Locate icmp structure in mbuf, and check
	 * that not corrupted and of at least minimum length.
	 */
	if (icmplen < ICMP_MINLEN)
		goto free;
	m->m_len -= hlen;
	m->m_off += hlen;
	/* need routine to make sure header is in this mbuf here */
	icp = mtod(m, struct icmp *);
	i = icp->icmp_cksum;
	icp->icmp_cksum = 0;
	if (i != in_cksum(m, icmplen)) {
		printf("icmp: cksum %x\n", i);
		goto free;
	}

	/*
	 * Message type specific processing.
	 */
	switch (i = icp->icmp_type) {

	case ICMP_UNREACH:
	case ICMP_TIMXCEED:
	case ICMP_PARAMPROB:
	case ICMP_REDIRECT:
	case ICMP_SOURCEQUENCH:
		/*
		 * Problem with previous datagram; advise
		 * higher level routines.
		 */
		if (icmplen < ICMP_ADVLENMIN || icmplen < ICMP_ADVLEN(icp))
			goto free;
		(*protosw[ip_protox[ip->ip_p]].pr_ctlinput)
			(icmpmap[i] + icp->icmp_code, (caddr_t)icp);
		goto free;

	case ICMP_ECHO:
		icp->icmp_type = ICMP_ECHOREPLY;
		goto reflect;

	case ICMP_TSTAMP:
		if (icmplen < ICMP_TSLEN)
			goto free;
		icp->icmp_type = ICMP_TSTAMPREPLY;
		icp->icmp_rtime = iptime();
		icp->icmp_ttime = icp->icmp_rtime;	/* bogus, do later! */
		goto reflect;
		
	case ICMP_IREQ:
		/* fill in source address zero fields! */
		goto reflect;

	case ICMP_ECHOREPLY:
	case ICMP_TSTAMPREPLY:
	case ICMP_IREQREPLY:
		if (icmplen < ICMP_ADVLENMIN || icmplen < ICMP_ADVLEN(icp))
			goto free;
		icmpsrc.sin_addr = ip->ip_src;
		icmpdst.sin_addr = ip->ip_dst;
		raw_input(dtom(icp), &icmproto, (struct sockaddr *)&icmpsrc,
		  (struct sockaddr *)&icmpdst);
		goto free;

	default:
		goto free;
	}
reflect:
	icmp_reflect(ip);
free:
	m_freem(dtom(ip));
}

/*
 * Reflect the ip packet back to the source
 * TODO: rearrange ip source routing options.
 */
icmp_reflect(ip)
	struct ip *ip;
{
	struct in_addr t;
COUNT(ICMP_REFLECT);

	t = ip->ip_dst;
	ip->ip_dst = ip->ip_src;
	ip->ip_src = t;
	icmp_send(ip);
}

int	generateicmpmsgs = 0;

/*
 * Send an icmp packet back to the ip level,
 * after supplying a checksum.
 */
icmp_send(ip)
	struct ip *ip;
{
	register int hlen = ip->ip_hl << 2;
	register struct icmp *icp;
	register struct mbuf *m = dtom(ip);

COUNT(ICMP_SEND);
	if (!generateicmpmsgs)
		return;
	icp = mtod(m, struct icmp *);
	icp->icmp_cksum = 0;
	icp->icmp_cksum = in_cksum(m, ip->ip_len - hlen);
	m->m_off -= hlen;
	m->m_len += hlen;
	(void) ip_output(m, 0, 0, 0);
}

n_time
iptime()
{
	int s = spl6();
	u_long t;

COUNT(IPTIME);
	t = (time % SECDAY) * 1000 + lbolt * hz;
	splx(s);
	return (htonl(t));
}
