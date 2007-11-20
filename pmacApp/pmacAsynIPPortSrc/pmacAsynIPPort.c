/*
   NOTES
   
   This driver is an asyn interpose driver intended for use with pmacAsynMotor to allow communication over ethernet to a PMAC.

   *** Ensure I3=2 and I6=1 on the PMAC before using this driver. ***
   
   This driver supports sending ascii commands to the PMAC over asyn IP and obtaining the response. The driver uses the PMAC ethernet 
   packets VR_PMAC_GETRESPONSE, VR_PMAC_READREADY and VR_PMAC_GETBUFFER to send commands and get responses. The PMAC may send responses 
   in several different formats.  
   1) PMAC ascii command responses for single/multiple commands (e.g. I113 I114 I130 I131 I133) are in an ACK terminated 
      form as follows (CR seperates the cmd responses):
         data<CR(13)>data<CR(13)>data<CR(13)><ACK(6)>
   2) PMAC error responses to ascii commands ARE NOT ACK terminated as follows:
         <BELL(7)>ERRxxx<CR(13)>
   3) PMAC may also return the following:
         <STX(2)>data<CR(13)> 
   
   This driver can send ctrl commands (ctrl B/C/F/G/P/V) to the pmac (using VR_CTRL_REPONSE packet) however because the resulting  response 
   data is not terminated as above the driver does not know when all the response data has been received. The response data will therefore 
   only be returned after the asynUser.timeout has expired.
   
   This driver supports the octet flush method and issues a VR_PMAC_FLUSH to the PMAC.
   
   This driver does NOT support large buffer transfers (VR_PMAC_WRITEBUFFER, VR_FWDOWNLOAD) or set/get of DPRAM (VR_PMAC_SETMEM etc) or changing
   comms setup (VR_IPADDRESS, VR_PMAC_PORT)


   REVISION HISTORY
   
   10 Aug 07 - Pete Leicester - Diamond Light Source
   Modified to handle responses other than those ending in <ACK> (e.g. errors) - No longer used asyn EOS.
   
   9 Aug 07 - Pete Leicester - Diamond Light Source
   Initial version reliant on asyn EOS to return <ACK> terminated responses.            
*/  


#include <stddef.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include <cantProceed.h>
#include <epicsAssert.h>
#include <epicsStdio.h>
#include <epicsString.h>
#include <iocsh.h>

#include <epicsExport.h>
#include "asynDriver.h"
#include "asynOctet.h"
#include "pmacAsynIPPort.h"

#include <netinet/in.h>

#define ETHERNET_DATA_SIZE 1492
#define INPUT_SIZE        (ETHERNET_DATA_SIZE+1)  /* +1 to allow space to add terminating ACK */
#define STX   '\2'
#define CTRLB '\2'
#define CTRLC '\3'
#define ACK   '\6'
#define CTRLF '\6'
#define BELL  '\7'
#define CTRLG '\7'
#define CTRLP '\16'
#define CTRLV '\22'
#define CTRLX '\24'

/* PMAC ethernet command structure */
#pragma pack(1)
typedef struct tagEthernetCmd
{
    unsigned char RequestType;
    unsigned char Request;
    unsigned short wValue;
    unsigned short wIndex;
    unsigned short wLength; /* length of bData */
    unsigned char bData[ETHERNET_DATA_SIZE];
} ethernetCmd;
#pragma pack()

#define ETHERNET_CMD_HEADER ( sizeof(ethernetCmd) - ETHERNET_DATA_SIZE )

/* PMAC ethernet commands - RequestType field */
#define VR_UPLOAD   0xC0
#define VR_DOWNLOAD 0x40

