#include <cnet.h>
#include <stdlib.h>
#include <string.h>
typedef enum    { DL_DATA, DL_ACK }   Framekind;

#define MAX_MESSAGE 256
#define MAX_LINKS 4
#define MAX_WINDOW 12

typedef struct {
    Framekind    kind;      	/* only ever DL_DATA or DL_ACK */
    size_t       len;       	/* the length of the msg field only */
    int          checksum;  	/* checksum of the whole frame */
    int          seq;       	/* only ever 0 or 1 */
    int          packetIndex;
    CnetAddr src_addr;
    CnetAddr dest_addr;
    char     data[MAX_MESSAGE];
} Frame;

#define FRAME_HEADER_SIZE  (sizeof(Frame) - (sizeof(char) * MAX_MESSAGE))
#define FRAME_SIZE(f)      (FRAME_HEADER_SIZE + f.len)


static void datalink_down(Frame f, Framekind kind, 
                    int seqno, int link);

static int packetIndex = 0;
Frame lastFrame[MAX_LINKS];
static CnetTimerID timer[MAX_LINKS];

// how large are our buffers?
static int windowSize;

// how much of each links buffer had been used?
static int windowUsed[MAX_LINKS];

// holds all our windows
static Frame window[MAX_LINKS][MAX_WINDOW];

//static int ackexpected[MAX_LINKS];
static int nextframetosend[MAX_LINKS];
//static int frameexpected[MAX_LINKS];

static int expectedFrame[MAX_LINKS];

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

/*
int expectedNextFrame(int link)
{
    // find the next sequency number
    // get the last in the queue 
    int last = window[link - 1][windowUsed[link - 1]].seq;
    int new = -1;

    if (last + 1 > (windowSize - 1))
    {
        new = 0;
    }
    else
    {
        new = last + 1;
    }

    return new;
}
*/

/* find the next sequency number */
int expectedNextFrame(int link)
{
    int nextFrame = expectedFrame[link - 1];
    if (nextFrame + 1 > (windowSize - 1))
    {
        nextFrame = 0;
    }
    else
    {
        nextFrame++;
    }

    return nextFrame;
}


void printbincharpad(char c)
{
    for (int i = 7; i >= 0; --i)
    {
        putchar( (c & (1 << i)) ? '1' : '0' );
    }
    putchar('\n');
}

/*
 * select which EV_TIMER to start depending on the
 * link.
 */
void startTimer(int link, CnetTime timeout)
{
    /* links start at 1 thus decrement it by one */
    switch((link - 1))
    {
        case 0:
            timer[0] = CNET_start_timer(EV_TIMER1, 3 * timeout, 0);
            break;
        case 1:
            timer[1] = CNET_start_timer(EV_TIMER2, 3 * timeout, 0);
            break;
        case 2:
            timer[2] = CNET_start_timer(EV_TIMER3, 3 * timeout, 0);
            break;
        case 3:
            timer[3] = CNET_start_timer(EV_TIMER4, 3 * timeout, 0);
            break;
    }
}

