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

/*
typedef struct {
        char data[MAX_MESSAGE_SIZE];
} Data;
*/
#define MAX_MESSAGE 256

typedef struct {
    Framekind    kind;      	/* only ever DL_DATA or DL_ACK */
    size_t       len;       	/* the length of the msg field only */
    int          checksum;  	/* checksum of the whole frame */
    int          seq;       	/* only ever 0 or 1 */
    CnetAddr src_addr;
    CnetAddr dest_addr;
    char     data[MAX_MESSAGE];
} Frame;

#define FRAME_HEADER_SIZE  (sizeof(Frame) - (sizeof(char) * MAX_MESSAGE))
#define FRAME_SIZE(f)      (FRAME_HEADER_SIZE + f.len)

#define MAX_LINKS 2

static void datalink_down(Frame f, Framekind kind, 
                    int seqno, int link);

static  char   *lastmsg;
//static  size_t    lastlength		= 0;
static  CnetTimerID	lasttimer		= NULLTIMER;

/*
static  int       	ackexpected		= 0;
static	int		nextframetosend		= 0;
static	int		frameexpected		= 0;
*/

static int ackexpected[MAX_LINKS];
static int nextframetosend[MAX_LINKS];
static int frameexpected[MAX_LINKS];

const int routingTable[3][3] = {
    {0, 1, 2}, /* PERTH */
    {1, 0, 1}, /* SYDNEY */
    {1, 1, 0} /* Melbourne */
};

void printFrame(int link, Frame *f, size_t length)
{
    printf("\tLength of frame: %d\n", length);
    if ( NULL == f)
    {
        printf("This is a null pointer");
    }
    else
    {
        printf("\tSeq #: %d\n", f->seq);
        printf("\tSource: %d\n", f->src_addr);
        printf("\tDestination: %d\n", f->dest_addr);
        printf("\tLink: %d\n", link);
        printf("\tType: ", f->kind);
        if (f->kind == DL_DATA)
        {
            printf("DATA\n");
        }
        else
        {
            printf("ACK\n");
        }
    }
}


void printbincharpad(char c)
{
    for (int i = 7; i >= 0; --i)
    {
        putchar( (c & (1 << i)) ? '1' : '0' );
    }
    putchar('\n');
}

static void printCharArray(const char *array, size_t length)
{
    int ii;
    printf("\"");
    for( ii = 0; ii < length; ii++)
    {
        printbincharpad(array[ii]);
    }
    printf("\"");
    printf("\n");

}

/**
 * Network and application layer for receiver
 */
static void network_ready(Frame f, size_t length, int link)
{
    /* check if this is out node, or if we need to route it */
    if (nodeinfo.nodenumber != f.dest_addr)
    {
        printf("NETWORK: We got a node for %d seq %d, we are %d\n         It came from link #%d\n",
                f.dest_addr, f.seq, nodeinfo.nodenumber, link);
        int newLink = routingTable[nodeinfo.nodenumber][f.dest_addr];
        datalink_down(f, DL_DATA, nextframetosend[newLink], newLink);
    }
    else
    {
        /* this is our frame. */
        printf("this is our frame\n");
        printf("frame of size %d about to be written to application\n", length);

        CHECK(CNET_write_application((char *)&f.data, &f.len));
    }

}

/**
 * Data link layer for receiver
 */
static void datalink_ready(int link, Frame f)
{
    int checksum = f.checksum;

    /* remove the checksum from the packet and recompute it */
    f.checksum = 0;
    if(CNET_ccitt((unsigned char *)&f, (int)sizeof(Frame)) != checksum) {
        printf("\t\t\t\tBAD checksum - frame ignored\n");
        return;            /*bad checksum, ignore frame*/
    }

    /* check what type of frame we have received */
    switch (f.kind) {

        /* we got an ACK check what frame we are on */
        case DL_ACK :
            if(f.seq == ackexpected[link]) {
                printf("\t\t\t\tACK received, seq=%d\n", f.seq);
                CNET_stop_timer(lasttimer);
                ackexpected[link] = 1 - ackexpected[link];
                CNET_enable_application(ALLNODES);
            }
        break;

        /* we got data check if it was the expected seq number
         * and send back an ACK if so.
         * If it's not, ignore it and let the timeout occur.
         */
        case DL_DATA :
            printf("\t\t\t\tDATA received, seq=%d, ", f.seq);
            printf("\t\t\t\ton link #%d\n", link);
            if(f.seq == frameexpected[link]) {

                // send ACK then push up network layer
                frameexpected[link] = 1 - frameexpected[link];
                datalink_down(f, DL_ACK, f.seq, link);

                // pass up to the network layer
                printf("DATALINK: Passing packet up to network. size:%d\n", sizeof(Frame));
                network_ready(f, f.len, link);
            }
            else
            {
                printf("Unexpected sequence number %d want %d\n", 
                        f.seq, frameexpected[link]);
            }
        break;
    }
}

