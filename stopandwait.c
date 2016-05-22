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
    CnetAddr src_addr;
    CnetAddr dest_addr;
    char     data[MAX_MESSAGE];
} Packet;

typedef struct {
    Framekind    kind;      	/* only ever DL_DATA or DL_ACK */
    size_t       len;       	/* the length of the msg field only */
    int          checksum;  	/* checksum of the whole frame */
    int          seq;       	/* only ever 0 or 1 */
    Packet       msg;
} Frame;

#define FRAME_HEADER_SIZE  (sizeof(Frame) - sizeof(Packet))
#define FRAME_SIZE(f)      (FRAME_HEADER_SIZE + f.len)

#define MAX_LINKS 2


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
        printf("\tSource: %d\n", f->msg.src_addr);
        printf("\tDestination: %d\n", f->msg.dest_addr);
        printf("\tLink: %d\n", link);
        printf("\tType: %d\n", f->kind);
    }
}


/* Passing into the physical layer from further up.*/
static void physical_down(int link, Frame *f, size_t length)
{
    //printf("PHYSICAL: Just sent a frame...\n");
    //printFrame(link, (Frame *)&f, length);
    printf("PHYSICAL: Trying to print frame of size %d\n", length);
    CHECK(CNET_write_physical(link, (char *)f, &length));
}

/* input to data link layer from above */
static void datalink_down(Packet *msg, Framekind kind, 
                    size_t length, int seqno, int link)
{
  //printf("\t passed into data link down %d\n", link);
  //if (kind == DL_ACK)
  //{
  //    printf("\t packet is an ACK no destination\n");
  //}
  //else
  //{
  //    printf("\t packet needs to go to %d\n", msg->dest_addr);
  //}
    // take the packet and generate a frame for it.
    Frame f;

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
            length = sizeof(Packet);
            printf("DEBUG: Copying %d from message into packet data\n", length);

            printf("DEBUG: msg src: %d dest: %d\n", msg->src_addr, msg->dest_addr);
            memcpy(&f.msg, (char *)msg, length);
            printf("DEBUG: msg src: %d dest: %d\n", f.msg.src_addr, f.msg.dest_addr);

            timeout = FRAME_SIZE(f)*((CnetTime)8000000 / linkinfo[link].bandwidth) +
                linkinfo[link].propagationdelay;

            lasttimer = CNET_start_timer(EV_TIMER1, 3 * timeout, 0);
            break;
        }
    }
    //length      = FRAME_SIZE(f);
    length = sizeof(Frame);
    printf("DATALINK DOWN: frame size: %d of type: %d\n", length, kind);
    f.checksum  = CNET_ccitt((unsigned char *)&f, (size_t)length);

    physical_down(link, &f, length);
}

/** 
 * given an input message work out which link to send it two.
 */
static void network_down(CnetAddr destAddr, size_t length, char *message)
{
    // find which node to send it too.
    int linkToUse = routingTable[nodeinfo.nodenumber][destAddr];

    /* encapsulate the message in a packet */
    Packet p;
    p.src_addr = nodeinfo.nodenumber;
    p.dest_addr = destAddr;
    memcpy((char *)&p.data, (char *)&message, length);

    printf("NETWORK: send packet on link %d for node %d\n", linkToUse, destAddr);
    //printf("\tnetwork had decided to use link %d to send to %d\n", linkToUse, p.dest_addr);
    datalink_down(&p, DL_DATA, length, nextframetosend[linkToUse], linkToUse);
    nextframetosend[linkToUse] = 1 - nextframetosend[linkToUse];
}

/*
static void printCharArray(const char *array, size_t length)
{
    int ii;
    printf("\"");
    for( ii = 0; ii < length; ii++)
    {
        printf("%c", array[ii]);
    }
    printf("\"");
    printf("\n");
}
*/

/**
 * received a message from application layer.
 * push this down to the stack.
 */
static void application_down(CnetEvent ev, CnetTimerID timer, CnetData cdata)
{
    CnetAddr destAddr;
    char data[MAX_MESSAGE];
    printf("DEBUG: Max size of message is %d\n", sizeof(data));
    size_t msgLength = sizeof(char) * MAX_MESSAGE;


    CHECK(CNET_read_application(&destAddr, (char *)&data, &msgLength));
    CNET_disable_application(ALLNODES);

    printf("APPLICATION: Send msg size %d to node #%d\n", msgLength, destAddr);

    //printCharArray((char *)&msg, msgLength); 

    network_down(destAddr, msgLength, (char *)&data);
}


static void network_ready(Packet *pkt, size_t length, int link)
{
    printf("\twe got a packet passed to us from link %d\n", link);

    /* check if this is out node, or if we need to route it */
    if (nodeinfo.nodenumber != pkt->dest_addr)
    {
        printf("this isn't our frame\n");
        printf("we are node # %d\n", nodeinfo.nodenumber);
        printf("send to node #%d\n", pkt->dest_addr);
        int newLink = routingTable[nodeinfo.nodenumber][pkt->dest_addr];
        datalink_down(pkt, DL_DATA, length, nextframetosend[newLink], newLink);
    }
    else
    {
        /* this is our frame. */
        printf("this is our frame\n");
        printf("frame of size %d about to be written to application\n", length);
        CHECK(CNET_write_application((char *)&pkt->data, &length));
    }

}

static void datalink_ready(int link, Frame *f, size_t len)
{
    int checksum = f->checksum;

    /* remove the checksum from the packet and recompute it */
    f->checksum = 0;
    if(CNET_ccitt((unsigned char *)f, (int)len) != checksum) {
        printf("\t\t\t\tBAD checksum - frame ignored\n");
        return;            /*bad checksum, ignore frame*/
    }

    /* check what type of frame we have received */
    switch (f->kind) {

        /* we got an ACK check what frame we are on */
        case DL_ACK :
            if(f->seq == ackexpected[link]) {
                printf("\t\t\t\tACK received, seq=%d\n", f->seq);
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
            printf("\t\t\t\tDATA received, seq=%d, ", f->seq);
            printf("\t\t\t\ton link #%d\n", link);
            if(f->seq == frameexpected[link]) {

                // send ACK then push up network layer
                frameexpected[link] = 1 - frameexpected[link];
                datalink_down((Packet *)NULL, DL_ACK, 0, f->seq, link);

                // pass up to the network layer
                printf("DATALINK: Passing packet up to network. size:%d\n", len);
                network_ready((Packet *)&f->msg, f->len, link);
            }
            else
            {
                printf("Unexpected sequence number %d want %d\n", 
                        f->seq, frameexpected[link]);
            }
        break;
    }
}

/**
 *  Push up to the data link layer
 */
static void physical_ready(CnetEvent ev, CnetTimerID timer, CnetData data)
{
    int link;
    Frame f;
    size_t len;

    len = sizeof(Frame);

    CHECK(CNET_read_physical(&link, (Frame *)&f, &len));
    if (len != 24)
    {
        printf("PHYSICAL: Just received a frame...\n");
        printFrame(link, &f, len);
    }
    datalink_ready(link, &f, len);
}


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
    lastmsg	= calloc(1, sizeof(Packet));

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