/* PMAC ethernet commands - Request field */
#define VR_PMAC_SENDLINE    0xB0
#define VR_PMAC_GETLINE     0xB1
#define VR_PMAC_FLUSH       0xB3
#define VR_PMAC_GETMEM      0xB4
#define VR_PMAC_SETMEN      0xB5
#define VR_PMAC_SETBIT      0xBA
#define VR_PMAC_SETBITS     0xBB
#define VR_PMAC_PORT        0xBE
#define VR_PMAC_GETRESPONSE 0xBF
#define VR_PMAC_READREADY   0xC2
#define VR_CTRL_RESPONSE    0xC4
#define VR_PMAC_GETBUFFER   0xC5
#define VR_PMAC_WRITEBUFFER 0xC6
#define VR_PMAC_WRITEERROR  0xC7
#define VR_FWDOWNLOAD       0xCB
#define VR_IPADDRESS        0xE0

/* PMAC control character commands (VR_CTRL_RESPONSE cmd) */
static char ctrlCommands[] = { CTRLB,CTRLC,CTRLF,CTRLG,CTRLP,CTRLV };

typedef struct pmacPvt {
    char          *portName;
    int           addr;
    asynInterface pmacInterface;
    asynOctet     *poctet;  /* The methods we're overriding */
    void          *octetPvt;
    asynUser      *pasynUser;     /* For connect/disconnect reporting */
    ethernetCmd   *poutCmd;
    ethernetCmd   *pinCmd;
    char          *inBuf;
    unsigned int  inBufHead;
    unsigned int  inBufTail;
    } pmacPvt;

/*
   Notes on asyn
   use asynUser.timeout to specify I/O request timeouts in seconds
   asynStatus may return asynSuccess(0),asynTimeout(1),asynOverflow(2) or asynError(3)
   eomReason may return ASYN_EOM_CNT (1:Request count reached), ASYN_EOM_EOS (2:End of String detected), ASYN_EOM_END (4:End indicator detected)
   asynError indicates that asynUser.errorMessage has been populated by epicsSnprintf(). 
*/

/* Connect/disconnect handling */
static void pmacInExceptionHandler(asynUser *pasynUser,asynException exception);

/* asynOctet methods */
static asynStatus writeIt(void *ppvt,asynUser *pasynUser,
    const char *data,size_t numchars,size_t *nbytesTransfered);
static asynStatus writeRaw(void *ppvt,asynUser *pasynUser,
    const char *data,size_t numchars,size_t *nbytesTransfered);
static asynStatus readIt(void *ppvt,asynUser *pasynUser,
    char *data,size_t maxchars,size_t *nbytesTransfered,int *eomReason);
static asynStatus readRaw(void *ppvt,asynUser *pasynUser,
    char *data,size_t maxchars,size_t *nbytesTransfered,int *eomReason);
static asynStatus flushIt(void *ppvt,asynUser *pasynUser);
static asynStatus registerInterruptUser(void *ppvt,asynUser *pasynUser,
    interruptCallbackOctet callback, void *userPvt,void **registrarPvt);
static asynStatus cancelInterruptUser(void *drvPvt,asynUser *pasynUser,
     void *registrarPvt);
static asynStatus setInputEos(void *ppvt,asynUser *pasynUser,
    const char *eos,int eoslen);
static asynStatus getInputEos(void *ppvt,asynUser *pasynUser,
    char *eos,int eossize ,int *eoslen);
static asynStatus setOutputEos(void *ppvt,asynUser *pasynUser,
    const char *eos,int eoslen);
static asynStatus getOutputEos(void *ppvt,asynUser *pasynUser,
    char *eos,int eossize,int *eoslen);
static asynOctet octet = {
    writeIt,writeRaw,readIt,readRaw,flushIt,
    registerInterruptUser, cancelInterruptUser,
    setInputEos,getInputEos,setOutputEos,getOutputEos
};

static asynStatus readResponse(pmacPvt *pPmacPvt, asynUser *pasynUser, size_t maxchars, size_t *nbytesTransfered, int *eomReason );
static int pmacReadReady(pmacPvt *pPmacPvt, asynUser *pasynUser );
static int pmacFlush(pmacPvt *pPmacPvt, asynUser *pasynUser );