void restartTimer(int link, CnetTime timeout)
{
    switch((link - 1))
    {
        case 0:
            CNET_stop_timer(timer[0]);
            break;
        case 1:
            CNET_stop_timer(timer[1]);
            break;
        case 2:
            CNET_stop_timer(timer[2]);
            break;
        case 3:
            CNET_stop_timer(timer[3]);
            break;
    }

    startTimer(link, timeout);
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

        /* pass a new datalink_down through */
        datalink_down(f, DL_DATA, expectedNextFrame(newLink), newLink);
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

    int ii;
    int accepted = 0;

    /* check what type of frame we have received */
    switch (f.kind) {

        /* we got an ACK check what frame we are on */
        case DL_ACK:
            // we received an ACK. Check what was the next
            // link in the buffer.

            // check from our final frame looping down for
            // the value f.seq 
            for (ii = windowUsed[link - 1]; ii >= 0; ii--)
            {
                if (accepted == 0)
                {
                    if (window[link - 1][ii].seq == f.seq)
                    {
                        // we have gotten acceptance up to this point 

                        // accepted now contains number of frames that
                        // have been accepted
                        accepted = (windowUsed[link - 1] - ii);
                        printf("DATALINK: We have accepted %d frames\n", accepted);
                    }
                }
            }

            // if we have received acceptance remove x number of frames
            // from the window
            if (accepted != 0)
            {
                int jj;
                for (jj = 0; jj < (windowUsed[link - 1] - accepted); jj++)
                {
                    window[link - 1][jj] = window[link - 1][jj + 1];
                }

                printf("DATALINK: Prev. window usage: %d\n", windowUsed[link - 1]);
                windowUsed[link - 1] = windowUsed[link - 1] - accepted;
                printf("DATALINK: New window usage: %d\n", windowUsed[link - 1]);

                /* restart the timeout for this link with the oldest
                 * node in the queue. */

                printf("DATALINK: Restarting timer on link %d\n", link);
                CnetTime timeout = FRAME_SIZE(f)
                    *((CnetTime)8000000 / linkinfo[link].bandwidth) +
                    linkinfo[link].propagationdelay;
                restartTimer(link, timeout);

                /* check if the window has room and reopen application */
                if (windowUsed[link - 1] < (windowSize - 1))
                {
                    CNET_enable_application(f.src_addr);
                }
            }
            else
            {
                // we don't know what ACK number that is....
            }
        break;

        /* we got data check if it was the expected seq number
         * and send back an ACK if so.
         * If it's not, ignore it and let the timeout occur.
         */
        case DL_DATA :
            printf("\t\t\t\tDATA received, seq=%d on link %d\n", f.seq, link);

            /*
             * frame expected should be the last sequence number
             * + 1 in the window for that link */
            if (f.seq == expectedNextFrame(link))
            {
                
                // send ACK then push up network layer
                //frameexpected[link] = 1 - frameexpected[link];

                Frame ack;
                ack.src_addr = nodeinfo.nodenumber;
                ack.dest_addr = f.src_addr;
                ack.seq = f.seq;
                ack.kind = DL_ACK;
                ack.len = 0;

                /* check if we need to add this to our buffer. */
                /* or pass it along. */
                if (f.dest_addr == nodeinfo.nodenumber)
                {
                    datalink_down(ack, DL_ACK, expectedNextFrame(link), link);
                    expectedFrame[link - 1] = expectedNextFrame(link);
                    network_ready(f, f.len, link);
                    printf("DATALINK: Passing packet up to network. size:%d\n", sizeof(Frame));
                }
                else
                {
                    int newLink = routingTable[nodeinfo.nodenumber][f.dest_addr];
                    /* 
                     * we will need to do some routing. 
                     * work out if we have room for it
                     */
                    if (windowUsed[newLink] < windowSize)
                    {
                        /* we have room for it */
                        window[newLink][windowUsed[newLink]] = f;
                        windowUsed[newLink]++;
                        /* increment the expected frame */

                        // send the ack after the window has been filled
                        datalink_down(ack, DL_ACK, expectedNextFrame(link), link);
                        expectedFrame[newLink - 1] = expectedNextFrame(newLink);

                        /* send out the frame we added to the window */
                        datalink_down(f, DL_DATA, expectedNextFrame(newLink), newLink);

                        printf("DATALINK: Frame added to window, ack sent\n");
                    }
                    else
                    {
                        /* we don't have room for it */
                        /* ignore it */
                        printf("DATALINK: Window exhasted, ignore frame.\n");
                    }
                }
            }
            else
            {
                /* we didn't get the correct send an ACK of the last we
                 * received TODO */
                printf("Unexpected sequence number %d want %d\n", 
                        f.seq, expectedNextFrame(link));
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

    printf("PHYSICAL: Just received a frame... %d\n", f.packetIndex);
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
            /* maybe remove this from last frame? */

        break;

        /**
         * we are sending a new frame with data 
         * check if we have room in our window
         *      if so, add and reduce our size.
         *
         * If the window is exhasted we should close
         * application for that link. TODO
         */
        case DL_DATA: {
            CnetTime	timeout;

            /* the window is currently full, ignore the
             * packet and let the sender resend later. */
            if (windowUsed[link] >= windowSize)
            {
                printf("DATA: No room in window for frame.\n");
                /* ignore it */
                return;
            }
            else
            {
                /* we have room inside out window. */
                window[link - 1][windowUsed[link - 1]] = f;
                windowUsed[link - 1]++;
                printf("DATALINK DOWN: Old sequence # is %d\n", expectedFrame[link -1]);
                expectedFrame[link - 1] = expectedNextFrame(link);
            }

            printf(" DATA transmitted, seq=%d\n", seqno);
            printf("DATALINK DOWN: new sequence # is %d\n", expectedFrame[link -1]);

            /* how long should our timeout be */
            timeout = FRAME_SIZE(f)*((CnetTime)8000000 / linkinfo[link].bandwidth) +
                linkinfo[link].propagationdelay;

            /* store our last frame incase of timeouts */
            lastFrame[link - 1] = f;

            restartTimer(link, timeout);
            break;
        }
    }
    f.packetIndex = packetIndex;
    packetIndex++;

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
    datalink_down(f, DL_DATA, expectedNextFrame(linkToUse), linkToUse);
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
    CNET_disable_application(f.dest_addr);

    printf("APPLICATION: Send msg size %d to node #%d\n", f.len, f.dest_addr);

    //printCharArray(f.data, f.len);
    network_down(f);
}

/**
 * HELPER FUNCTIONS
 */
static void timeout1(CnetEvent ev, CnetTimerID timer, CnetData data)
{
    printf("timeout on link #1\n");
    datalink_down(window[0][0], DL_DATA, lastFrame[0].seq, 1);
}

static void timeout2(CnetEvent ev, CnetTimerID timer, CnetData data)
{
    printf("timeout on link #2\n");
    datalink_down(window[1][0], DL_DATA, lastFrame[1].seq, 2);
}

static void timeout3(CnetEvent ev, CnetTimerID timer, CnetData data)
{
    printf("timeout on link #3\n");
    datalink_down(window[2][0], DL_DATA, lastFrame[2].seq, 3);
}

static void timeout4(CnetEvent ev, CnetTimerID timer, CnetData data)
{
    printf("timeout on link #4\n");
    datalink_down(window[3][0], DL_DATA, lastFrame[3].seq, 4);
}

static void showstate(CnetEvent ev, CnetTimerID timer, CnetData data)
{
    /*printf(
    "\n\tackexpected\t= %d\n\tnextframetosend\t= %d\n\tframeexpected\t= %d\n",
		    ackexpected, nextframetosend, frameexpected);*/
}

void reboot_node(CnetEvent ev, CnetTimerID timer, CnetData data)
{
    CHECK(CNET_set_handler( EV_APPLICATIONREADY, application_down, 0));
    CHECK(CNET_set_handler( EV_PHYSICALREADY,    physical_ready, 0));

    CHECK(CNET_set_handler( EV_TIMER1,           timeout1, 0));
    CHECK(CNET_set_handler( EV_TIMER2,           timeout2, 0));
    CHECK(CNET_set_handler( EV_TIMER3,           timeout3, 0));
    CHECK(CNET_set_handler( EV_TIMER4,           timeout4, 0));

    CHECK(CNET_set_handler( EV_DEBUG0,           showstate, 0));

    CHECK(CNET_set_debug_string( EV_DEBUG0, "State"));

    windowSize = MAX_WINDOW / 2;

    int ii;
    for (ii = 0; ii < (MAX_WINDOW / 2); ii++)
    {
        windowUsed[ii] = 0;
    }

    if (nodeinfo.nodenumber == 0)
    {
        CNET_enable_application(ALLNODES);
    }
    else
    {
        CNET_disable_application(ALLNODES);
    }
}


