#include "clientMDS.h"

#include <logging/idamLog.h>
#include <clientserver/initStructs.h>
#include <clientserver/protocol.h>

#include "startup.h"
#include "idamClient.h"

/**
 * Reads the Requested Data
 *
 * @param server MDS+ Server Name
 * @param tree MDS+ Tree Name or Generic)
 * @param node MDS+ Node Name
 * @param treenum MDS+ Tree Number
 * @return
 */
int idamClientMDS(const char* server, const char* tree, const char* node, int treenum)
{
    REQUEST_BLOCK request_block;

//-------------------------------------------------------------------------
// Open the Logs

    if (idamStartup(0) != 0) return PROBLEM_OPENING_LOGS;

//-------------------------------------------------------------------------
// Passed Args

    initRequestBlock(&request_block);

    request_block.request = REQUEST_READ_MDS;
    request_block.exp_number = treenum;

    strcpy(request_block.file, tree);
    strcpy(request_block.signal, node);
    strcpy(request_block.server, server);

    idamLog(LOG_DEBUG, "Routine: ClientMDS\n");
    idamLog(LOG_DEBUG, "Server 		 %s\n", request_block.server);
    idamLog(LOG_DEBUG, "Tree  		 %s\n", request_block.file);
    idamLog(LOG_DEBUG, "Node  		 %s\n", request_block.signal);
    idamLog(LOG_DEBUG, "Tree Number       %d\n", request_block.exp_number);

//-------------------------------------------------------------------------
// Fetch Data

    return idamClient(&request_block);
}