epicsShareFunc int pmacAsynIPPortConfigure(const char *portName,int addr)
{
    pmacPvt       *pPmacPvt;
    asynInterface *plowerLevelInterface;
    asynStatus    status;
    asynUser      *pasynUser;
    size_t        len;

    len = sizeof(pmacPvt) + strlen(portName) + 1;
    pPmacPvt = callocMustSucceed(1,len,"pmacAsynIPPortConfig");
    pPmacPvt->portName = (char *)(pPmacPvt+1);
    strcpy(pPmacPvt->portName,portName);
    pPmacPvt->addr = addr;
    pPmacPvt->pmacInterface.interfaceType = asynOctetType;
    pPmacPvt->pmacInterface.pinterface = &octet;
    pPmacPvt->pmacInterface.drvPvt = pPmacPvt;
    pasynUser = pasynManager->createAsynUser(0,0);
    pPmacPvt->pasynUser = pasynUser;
    pPmacPvt->pasynUser->userPvt = pPmacPvt;
    status = pasynManager->connectDevice(pasynUser,portName,addr);
    if(status!=asynSuccess) {
        printf("pmacAsynIPPortConfigure: %s connectDevice failed\n",portName);
        pasynManager->freeAsynUser(pasynUser);
        free(pPmacPvt);
        return -1;
    }
    status = pasynManager->exceptionCallbackAdd(pasynUser,pmacInExceptionHandler);
    if(status!=asynSuccess) {
        printf("pmacAsynIPPortConfigure: %s exceptionCallbackAdd failed\n",portName);
        pasynManager->freeAsynUser(pasynUser);
        free(pPmacPvt);
        return -1;
    }
    status = pasynManager->interposeInterface(portName,addr,
       &pPmacPvt->pmacInterface,&plowerLevelInterface);
    if(status!=asynSuccess) {
        printf("pmacAsynIPPortConfigure: %s interposeInterface failed\n",portName);
        pasynManager->exceptionCallbackRemove(pasynUser);
        pasynManager->freeAsynUser(pasynUser);
        free(pPmacPvt);
        return -1;
    }
    pPmacPvt->poctet = (asynOctet *)plowerLevelInterface->pinterface;
    pPmacPvt->octetPvt = plowerLevelInterface->drvPvt;

    status = pPmacPvt->poctet->setInputEos(pPmacPvt->octetPvt, pasynUser, "\6", 1);
    if (status) {
        asynPrint(pasynUser, ASYN_TRACE_ERROR,
                  "pmacAsynIPPortConfigure: unable to set input EOS on %s: %s\n", 
                  portName, pasynUser->errorMessage);
        return -1;
    }
    status = pPmacPvt->poctet->setOutputEos(pPmacPvt->octetPvt, pasynUser, "\r", 1);
    if (status) {
        asynPrint(pasynUser, ASYN_TRACE_ERROR,
                  "pmacAsynIPPortConfigure: unable to set output EOS on %s: %s\n", 
                  portName, pasynUser->errorMessage);
        return -1;
    }


    pPmacPvt->poutCmd = callocMustSucceed(1,sizeof(ethernetCmd),"pmacAsynIPPortConfig");
    pPmacPvt->pinCmd = callocMustSucceed(1,sizeof(ethernetCmd),"pmacAsynIPPortConfig");
    
    pPmacPvt->inBuf = callocMustSucceed(1,INPUT_SIZE,"pmacAsynIPPortConfig");
    
    pPmacPvt->inBufHead = 0;
    pPmacPvt->inBufTail = 0;

    asynPrint(pasynUser,ASYN_TRACE_FLOW, "Done pmacAsynIPPortConfig OK\n");
   return(0);
}

static void pmacInExceptionHandler(asynUser *pasynUser,asynException exception)
{
    pmacPvt *pPmacPvt = (pmacPvt *)pasynUser->userPvt;
    asynPrint(pasynUser,ASYN_TRACE_FLOW, "pmacInExceptionHandler\n");

    if (exception == asynExceptionConnect) {
        pPmacPvt->inBufHead = 0;
        pPmacPvt->inBufTail = 0;
    }
}

