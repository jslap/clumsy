// BWLimiter packets
#include <deque>

extern "C"
{

#include "iup.h"
#include "common.h"
}


#define BW_MIN "1"
#define BW_MAX "1000000"
#define BW_DEFAULT 10

#define BW_QUEUESIZE_MIN "10"
#define BW_QUEUESIZE_MAX "1000"
#define BW_QUEUESIZE_DEFAULT 100

// threshold for how many packet to throttle at most
#define KEEP_AT_MOST 1000

static Ihandle  *inboundCheckbox, *outboundCheckbox, *BWmaxInput, *BWQueueSize;

static volatile short BWLimiterEnabled = 0,
	BWLimiterInbound = 1, BWLimiterOutbound = 1,
	BWMaxValue = BW_DEFAULT,
BWQueueSizeValue = BW_QUEUESIZE_DEFAULT
	 ; 

static PacketNode BWLimiterHeadNode = {0}, BWLimiterTailNode = {0};
static PacketNode *bufHead = &BWLimiterHeadNode, *bufTail = &BWLimiterTailNode;
static int bufSize = 0;
static DWORD bufSizeByte = 0;
static DWORD BWLimiterStartTick = 0;

// pair of time, packet size.
const DWORD TimerGranularity = 35;
static std::deque< std::pair <DWORD, DWORD> > LastSentPackets;
static DWORD TotalLastSentPackets;

static INLINE_FUNCTION short canSendPacket(DWORD curTime)
{
	while (!LastSentPackets.empty() && LastSentPackets.front().first + TimerGranularity < curTime)
	{
		TotalLastSentPackets -= LastSentPackets.front().second;
		LastSentPackets.pop_front();
	}

	DWORD curBwUse = ((TotalLastSentPackets*1000)/(TimerGranularity));
	DWORD MaxBW = (DWORD)BWMaxValue*1024;
	LOG("curTotal(%d): %d curBwUse : %d max: %d", (int)LastSentPackets.size(), (int)TotalLastSentPackets, (int)curBwUse, (int)MaxBW);
	return ( curBwUse < MaxBW );
}

static INLINE_FUNCTION void recordSentPacket(DWORD curTime, DWORD packSize)
{
	LastSentPackets.push_back(std::make_pair(curTime, packSize));
	TotalLastSentPackets += packSize;
}


static INLINE_FUNCTION short isBufEmpty() {
    short ret = bufHead->next == bufTail;
    if (ret) assert(bufSize == 0);
    return ret;
}

static Ihandle *BWLimiterSetupUI() {

    Ihandle *BWLimiterControlsBox = IupHbox(
        IupLabel("Max BW(Kb):"),
        BWmaxInput = IupText(NULL),
		  inboundCheckbox = IupToggle("Inbound", NULL),
		  outboundCheckbox = IupToggle("Outbound", NULL),
		  IupLabel("Q Size(Kb):"),
		  BWQueueSize = IupText(NULL),
        NULL
        );

	 // sync BWLimiter packet number
	 IupSetAttribute(BWmaxInput, "VISIBLECOLUMNS", "3");
	 IupSetAttribute(BWmaxInput, "VALUE", STR(BW_DEFAULT));
	 IupSetCallback(BWmaxInput, "VALUECHANGED_CB", (Icallback)uiSyncInteger);
	 IupSetAttribute(BWmaxInput, SYNCED_VALUE, (char*)&BWMaxValue);
	 IupSetAttribute(BWmaxInput, INTEGER_MAX, BW_MAX);
	 IupSetAttribute(BWmaxInput, INTEGER_MIN, BW_MIN);

	 IupSetCallback(inboundCheckbox, "ACTION", (Icallback)uiSyncToggle);
	 IupSetAttribute(inboundCheckbox, SYNCED_VALUE, (char*)&BWLimiterInbound);
	 IupSetCallback(outboundCheckbox, "ACTION", (Icallback)uiSyncToggle);
	 IupSetAttribute(outboundCheckbox, SYNCED_VALUE, (char*)&BWLimiterOutbound);
	 
	 // sync BWLimiter packet number
	 IupSetAttribute(BWQueueSize, "VISIBLECOLUMNS", "3");
	 IupSetAttribute(BWQueueSize, "VALUE", STR(BW_QUEUESIZE_DEFAULT));
	 IupSetCallback(BWQueueSize, "VALUECHANGED_CB", (Icallback)uiSyncInteger);
	 IupSetAttribute(BWQueueSize, SYNCED_VALUE, (char*)&BWQueueSizeValue);
	 IupSetAttribute(BWQueueSize, INTEGER_MAX, BW_QUEUESIZE_MAX);
	 IupSetAttribute(BWQueueSize, INTEGER_MIN, BW_QUEUESIZE_MIN);



    return BWLimiterControlsBox;
}

static void BWLimiterStartUp() 
{
	if (bufHead->next == NULL && bufTail->next == NULL) {
		bufHead->next = bufTail;
		bufTail->prev = bufHead;
		bufSize = 0;
	} else {
		assert(isBufEmpty());
	}
	BWLimiterStartTick = 0;
	startTimePeriod();
}

static void clearBufPackets(PacketNode *tail) {
    PacketNode *oldLast = tail->prev;
    LOG("Throttled end, send all %d packets. Buffer at max: %s", bufSize, bufSize == KEEP_AT_MOST ? "YES" : "NO");
    while (!isBufEmpty()) {
		 bufSizeByte -= bufTail->prev->packetLen;
        insertAfter(popNode(bufTail->prev), oldLast);
        --bufSize;
    }
    BWLimiterStartTick = 0;
}

static void BWLimiterCloseDown(PacketNode *head, PacketNode *tail)
{
	UNREFERENCED_PARAMETER(tail);
	UNREFERENCED_PARAMETER(head);
	clearBufPackets(tail);
	endTimePeriod();
}

static short BWLimiterProcess(PacketNode *head, PacketNode *tail)
{
	int dropped = 0;

	DWORD currentTime = timeGetTime();
	PacketNode *pac = tail->prev;
	// pick up all packets and fill in the current time
	while (pac != head) 
	{
		if (checkDirection(pac->addr.Direction, BWLimiterInbound, BWLimiterOutbound)) 
		{
			if ( bufSizeByte >= ((DWORD)BWQueueSizeValue*1024))
			{
				LOG("droped with chance, direction %s",
					BOUND_TEXT(pac->addr.Direction));
				freeNode(popNode(pac));
				++dropped;
			}
			else
			{
				insertAfter(popNode(pac), bufHead)->timestamp = timeGetTime();
				++bufSize;
				bufSizeByte += pac->packetLen;
				pac = tail->prev;
			}
		} 
		else 
		{
			pac = pac->prev;
		}
	}


	while (!isBufEmpty() && canSendPacket(currentTime))
	{
		PacketNode *pac = bufTail->prev;
		bufSizeByte -= pac->packetLen;
		recordSentPacket(currentTime, pac->packetLen);
		insertAfter(popNode(bufTail->prev), head); 
		--bufSize;
	}

	if (bufSize > 0)
	{
		LOG("Queued buffers : %d",
			bufSize);
	}


	return (bufSize>0) || (dropped>0);
}

Module BWLimiterModule = {
    "BW Limit",
    (short*)&BWLimiterEnabled,
    BWLimiterSetupUI,
    BWLimiterStartUp,
    BWLimiterCloseDown,
    BWLimiterProcess
};