/**
 *  Physical Layer Receiver
 */
static void physical_ready(CnetEvent ev, CnetTimerID timer, CnetData data)
{
    int link;
    Frame f;
    size_t len;

    len = sizeof(Frame);

    CHECK(CNET_read_physical(&link, (Frame *)&f, &len));

    printf("PHYSICAL: Just received a frame...\n");
    printFrame(link, &f, len);

    datalink_ready(link, f);
}

/**
 * physical layer sender
 */
static void physical_down(int link, Frame f)
{
    size_t length = sizeof(Frame);
    printf("PHYSICAL: Trying to transmit frame of size %d\n", length);
    printFrame(link, &f, length);
    CHECK(CNET_write_physical(link, (char *)&f, &length));
}

/**
 * Data link layer sender
 */
static void datalink_down(Frame f, Framekind kind, 
                    int seqno, int link)
{
    // take the packet and generate a frame for it.
    f.kind      = kind;
    f.seq       = seqno;
    f.checksum  = 0;

    switch (kind) {
        case DL_ACK :
            printf("ACK transmitted, seq=%d\n", seqno);
            f.src_addr = nodeinfo.nodenumber;
        break;

        case DL_DATA: {
            CnetTime	timeout;

            printf(" DATA transmitted, seq=%d\n", seqno);
            printf("DEBUG: Copying %d from message into packet data\n", f.len);

            timeout = FRAME_SIZE(f)*((CnetTime)8000000 / linkinfo[link].bandwidth) +
                linkinfo[link].propagationdelay;

            lasttimer = CNET_start_timer(EV_TIMER1, 3 * timeout, 0);
            break;
        }
    }

    printf("DATALINK DOWN: frame size: %d of type: %d\n", sizeof(Frame), f.kind);
    f.checksum  = CNET_ccitt((unsigned char *)&f, sizeof(Frame));

    physical_down(link, f);
}

/** 
 * Network layer Sender
 */
static void network_down(Frame f)
{
    // find which node to send it too.
    int linkToUse = routingTable[nodeinfo.nodenumber][f.dest_addr];

    /* encapsulate the message in a packet */
    f.src_addr = nodeinfo.nodenumber;
    printf("NETWORK: send packet on link %d for node %d\n", linkToUse, f.dest_addr);
    //printf("\tnetwork had decided to use link %d to send to %d\n", linkToUse, p.dest_addr);
    datalink_down(f, DL_DATA, nextframetosend[linkToUse], linkToUse);
    nextframetosend[linkToUse] = 1 - nextframetosend[linkToUse];
}


/**
 * Application Layer Sender
 */
static void application_down(CnetEvent ev, CnetTimerID timer, CnetData cdata)
{
    Frame f;
    f.len = sizeof(char) * MAX_MESSAGE;
    printf("DEBUG: Max size of message is %d\n", (sizeof(char) * MAX_MESSAGE));


    CHECK(CNET_read_application(&f.dest_addr, (char *)&f.data, &f.len));
    CNET_disable_application(ALLNODES);

    printf("APPLICATION: Send msg size %d to node #%d\n", f.len, f.dest_addr);

    //printCharArray((char *)&msg, msgLength); 

    printCharArray(f.data, f.len);
    network_down(f);
}

/**
 * HELPER FUNCTIONS
 */
static void timeouts(CnetEvent ev, CnetTimerID timer, CnetData data)
{
    printf("timeout, seq= not sure on link just yet\n");
    //datalink_down(lastmsg, DL_DATA, lastlength, ackexpected, 1);
    printf("Should send on datalink but need to identify links");
}


static void showstate(CnetEvent ev, CnetTimerID timer, CnetData data)
{
    /*printf(
    "\n\tackexpected\t= %d\n\tnextframetosend\t= %d\n\tframeexpected\t= %d\n",
		    ackexpected, nextframetosend, frameexpected);*/
}

void reboot_node(CnetEvent ev, CnetTimerID timer, CnetData data)
{
    lastmsg	= calloc(1, sizeof(char) * MAX_MESSAGE);

    CHECK(CNET_set_handler( EV_APPLICATIONREADY, application_down, 0));
    CHECK(CNET_set_handler( EV_PHYSICALREADY,    physical_ready, 0));
    CHECK(CNET_set_handler( EV_TIMER1,           timeouts, 0));
    CHECK(CNET_set_handler( EV_DEBUG0,           showstate, 0));

    CHECK(CNET_set_debug_string( EV_DEBUG0, "State"));

    if (nodeinfo.nodenumber != 1)
    {
        CNET_enable_application(ALLNODES);
    }
}