/*
    Read reponse data from PMAC into buffer pmacPvt.inBuf. If there is no data in the asyn buffer then issue
    pmac GETBUFFER command to get any outstanding data still on the PMAC.
*/
static asynStatus readResponse(pmacPvt *pPmacPvt, asynUser *pasynUser, size_t maxchars, size_t *nbytesTransfered, int *eomReason )
{
    ethernetCmd* inCmd;
    asynStatus status = asynSuccess;
    size_t thisRead;
    *nbytesTransfered = 0;
    if (maxchars>INPUT_SIZE) maxchars = INPUT_SIZE;

    status = pPmacPvt->poctet->readRaw(pPmacPvt->octetPvt,
         pasynUser,pPmacPvt->inBuf,maxchars,&thisRead,eomReason);
    asynPrint(pasynUser,ASYN_TRACE_FLOW, "%s readResponse1 maxchars=%d, thisRead=%d, eomReason=%d, status=%d\n",pPmacPvt->portName, maxchars, thisRead, *eomReason, status);
         
    if (status == asynTimeout && thisRead == 0 && pasynUser->timeout>0) {
         /* failed to read as many characters as required into the input buffer, 
            check for more response data on the PMAC */
         if ( pmacReadReady(pPmacPvt,pasynUser )) { 
            /* response data is available on the PMAC */
            /* issue a getbuffer command */
            inCmd = pPmacPvt->pinCmd;
            inCmd->RequestType = VR_UPLOAD;
            inCmd->Request = VR_PMAC_GETBUFFER;
            inCmd->wValue = 0;
            inCmd->wIndex = 0;
            inCmd->wLength = htons(0);
            status = pPmacPvt->poctet->writeRaw(pPmacPvt->octetPvt,
               pasynUser,(char*)pPmacPvt->pinCmd,ETHERNET_CMD_HEADER,nbytesTransfered);
               
            asynPrintIO(pasynUser,ASYN_TRACE_FLOW,(char*)pPmacPvt->pinCmd,ETHERNET_CMD_HEADER,
                "%s write GETBUFFER\n",pPmacPvt->portName);
                
            /* We have nothing to return at the moment so read again */
            status = pPmacPvt->poctet->readRaw(pPmacPvt->octetPvt,
                pasynUser,pPmacPvt->inBuf,maxchars,&thisRead,eomReason);
               
            asynPrint(pasynUser,ASYN_TRACE_FLOW, "%s readResponse2 maxchars=%d, thisRead=%d, eomReason=%d, status=%d\n",pPmacPvt->portName, maxchars, thisRead, *eomReason, status);                    
        } 
    } 
          
    if (thisRead>0) {
        if (status == asynTimeout) status = asynSuccess;
        /* TODO do we need eomReason = 0 here ??? */
        *nbytesTransfered = thisRead;   
        pPmacPvt->inBufTail = 0;
        pPmacPvt->inBufHead = thisRead;
    }

    return status; 
}


/*
   Send ReadReady command to PMAC to discover if there is any data to read from it.
   Returns: 0 - no data available
            1 - data available
*/            
static int pmacReadReady(pmacPvt *pPmacPvt, asynUser *pasynUser )
{
    ethernetCmd cmd;
    unsigned char data[2];
    asynStatus status;
    size_t thisRead;
    size_t nbytesTransfered = 0;
    int eomReason = 0;
    int retval = 0;

    cmd.RequestType = VR_UPLOAD;
    cmd.Request = VR_PMAC_READREADY;
    cmd.wValue = 0;
    cmd.wIndex = 0;
    cmd.wLength = htons(2);
    status = pPmacPvt->poctet->writeRaw(pPmacPvt->octetPvt,
        pasynUser,(char*)&cmd,ETHERNET_CMD_HEADER,&nbytesTransfered);
    
    if(status!=asynSuccess) { 
        asynPrintIO(pasynUser,ASYN_TRACE_ERROR,(char*)&cmd,ETHERNET_CMD_HEADER,
            "%s write pmacReadReady fail\n",pPmacPvt->portName);
    }
     
    status = pPmacPvt->poctet->readRaw(pPmacPvt->octetPvt,
         pasynUser,data,2,&thisRead,&eomReason);

    if(status==asynSuccess) {
         if (thisRead==2 && data[0] != 0) {
             retval = 1;
         }    
         asynPrintIO(pasynUser,ASYN_TRACE_FLOW,data,thisRead,
             "%s read pmacReadReady OK thisRead=%d\n",pPmacPvt->portName,thisRead);
    } else {
        asynPrint(pasynUser,ASYN_TRACE_ERROR, "%s read pmacReadReady failed status=%d,retval=%d",pPmacPvt->portName,status,retval);
    }
    return retval;            
}

