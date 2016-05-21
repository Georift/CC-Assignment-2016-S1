#include <cnet.h>
#include <stdlib.h>
#include <string.h>

/*  This is an implementation of a stop-and-wait data link protocol.
    It is based on Tanenbaum's `protocol 4', 2nd edition, p227
    (or his 3rd edition, p205).
    This protocol employs only data and acknowledgement frames -
    piggybacking and negative acknowledgements are not used.

    It is currently written so that only one node (number 0) will
    generate and transmit messages and the other (number 1) will receive
    them. This restriction seems to best demonstrate the protocol to
    those unfamiliar with it.
    The restriction can easily be removed by "commenting out" the line

	    if(nodeinfo.nodenumber == 0)

    in reboot_node(). Both nodes will then transmit and receive (why?).

    Note that this file only provides a reliable data-link layer for a
    network of 2 nodes.
 */

typedef enum    { DL_DATA, DL_ACK }   Framekind;

typedef struct {
        char data[MAX_MESSAGE_SIZE];
} Data;

typedef struct {
    CnetAddr src_addr;
    CnetAddr dest_addr;
    int      type;           
    Data     data;
} Packet;

typedef struct {
    Framekind    kind;      	/* only ever DL_DATA or DL_ACK */
    unsigned int len;       	/* the length of the msg field only */
    int          checksum;  	/* checksum of the whole frame */
    int          seq;       	/* only ever 0 or 1 */
    Packet       msg;
} Frame;

#define FRAME_HEADER_SIZE  (sizeof(Frame) - sizeof(Packet))
#define FRAME_SIZE(f)      (FRAME_HEADER_SIZE + f.len)


static  Packet   *lastmsg;
static  size_t    lastlength		= 0;
static  CnetTimerID	lasttimer		= NULLTIMER;

static  int       	ackexpected		= 0;
static	int		nextframetosend		= 0;
static	int		frameexpected		= 0;


/* physical layer input */
static void transmit_frame(Packet *msg, Framekind kind,
                           size_t length, int seqno)
{
    Frame       f;
    int		link = 1;

    f.kind      = kind;
    f.seq       = seqno;
    f.checksum  = 0;
    f.len       = length;

    switch (kind) {
    case DL_ACK :
        printf("ACK transmitted, seq=%d\n", seqno);
	break;

    case DL_DATA: {
	CnetTime	timeout;

        printf(" DATA transmitted, seq=%d\n", seqno);
        memcpy(&f.msg, (char *)msg, (int)length);

	timeout = FRAME_SIZE(f)*((CnetTime)8000000 / linkinfo[link].bandwidth) +
				linkinfo[link].propagationdelay;

        lasttimer = CNET_start_timer(EV_TIMER1, 3 * timeout, 0);
	break;
      }
    }
    length      = FRAME_SIZE(f);
    f.checksum  = CNET_ccitt((unsigned char *)&f, (int)length);
    CHECK(CNET_write_physical(link, (char *)&f, &length));
}


/**
 * received a message from application layer.
 * push this down to the stack.
 */
static void application_ready(CnetEvent ev, CnetTimerID timer, CnetData data)
{
    CnetAddr destaddr;

    lastlength  = sizeof(Packet);
    CHECK(CNET_read_application(&destaddr, (char *)lastmsg, &lastlength));
    CNET_disable_application(ALLNODES);

    printf("down from application, seq=%d\n", nextframetosend);
    transmit_frame(lastmsg, DL_DATA, lastlength, nextframetosend);
    nextframetosend = 1-nextframetosend;
}

/**
 * We have received a packet on the physical layer.
 *  - perform a checksum on the packet
 *
 *  Push up to the data link layer
 */
static void physical_ready(CnetEvent ev, CnetTimerID timer, CnetData data)
{
    Frame        f;
    size_t      len;
    int          link, checksum;

    len         = sizeof(Frame);

    /* link will now contain the link we received it on */
    CHECK(CNET_read_physical(&link, (char *)&f, &len));

    /* check the checksum and throw out if wrong */
    checksum    = f.checksum;
    f.checksum  = 0;
    if(CNET_ccitt((unsigned char *)&f, (int)len) != checksum) {
        /* the timer will simply run out and the node resends. */
        printf("\t\t\t\tBAD checksum - frame ignored\n");
        return;           /* bad checksum, ignore frame */
    }

    /* check what type of frame we have received */
    switch (f.kind) {

        /* we got an ACK check what frame we are on */
        case DL_ACK :
            if(f.seq == ackexpected) {
                printf("\t\t\t\tACK received, seq=%d\n", f.seq);
                CNET_stop_timer(lasttimer);
                ackexpected = 1-ackexpected;
                CNET_enable_application(ALLNODES);
            }
        break;

        /* we got data check if it was the expected seq number
         * and send back an ACK if so.
         * If it's not, ignore it and let the timeout occur.
         */
        case DL_DATA :
            printf("\t\t\t\tDATA received, seq=%d, ", f.seq);
            if(f.seq == frameexpected) {
                printf("up to application\n");
                len = f.len;
                CHECK(CNET_write_application((char *)&f.msg, &len));
                frameexpected = 1-frameexpected;
                transmit_frame((Packet *)NULL, DL_ACK, 0, f.seq);
            }
            else
            {
                printf("ignored\n");
            }
        break;
    }
}


static void timeouts(CnetEvent ev, CnetTimerID timer, CnetData data)
{
    printf("timeout, seq=%d\n", ackexpected);
    transmit_frame(lastmsg, DL_DATA, lastlength, ackexpected);
}


static void showstate(CnetEvent ev, CnetTimerID timer, CnetData data)
{
    printf(
    "\n\tackexpected\t= %d\n\tnextframetosend\t= %d\n\tframeexpected\t= %d\n",
		    ackexpected, nextframetosend, frameexpected);
}


void reboot_node(CnetEvent ev, CnetTimerID timer, CnetData data)
{
    if(nodeinfo.nodenumber > 1) {
        fprintf(stderr,"This is not a 2-node network!\n");
        exit(1);
    }

    lastmsg	= calloc(1, sizeof(Packet));

    CHECK(CNET_set_handler( EV_APPLICATIONREADY, application_ready, 0));
    CHECK(CNET_set_handler( EV_PHYSICALREADY,    physical_ready, 0));
    CHECK(CNET_set_handler( EV_TIMER1,           timeouts, 0));
    CHECK(CNET_set_handler( EV_DEBUG0,           showstate, 0));

    CHECK(CNET_set_debug_string( EV_DEBUG0, "State"));

	CNET_enable_application(ALLNODES);
}
