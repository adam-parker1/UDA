/*---------------------------------------------------------------
* Puts the passed Data to the serverside plugin
*
* Input Arguments:  1) plugin library call
*                   2) the data passed to the plugin
*
* Returns:
*--------------------------------------------------------------*/

#include "udaPutAPI.h"

#include <logging/logging.h>
#include <structures/struct.h>
#include <clientserver/initStructs.h>
#include <clientserver/errorLog.h>
#include <clientserver/printStructs.h>
#include <clientserver/allocData.h>

#include "makeClientRequestBlock.h"
#include "udaGetAPI.h"
#include "udaClient.h"

// Create a list of data blocks to be sent to the server
// Each block has a private name
// Put data are available to server plugins as a set of named arguments in arbitrary order
// It is more efficient to send multiple blocks simultaneously when a Long Fat Pipe is involved
// Server side functions may require multiple array arguments, e.g. NAG routines

// Send multiple data blocks to the server

#ifndef FATCLIENT
static unsigned short idamGetAPICalledOnce = 0;
#endif

int idamPutListAPI(const char* putInstruction, PUTDATA_BLOCK_LIST* inPutDataBlockList)
{

    int err = 0;
    REQUEST_BLOCK request_block;
    PUTDATA_BLOCK_LIST emptyPutDataBlockList;
    PUTDATA_BLOCK_LIST* putDataBlockList = NULL;

//-------------------------------------------------------------------------
// Pass an empty structure rather than NULL (Caller is responsible for freeing)

    if (inPutDataBlockList != NULL)
        putDataBlockList = inPutDataBlockList;
    else {
        putDataBlockList = &emptyPutDataBlockList;
        initIdamPutDataBlockList(putDataBlockList);
    }

//-------------------------------------------------------------------------
// All client/server initialisation is controlled by the main API: idamGetAPI
// This needs to have been called at least once before a put! - *** temporary fix!!!
// This problem also causes the application malloclog and the userdefinedtypelist heaps to be overwritten.
// Copy and replace to preserve the application heap

#ifndef FATCLIENT
    if (!idamGetAPICalledOnce) {
        LOGMALLOCLIST* oldlogmalloclist = logmalloclist;
        USERDEFINEDTYPELIST* olduserdefinedtypelist = userdefinedtypelist;
        logmalloclist = NULL;
        userdefinedtypelist = NULL;
        int h = idamGetAPI("help::ping()", "");
        idamFree(h);
        idamGetAPICalledOnce = 1;
        lastMallocIndex = 0;
        logmalloclist = oldlogmalloclist;
        userdefinedtypelist = olduserdefinedtypelist;
    }
#endif

//-------------------------------------------------------------------------
// Initialise the Client Data Request Structure

    initRequestBlock(&request_block);

//------------------------------------------------------------------------------
// Build the Request Data Block (Version and API dependent)

    if ((err = makeClientRequestBlock(putInstruction, "", &request_block)) != 0) {
        closeIdamError(&idamerrorstack);
        if (idamerrorstack.nerrors == 0) {
            IDAM_LOGF(LOG_ERROR, "Error processing the put instruction [%s]\n", putInstruction);
            addIdamError(&idamerrorstack, CODEERRORTYPE, __func__, 999, "Error processing the put instruction");
        }
        return -err;
    }

    printRequestBlock(request_block);

//-------------------------------------------------------------------------
// Pass an empty structure rather than NULL

//-------------------------------------------------------------------------
// Data to Put to the server

    request_block.put = 1; // flags the direction of data (0 is default => get operation)
    request_block.putDataBlockList = *putDataBlockList;

    err = idamClient(&request_block);

    return err;

}


// Send a single data block to the server

int idamPutAPI(const char* putInstruction, PUTDATA_BLOCK* inPutData)
{
    int err = 0;
    REQUEST_BLOCK request_block;
    PUTDATA_BLOCK emptyPutDataBlock;
    PUTDATA_BLOCK* putData = NULL;

//-------------------------------------------------------------------------
// Pass an empty structure rather than NULL (Caller is responsible for freeing)

    if (inPutData != NULL)
        putData = inPutData;
    else {
        putData = &emptyPutDataBlock;
        initIdamPutDataBlock(putData);
    }

//-------------------------------------------------------------------------
// All client/server initialisation is controlled by the main API: idamGetAPI
// This needs to have been called at least once before a put! - *** temporary fix!!!
// This problem also causes the application malloclog and the userdefinedtypelist heaps to be overwritten.
// Copy and replace to preserve the application heap

#ifndef FATCLIENT
    if (!idamGetAPICalledOnce) {
        LOGMALLOCLIST* oldlogmalloclist = logmalloclist;
        USERDEFINEDTYPELIST* olduserdefinedtypelist = userdefinedtypelist;
        logmalloclist = NULL;
        userdefinedtypelist = NULL;
        int h = idamGetAPI("help::ping()", "");
        idamFree(h);
        idamGetAPICalledOnce = 1;
        lastMallocIndex = 0;
        logmalloclist = oldlogmalloclist;
        userdefinedtypelist = olduserdefinedtypelist;
    }
#endif

//-------------------------------------------------------------------------
// Initialise the Client Data Request Structure

    initRequestBlock(&request_block);

//------------------------------------------------------------------------------
// Build the Request Data Block (Version and API dependent)

    if ((err = makeClientRequestBlock(putInstruction, "", &request_block)) != 0) {
        closeIdamError(&idamerrorstack);
        if (idamerrorstack.nerrors == 0) {
            IDAM_LOGF(LOG_ERROR, "Error processing the put instruction [%s]\n", putInstruction);
            addIdamError(&idamerrorstack, CODEERRORTYPE, __func__, 999, "Error processing the put instruction");
        }
        return -err;
    }

    printRequestBlock(request_block);

//-------------------------------------------------------------------------
// Data to Put to the server

    request_block.put = 1; // flags the direction of data (0 is default => get operation)

    addIdamPutDataBlockList(putData, &request_block.putDataBlockList);
    err = idamClient(&request_block);

//-------------------------------------------------------------------------
// Free Heap

    freeIdamClientPutDataBlockList(&request_block.putDataBlockList);

    return err;

}