/*
   Send Flush command to PMAC and wait for confirmation ctrlX to be returned. 
   Returns: 0 - failed
            1 - success
*/            
static int pmacFlush(pmacPvt *pPmacPvt, asynUser *pasynUser )
{
    ethernetCmd cmd;
    unsigned char data[2];
    asynStatus status = asynSuccess;
    size_t thisRead;
    size_t nbytesTransfered = 0;
    int eomReason = 0;
    int retval = 0;

    cmd.RequestType = VR_DOWNLOAD;
    cmd.Request = VR_PMAC_FLUSH;
    cmd.wValue = 0;
    cmd.wIndex = 0;
    cmd.wLength = 0;
    status = pPmacPvt->poctet->writeRaw(pPmacPvt->octetPvt,
        pasynUser,(char*)&cmd,ETHERNET_CMD_HEADER,&nbytesTransfered);
    
    if(status!=asynSuccess) { 
        asynPrintIO(pasynUser,ASYN_TRACE_ERROR,(char*)&cmd,ETHERNET_CMD_HEADER,
            "%s write pmacFlush fail\n",pPmacPvt->portName);
    }
        
    /* read flush acknowledgement character */
    /* NB we don't check what the character is (manual sais ctrlX, we get 0x40 i.e.VR_DOWNLOAD) */
    status = pPmacPvt->poctet->readRaw(pPmacPvt->octetPvt,
         pasynUser,data,1,&thisRead,&eomReason);

    if(status==asynSuccess) {
         asynPrint(pasynUser,ASYN_TRACE_FLOW, "%s read pmacFlush OK\n",pPmacPvt->portName);
         retval = 1;    
    } else {
        asynPrint(pasynUser,ASYN_TRACE_ERROR, "%s read pmacFlush failed - thisRead=%d, eomReason=%d, status=%d\n",pPmacPvt->portName, thisRead, eomReason, status);
    }

    pPmacPvt->inBufTail = 0;
    pPmacPvt->inBufHead = 0;
    
    return retval;            
}


/* asynOctet methods */


/* This function sends either a ascii string command to the PMAC using VR_PMAC_GETRESPONSE or a single control 
   character (ctrl B/C/F/G/P/V) using VR_CTRL_RESPONSE
*/
static asynStatus writeIt(void *ppvt,asynUser *pasynUser,
    const char *data,size_t numchars,size_t *nbytesTransfered)
{
    asynStatus status;
    ethernetCmd* outCmd;
    pmacPvt *pPmacPvt = (pmacPvt *)ppvt;
    size_t nbytesActual = 0;
    asynPrint( pasynUser, ASYN_TRACE_FLOW, "pmacAsynIPPort::writeIt\n" );
    assert(pPmacPvt);
    
    /* NB currently we assume control characters arrive as individual characters/calls to this routine. Idealy we should probably
       scan the data buffer for control commands and do PMAC_GETRESPONSE and CTRL_RESPONSE commands as necessary,  */
    outCmd = pPmacPvt->poutCmd;
    if (numchars==1 && strchr(ctrlCommands,data[0])){
        outCmd->RequestType = VR_UPLOAD;
        outCmd->Request = VR_CTRL_RESPONSE;
        outCmd->wValue = data[0];
        outCmd->wIndex = 0;
        outCmd->wLength = htons(0);
        status = pPmacPvt->poctet->writeRaw(pPmacPvt->octetPvt,
            pasynUser,(char*)pPmacPvt->poutCmd,ETHERNET_CMD_HEADER,&nbytesActual);
        *nbytesTransfered = (nbytesActual==ETHERNET_CMD_HEADER) ? numchars : 0;
    } else {
        if (numchars > ETHERNET_DATA_SIZE) {
            /* NB large data should probably be sent using PMAC_WRITEBUFFER which isnt implemented yet - for the moment just truncate */
            numchars = ETHERNET_DATA_SIZE;
            asynPrint( pasynUser, ASYN_TRACE_ERROR, "writeIt - ERROR TRUNCATED\n" );
        }
        outCmd->RequestType = VR_DOWNLOAD;
        outCmd->Request = VR_PMAC_GETRESPONSE;
        outCmd->wValue = 0;
        outCmd->wIndex = 0;
        outCmd->wLength = htons(numchars);
        memcpy(outCmd->bData,data,numchars);
        status = pPmacPvt->poctet->writeRaw(pPmacPvt->octetPvt,
            pasynUser,(char*)pPmacPvt->poutCmd,numchars+ETHERNET_CMD_HEADER,&nbytesActual);
        *nbytesTransfered = (nbytesActual>ETHERNET_CMD_HEADER) ? (nbytesActual - ETHERNET_CMD_HEADER) : 0;
    }
        
    asynPrintIO(pasynUser,ASYN_TRACE_FLOW,(char*)pPmacPvt->poutCmd,numchars+ETHERNET_CMD_HEADER,
            "%s writeIt\n",pPmacPvt->portName);
                    
    return status;
}


static asynStatus writeRaw(void *ppvt,asynUser *pasynUser,
    const char *data,size_t numchars,size_t *nbytesTransfered)
{
    pmacPvt *pPmacPvt = (pmacPvt *)ppvt;
    asynStatus status = asynSuccess;
    asynPrint( pasynUser, ASYN_TRACE_FLOW, "pmacAsynIPPort::writeRaw\n" );
    assert(pPmacPvt);

    status = pPmacPvt->poctet->writeRaw(pPmacPvt->octetPvt,
        pasynUser,data,numchars,nbytesTransfered);
        
    return status;    
}


/* This function reads data using readRaw into a local buffer and then look for message terminating characters and returns a complete 
   response (or times out). Note that <ACK> is stripped from the end of the response, other responses are returned as is. 
   The PMAC command response may be any of the following:-
       data<CR>data<CR>....data<CR><ACK>
       <BELL>data<CR> e.g. an error <BELL>ERRxxx<CR>
       <STX>data<CR>
   (NB asyn EOS only allows one message terminator to be specified so cannot be used here)    
*/             
static asynStatus readIt(void *ppvt,asynUser *pasynUser,
    char *data,size_t maxchars,size_t *nbytesTransfered,int *eomReason)
{
    pmacPvt *pPmacPvt = (pmacPvt *)ppvt;
    asynStatus status = asynSuccess;
    size_t thisRead = 0;
    size_t nRead = 0;
    double timeout = pasynUser->timeout;
    double timeleft;
    float delay;
    int bell = 0;
 
    asynPrint( pasynUser, ASYN_TRACE_FLOW, "pmacAsynIPPort::readIt\n" );
    assert(pPmacPvt);
    
    if (eomReason) *eomReason = 0;
    if (maxchars > 0) {
        for (;;) {
            if ((pPmacPvt->inBufTail != pPmacPvt->inBufHead)) {
                *data = pPmacPvt->inBuf[pPmacPvt->inBufTail++];
                if (*data == BELL || *data == STX) bell = 1;
                if (*data == '\r' && bell) {  
                    /* <BELL>xxxxxx<CR> or <STX>xxxxx<CR> received - its probably an error response (<BELL>ERRxxx<CR>) - assume there is no more response data to come */
                    nRead++; /* make sure the <CR> is passed to the client app */
                    if (eomReason) *eomReason = ASYN_EOM_EOS;
                    break;
                }
                if (*data == ACK || *data == '\n') {
                    /* <ACK> or <LF> received - assume there is no more response data to come */
                    if (eomReason) *eomReason = ASYN_EOM_EOS;
                    break;
                }
                data++;
                nRead++;
                if (nRead >= maxchars) break;
                continue;
            }
            if(eomReason && *eomReason) break;
            
            /* read data with zero timeout - i.e. get whats already available in the asyn buffer without waiting */
            timeleft = timeout;
            delay = 0;
            do {
                pasynUser->timeout = delay;
                status = readResponse(pPmacPvt, pasynUser, maxchars-nRead, &thisRead, eomReason);
                timeleft -= delay;
                if (delay < 0.5) delay += 0.05; /* lengthen delay up to a maximum of 0.5 sec */
            } while (thisRead==0 && timeleft > 0);
            
            if(status!=asynSuccess || thisRead==0) break;       
        }
    } else if (eomReason) *eomReason = ASYN_EOM_CNT;
    *nbytesTransfered = nRead;
    pasynUser->timeout = timeout; /* restore users timeout */
    
/*    asynPrintIO(pasynUser,ASYN_TRACE_FLOW, data, *nbytesTransfered, "%s readIt nbytesTransfered=%d, eomReason=%d, status=%d\n",pPmacPvt->portName,*nbytesTransfered, *eomReason, status);
*/        
    return status;
}


static asynStatus readRaw(void *ppvt,asynUser *pasynUser,
    char *data,size_t maxchars,size_t *nbytesTransfered,int *eomReason)
{
    pmacPvt *pPmacPvt = (pmacPvt *)ppvt;
    asynStatus status = asynSuccess;
    size_t thisRead;
    size_t nRead = 0;
    asynPrint( pasynUser, ASYN_TRACE_FLOW, "pmacAsynIPPort::readRaw\n" );
    assert(pPmacPvt);

    if(eomReason) *eomReason = 0;
    if (maxchars > 0) {
        for (;;) {
            if ((pPmacPvt->inBufTail != pPmacPvt->inBufHead)) {
                *data++ = pPmacPvt->inBuf[pPmacPvt->inBufTail++];
                nRead++;
                if (nRead >= maxchars) break;
                continue;
            }
            if(eomReason && *eomReason) break;
            status = readResponse(pPmacPvt, pasynUser, maxchars-nRead, &thisRead, eomReason);
        
            if(status!=asynSuccess || thisRead==0) break;
        }
    } else if (eomReason) *eomReason = ASYN_EOM_CNT;
    *nbytesTransfered = nRead;

/*    asynPrintIO(pasynUser,ASYN_TRACE_FLOW, data, *nbytesTransfered, "%s readRaw nbytesTransfered=%d, eomReason=%d, status=%d\n",pPmacPvt->portName,*nbytesTransfered, *eomReason, status);
*/        
    return status;
}


static asynStatus flushIt(void *ppvt,asynUser *pasynUser)
{
    pmacPvt *pPmacPvt = (pmacPvt *)ppvt;
    asynStatus status = asynSuccess;
    asynPrint( pasynUser, ASYN_TRACE_FLOW, "pmacAsynIPPort::flushIt\n" );
    assert(pPmacPvt);

    pmacFlush (pPmacPvt, pasynUser);
    status = pPmacPvt->poctet->flush(pPmacPvt->octetPvt,pasynUser);
    return asynSuccess;
}

static asynStatus registerInterruptUser(void *ppvt,asynUser *pasynUser,
    interruptCallbackOctet callback, void *userPvt,void **registrarPvt)
{
    pmacPvt *pPmacPvt = (pmacPvt *)ppvt;
    asynPrint( pasynUser, ASYN_TRACE_FLOW, "pmacAsynIPPort::registerInterruptUser\n" );
    assert(pPmacPvt);

    return pPmacPvt->poctet->registerInterruptUser(pPmacPvt->octetPvt,
        pasynUser,callback,userPvt,registrarPvt);
} 

static asynStatus cancelInterruptUser(void *drvPvt,asynUser *pasynUser,
     void *registrarPvt)
{
    pmacPvt *pPmacPvt = (pmacPvt *)drvPvt;
    asynPrint( pasynUser, ASYN_TRACE_FLOW, "pmacAsynIPPort::cancelInterruptUser\n" );
    assert(pPmacPvt);

    return pPmacPvt->poctet->cancelInterruptUser(pPmacPvt->octetPvt,
        pasynUser,registrarPvt);
} 

static asynStatus setInputEos(void *ppvt,asynUser *pasynUser,
    const char *eos,int eoslen)
{
    pmacPvt *pPmacPvt = (pmacPvt *)ppvt;
    asynPrint( pasynUser, ASYN_TRACE_FLOW, "pmacAsynIPPort::setInputEos\n" );
    assert(pPmacPvt);

    return pPmacPvt->poctet->setInputEos(pPmacPvt->octetPvt,pasynUser,
      eos,eoslen); 
}

static asynStatus getInputEos(void *ppvt,asynUser *pasynUser,
    char *eos,int eossize,int *eoslen)
{
    pmacPvt *pPmacPvt = (pmacPvt *)ppvt;
    asynPrint( pasynUser, ASYN_TRACE_FLOW, "pmacAsynIPPort::getInputEos\n" );
    assert(pPmacPvt);

    return pPmacPvt->poctet->getInputEos(pPmacPvt->octetPvt,pasynUser,
       eos,eossize,eoslen);
}

static asynStatus setOutputEos(void *ppvt,asynUser *pasynUser,
    const char *eos, int eoslen)
{
    pmacPvt *pPmacPvt = (pmacPvt *)ppvt;
    asynPrint( pasynUser, ASYN_TRACE_FLOW, "pmacAsynIPPort::setOutputEos\n" );
    assert(pPmacPvt);

    return pPmacPvt->poctet->setOutputEos(pPmacPvt->octetPvt,
        pasynUser,eos,eoslen); 
}

static asynStatus getOutputEos(void *ppvt,asynUser *pasynUser,
    char *eos,int eossize,int *eoslen)
{
    pmacPvt *pPmacPvt = (pmacPvt *)ppvt;
    asynPrint( pasynUser, ASYN_TRACE_FLOW, "pmacAsynIPPort::getOutputEos\n" );
    assert(pPmacPvt);

    return pPmacPvt->poctet->getOutputEos(pPmacPvt->octetPvt,
        pasynUser,eos,eossize,eoslen);
}


/* register pmacAsynIPPortConfig*/
static const iocshArg pmacAsynIPPortConfigArg0 =
    { "portName", iocshArgString };
static const iocshArg pmacAsynIPPortConfigArg1 =
    { "addr", iocshArgInt };
static const iocshArg *pmacAsynIPPortConfigArgs[] = 
    {&pmacAsynIPPortConfigArg0,&pmacAsynIPPortConfigArg1};
static const iocshFuncDef pmacAsynIPPortConfigFuncDef =
    {"pmacAsynIPPortConfigure", 2, pmacAsynIPPortConfigArgs};
static void pmacAsynIPPortConfigCallFunc(const iocshArgBuf *args)
{
    pmacAsynIPPortConfigure(args[0].sval,args[1].ival);
}

static void pmacAsynIPPortRegister(void)
{
    static int firstTime = 1;
    if (firstTime) {
        firstTime = 0;
        iocshRegister(&pmacAsynIPPortConfigFuncDef, pmacAsynIPPortConfigCallFunc);
    }
}
epicsExportRegistrar(pmacAsynIPPortRegister);