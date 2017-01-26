// Client/Server Conversation Layer - minimal send and receive
//
// Input Arguments: 1) Client Request Structure
//
/*--------------------------------------------------------------
* BUG:  the value of handle returned in the DATA_BLOCK structure is 1 too high: Fortran accessors? - needs investigation!
*--------------------------------------------------------------*/

#include "idam_client.h"

#include <unistd.h>

#include <logging/idamLog.h>
#include <clientserver/idamErrors.h>
#include <include/idamclientprivate.h>
#include <include/idamclientpublic.h>
#include <include/idamgenstruct.h>
#include <include/idamclient.h>
#include <clientserver/idamErrorLog.h>
#include <clientserver/initStructs.h>
#include <clientserver/manageSockets.h>
#include <stdlib.h>
#include <clientserver/userid.h>
#include <clientserver/printStructs.h>
#include <clientserver/protocol2.h>
#include <clientserver/idamTypes.h>
#include <clientserver/freeDataBlock.h>

#include "closedown.h"
#include "accAPI_C.h"

#ifdef FATCLIENT
#  include "idamServerPlugin.h"
#  include "compressDim.h"
#  include "idamclient.h"
#  include "idamserver.h"
#else
#  include "idamCreateConnection.h"
#  include "createClientXDRStream.h"
#endif

#ifdef MEMCACHE
#include "idamCache.h"
#endif

#ifdef FATCLIENT

int idamServer(CLIENT_BLOCK, REQUEST_BLOCK *, SERVER_BLOCK *, DATA_BLOCK *);
void ncclose(int fh) {}

#endif

#ifdef PERFORMANCETEST
static PERFORMANCE clientPerformance;
#endif

REQUEST_BLOCK* request_block_ptr = NULL;
SOCKETLIST server_socketlist;
#ifndef FATCLIENT
int protocolVersion = 7;
#endif

int malloc_source = MALLOCSOURCENONE;
char clientUsername[STRING_LENGTH] = "client";

int authenticationNeeded = 1; // Enable the mutual authentication conversation at startup

void updateClientBlock(CLIENT_BLOCK* str)
{
    // other structure elements are set when the structure is initialised

    // ****** LEGACY ******

    str->timeout = user_timeout;
    str->clientFlags = clientFlags;
    str->altRank = altRank;
    str->get_datadble = get_datadble;
    str->get_dimdble = get_dimdble;
    str->get_timedble = get_timedble;
    str->get_scalar = get_scalar;
    str->get_bytes = get_bytes;
    str->get_bad = get_bad;
    str->get_meta = get_meta;
    str->get_asis = get_asis;
    str->get_uncal = get_uncal;
    str->get_notoff = get_notoff;
    str->get_nodimdata = get_nodimdata;

    str->privateFlags = privateFlags;
}

void copyClientBlock(CLIENT_BLOCK* str)
{
    // other structure elements are set when the structure is initialised

    // ****** LEGACY ******

    str->timeout = user_timeout;
    str->clientFlags = clientFlags;
    str->altRank = altRank;
    str->get_datadble = get_datadble;
    str->get_dimdble = get_dimdble;
    str->get_timedble = get_timedble;
    str->get_scalar = get_scalar;
    str->get_bytes = get_bytes;
    str->get_bad = get_bad;
    str->get_meta = get_meta;
    str->get_asis = get_asis;
    str->get_uncal = get_uncal;
    str->get_notoff = get_notoff;
    str->get_nodimdata = get_nodimdata;
}

/*
      *** stop using Data_Block_Count-1 to identify the new DATA_BLOCK structure, use newHandleIndex
      *** retain Data_Block_Count as this identifies the upper value of valid handles issued
      *** search for a handle < 0
      *** when freed/initialised, the DATA_BLOCK handle should be set to -1
      *** are there any instances where the data_block handle value is used?
*/

int idamClient(REQUEST_BLOCK* request_block)
{

    // Efficient reduced (filled) tcp packet protocol for efficiency over large RTT fat pipes
    // This client version will only be able to communicate with a version 7+ server

    int err, newHandle;
    int data_block_idx = -1;
    DATA_BLOCK* data_block = NULL;

    int initServer, allocMetaHeap = 0;
    int serverside = 0;

#ifdef MEMCACHE
    static IDAM_CACHE * cache;
    //    request_block_ptr = request_block;    // Passed down to middleware player via global pointer
#endif

#ifndef FATCLIENT
    static int startupStates;
#endif

    DATA_SYSTEM* data_system = NULL;
    SYSTEM_CONFIG* system_config = NULL;
    DATA_SOURCE* data_source = NULL;
    SIGNAL* signal_rec = NULL;
    SIGNAL_DESC* signal_desc = NULL;

    static int system_startup = 1;

    protocolVersion = clientVersion;

    time_t protocol_time;            // Time a Conversation Occured


#ifdef PERFORMANCETEST
    clientPerformance.npoints = 2;
    strcpy(clientPerformance.label[0],"#0: Total Time");
    rc = gettimeofday(&(clientPerformance.tv_start[0]), NULL);
    strcpy(clientPerformance.label[1],"#1: Client Start-up ");
    rc = gettimeofday(&(clientPerformance.tv_start[1]), NULL);
#endif

    //------------------------------------------------------------------------------
    // Open the Socket if this is the First call for Data or the server is known to be dead
    //
    // If this is Not the First Call then assume the server is asleep and waiting for a request
    // Check the time since last Call for Data does not exceed a threshold:
    //  If threshold exceeded then assume the Server has closed down.
    //      => Close old Socket (the server will automatically die if awake)
    //      => Open New Connection and use the Startup Protocol
    //
    //------------------------------------------------------------------------------
    //-------------------------------------------------------------------------
    // Initialise the Error Stack before Accessing Data

    if (tv_server_start != 0) {
        closeIdamError(&server_block.idamerrorstack);    // Free Previous Stack Heap
    }

    initServerBlock(&server_block, 0); // Reset previous Error Messages from the Server & Free Heap
    initIdamErrorStack(&idamerrorstack);

    //-------------------------------------------------------------------------
    // Initialise Protocol Timings (in Debug Mode only)

    time(&protocol_time);

    //------------------------------------------------------------------------------
    // Error Trap: Some Errors are Fatal => Server Destroyed & Connections Closed

    // *** change error management when large RTT involved - why destroy the server if not warranted?

    do {

        newHandle = 0;    // If a New Handle is Issued then Always Return with a Handle

        if (tv_server_start == 0) {
            time(&tv_server_start);    // First Call: Start the Clock
        }

#ifndef FATCLIENT   // <========================== Client Server Code Only

        //-------------------------------------------------------------------------
        // Check the local cache for the data (GET methods only - Note: some GET methods may disguise PUT methods!)

#ifdef MEMCACHE

        static unsigned int cacheStatus = CACHE_NOT_OPENED;

        do {

            // Check Client Properties for permission to cache

            if (clientFlags & CLIENTFLAG_CACHE && !request_block->put && (cacheStatus == CACHE_AVAILABLE || cacheStatus == CACHE_NOT_OPENED)) {

                // Open the Cache

                if (cacheStatus == CACHE_NOT_OPENED) {
                    cache = idamOpenCache();

                    if (cache == NULL) {
                        cacheStatus = CACHE_NOT_AVAILABLE;
                        break;
                    }

                    cacheStatus = CACHE_AVAILABLE;
                }

                // Query the cache for the Data

                data = idamCacheRead(cache, request_block);
		
                if (data != NULL) {	// Success
		   
		   int lastHandle = -1;
	           if((clientFlags & CLIENTFLAG_REUSELASTHANDLE || clientFlags & CLIENTFLAG_FREEREUSELASTHANDLE) && (lastHandle = getIdamThreadLastHandle()) >= 0){
                      if(clientFlags & CLIENTFLAG_FREEREUSELASTHANDLE)
                         idamFree(lastHandle);
                      else
                         initDataBlock(&Data_Block[lastHandle]);		// Application has responsibility for freeing heap in the Data Block
                      Data_Block[lastHandle].handle = lastHandle;
                      memcpy(&Data_Block[lastHandle], data, sizeof(DATA_BLOCK));
                      free(data);
                      return lastHandle;
		      
                   } else {		// re-use or create a new Data Block
		   
		      lastHandle = acc_getIdamNewDataHandle();
		                         
		      memcpy(&Data_Block[lastHandle], data, sizeof(DATA_BLOCK));
                      free(data);
                      return lastHandle;
	           }	   
                }	 	 

            }
        } while (0);

#endif // MEMCACHE


        //-------------------------------------------------------------------------
        // Manage Multiple IDAM Server connections ...
        //
        // Instance a new server on the same Host/Port or on a different Host/port

        if (environment.server_reconnect || environment.server_change_socket) {
            int socketId = -1, clientSocket0 = -1, user_timeout0 = TIMEOUT;
            time_t tv_server_start0;
            XDR* clientInput0, * clientOutput0;

            // Save current client and server timer settings, Socket and XDR handles

            tv_server_start0 = tv_server_start;
            user_timeout0 = user_timeout;
            clientSocket0 = clientSocket;
            clientInput0 = clientInput;
            clientOutput0 = clientOutput;

            // Identify the current Socket connection in the Socket List

            socketId = getSocketRecordId(&client_socketlist, clientSocket);

            // Instance a new server if the Client has changed the host and/or port number

            if (environment.server_reconnect) {
                time(&tv_server_start);         // Start a New Server AGE timer
                clientSocket = -1;              // Flags no Socket is open
                environment.server_change_socket = 0;   // Client doesn't know the Socket ID so disable
            }

            // Client manages connections through the Socket id and specifies which running server to connect to

            if (environment.server_change_socket) {
                if ((socketId = getSocketRecordId(&client_socketlist, environment.server_socket)) < 0) {
                    err = NO_SOCKET_CONNECTION;
                    addIdamError(&idamerrorstack, CODEERRORTYPE, "idamClient", err,
                                 "The User Specified Socket Connection does not exist");
                    break;
                }

                // replace with previous timer settings and XDR handles

                tv_server_start = client_socketlist.sockets[socketId].tv_server_start;
                user_timeout = client_socketlist.sockets[socketId].user_timeout;
                clientSocket = client_socketlist.sockets[socketId].fh;
                clientInput = client_socketlist.sockets[socketId].Input;
                clientOutput = client_socketlist.sockets[socketId].Output;

                environment.server_change_socket = 0;
                environment.server_socket = client_socketlist.sockets[socketId].fh;
                environment.server_port = client_socketlist.sockets[socketId].port;
                strcpy(environment.server_host, client_socketlist.sockets[socketId].host);
            }

            // save Previous data if a previous socket existed

            if (socketId >= 0) {
                client_socketlist.sockets[socketId].tv_server_start = tv_server_start0;
                client_socketlist.sockets[socketId].user_timeout = user_timeout0;
                client_socketlist.sockets[socketId].fh = clientSocket0;
                client_socketlist.sockets[socketId].Input = clientInput0;
                client_socketlist.sockets[socketId].Output = clientOutput0;
            }
        }

        //-------------------------------------------------------------------------
        // Server Age (secs) = Time since the Server was last called

        time(&tv_server_end);
        long age = (long) tv_server_end - (long) tv_server_start;

        IDAM_LOGF(LOG_DEBUG, "Start: %ld    End: %ld\n", (long) tv_server_start, (long) tv_server_end);
        IDAM_LOGF(LOG_DEBUG, "Server Age: %ld\n", (long) age);

        //-------------------------------------------------------------------------
        // Server State: Is the Server Dead? (Age Dependent)

        initServer = 1;

        if (age >= user_timeout - 2) {  // Assume the Server has Self-Destructed so Instanciate a New Server

            IDAM_LOGF(LOG_DEBUG, "idamClient: Server Age Limit Reached %ld\n", (long) age);
            IDAM_LOG(LOG_DEBUG, "idamClient: Server Closed and New Instance Started\n");

            idamClosedown(0);  // Close the Existing Socket and XDR Stream: Reopening will Instance a New Server

        } else {

            if (clientSocket != -1) {          // Assume the Server is Still Alive

                if (clientOutput->x_ops == NULL || clientInput->x_ops == NULL) {
                    addIdamError(&idamerrorstack, CODEERRORTYPE, "idamClient", 999, "XDR Streams are Closed!");

                    IDAM_LOG(LOG_DEBUG, "idamClient: XDR Streams are Closed!\n");

                    idamClosedown(0);
                    initServer = 1;
                } else {

                    initServer = 0;
                    xdrrec_eof(clientInput); // Flush input socket
                }
            }
        }

#ifdef PERFORMANCETEST
        rc = gettimeofday(&(clientPerformance.tv_end[1]), NULL);

        clientPerformance.npoints = 3;
        strcpy(clientPerformance.label[2],"#2: Open Server Socket");
        rc = gettimeofday(&(clientPerformance.tv_start[2]), NULL);
#endif

        //-------------------------------------------------------------------------
        // Open a Socket and Connect to the IDAM Data Server (Multiple Servers?)

        if (initServer) {

            authenticationNeeded = 1;
            startupStates = 0;

            if ((idamCreateConnection()) != 0) {
                err = NO_SOCKET_CONNECTION;
                addIdamError(&idamerrorstack, CODEERRORTYPE, "idamClient", err, "No Socket Connection to Server");
                break;
            }

            time(&tv_server_start);        // Start the Clock again: Age of Server
        }

        //-------------------------------------------------------------------------
        // Create the XDR Record Streams

        if (initServer) {
            idamCreateXDRStream();
        }

#ifdef PERFORMANCETEST
        rc = gettimeofday(&(clientPerformance.tv_end[2]), NULL);

        clientPerformance.npoints = 4;
        strcpy(clientPerformance.label[3],"#3: Initialise Structures");
        rc = gettimeofday(&(clientPerformance.tv_start[3]), NULL);
#endif

        //-------------------------------------------------------------------------

#else       // <========================== End of Client Server Code Only
        initServer = 1;
#endif      // <========================== End of FatClient Code Only

        //-------------------------------------------------------------------------
        // Initialise the Client/Server Structures

        if (initServer && system_startup) {
            char* uid = NULL;

            if ((uid = getlogin()) != NULL) {  // Very Slow!! ... so do once only
                strcpy(clientUsername, uid);
            } else {
                userid(clientUsername);    // Use an even Slower method!
            }

            initClientBlock(&client_block, clientVersion, clientUsername);
            system_startup = 0;            // Don't call again!
        }

        // Update the Private Flags

        char* env = NULL;

        if ((env = getenv("IDAM_PRIVATEFLAGS")) != NULL) {
            setIdamPrivateFlag(atoi(env));
        }

        updateClientBlock(&client_block);     // Allows User to Change Properties at run-time

        // Operating System Name


        if ((env = getenv("OSTYPE")) != NULL) {
            strcpy(client_block.OSName, env);
        }

        // Client's study DOI

        if ((env = getenv("IDAM_CLIENT_DOI")) != NULL) {
            strcpy(client_block.DOI, env);
        }

        printClientBlock(client_block);

#ifdef PERFORMANCETEST
        rc = gettimeofday(&(clientPerformance.tv_end[3]), NULL);

        clientPerformance.npoints = 5;
        strcpy(clientPerformance.label[4],"#4: Establish Communication with Server");
        rc = gettimeofday(&(clientPerformance.tv_start[4]), NULL);
#endif

        //------------------------------------------------------------------------------
        // User Authentication at startup

#ifndef FATCLIENT   // <========================== Client Server Code Only

#ifdef SECURITYENABLED

        // Client/Server connection is established but neither the server nor the user identity has been authenticated.
        // Client initiates the process by sending an encrypted token with its claim of identity.
        // The multi-step dialogue between client and server tests the identity claims of both.
        // Authentication occurs at server startup only.
        // Once passed, the original data request is processed.

        // TBD:
        // Additional tests of identity will be included in consequent client/server dialogue to maintain authenticated state.
        // If the connection breaks and the client has to start a new connection to the same host, the last good exchanged token
        // (within a very short time window) can be used to byepass the multi-RTT cost of authentication.
        // Use asynchronous encrypted caching to manage these short-lived session tokens. Use a key based on originating IP address and claimed identity.

        // When a server is a regular server and connects to another server as a data source, then the originating server has
        // to authenticate as if a client. The originating client has also to authenticate with the final server.

        // When a server is acting as a simple proxy it passes all requests onwards without authentication - it does not
        // interpret the data access request.

        initSecurityBlock(&client_block.securityBlock);
        initSecurityBlock(&server_block.securityBlock);

        if (authenticationNeeded) {

            // generate token A and encrypt with the server public key
            // send Client_block: the client certificate and encrypted token A

            unsigned short authenticationStep = 1;     // Client Certificate authenticated by server

            if ((err = idamClientAuthentication(&client_block, &server_block, authenticationStep)) != 0) {
                addIdamError(&idamerrorstack, CODEERRORTYPE, "idamClient", err, "Client or Server Authentication Failed #1");
                break;
            }

            // receive server_block
            // decrypt tokens A, B using private key
            // Test token A is identical to that sent in step 1 => test server has a valid private key

            authenticationStep = 5;

            if ((err = idamClientAuthentication(&client_block, &server_block, authenticationStep)) != 0) {
                addIdamError(&idamerrorstack, CODEERRORTYPE, "idamClient", err, "Client or Server Authentication Failed #5");
                break;
            }

            // encrypt token B with the server public key       => send proof client has a valid private key (paired with public key from certificate)
            // send client block

            authenticationStep = 6;

            if ((err = idamClientAuthentication(&client_block, &server_block, authenticationStep)) != 0) {
                addIdamError(&idamerrorstack, CODEERRORTYPE, "idamClient", err, "Client or Server Authentication Failed #6");
                break;
            }

            // receive server_block
            // decrypt new token B using private key        => proof server has a valid private key == server authenticated
            // encrypt new token B with the server public key   => maintain authentication

            authenticationStep = 8;

            if ((err = idamClientAuthentication(&client_block, &server_block, authenticationStep)) != 0) {
                addIdamError(&idamerrorstack, CODEERRORTYPE, "idamClient", err, "Client or Server Authentication Failed #8");
                break;
            }

            authenticationNeeded = 0;  // Both Client and Server have been mutually authenticated
        }

#else

        //-------------------------------------------------------------------------
        // Client and Server States at Startup only (1 RTT)
        // Will be passed during mutual authentication step

        if (!startupStates) {

            // Flush (mark as at EOF) the input socket buffer (before the exchange begins)

            int protocol_id = PROTOCOL_CLIENT_BLOCK;      // Send Client Block (proxy for authenticationStep = 6)

            if ((err = protocol2(clientOutput, protocol_id, XDR_SEND, NULL, &client_block)) != 0) {
                addIdamError(&idamerrorstack, CODEERRORTYPE, "idamClient", err, "Protocol 10 Error (Client Block)");

                IDAM_LOG(LOG_DEBUG, "idamClient: Error Sending Client Block\n");

                break;
            }

            if (!(xdrrec_endofrecord(clientOutput, 1))) { // Send data now
                err = PROTOCOL_ERROR_7;
                addIdamError(&idamerrorstack, CODEERRORTYPE, "idamClient", err, "Protocol 7 Error (Client Block)");

                IDAM_LOG(LOG_DEBUG, "idamClient: Error xdrrec_endofrecord after Client Block\n");

                break;
            }

            // Flush (mark as at EOF) the input socket buffer (start of wait for data)

            if (!(xdrrec_skiprecord(
                    clientInput))) { // Wait for data, then position buffer reader to the start of a new record
                err = PROTOCOL_ERROR_5;
                addIdamError(&idamerrorstack, CODEERRORTYPE, "idamClient", err, "Protocol 5 Error (Server Block)");

                IDAM_LOG(LOG_DEBUG, "idamClient: Error xdrrec_skiprecord prior to Server Block\n");

                break;
            }

            protocol_id = PROTOCOL_SERVER_BLOCK;      // Receive Server Block: Server Aknowledgement (proxy for authenticationStep = 8)

            if ((err = protocol2(clientInput, protocol_id, XDR_RECEIVE, NULL, &server_block)) != 0) {
                addIdamError(&idamerrorstack, CODEERRORTYPE, "idamClient", err, "Protocol 11 Error (Server Block #1)");
                // Assuming the server_block is corrupted, replace with a clean copy to avoid concatonation problems
                server_block.idamerrorstack.nerrors = 0;

                IDAM_LOG(LOG_DEBUG, "idamClient: Error receiving Server Block\n");

                break;
            }

            // Flush (mark as at EOF) the input socket buffer (not all server state data may have been read - version dependent)

            int rc = xdrrec_eof(clientInput);

            IDAM_LOG(LOG_DEBUG, "idamClient: Server Block Received\n");
            IDAM_LOGF(LOG_DEBUG, "idamClient: xdrrec_eof rc = %d [1 => no more input]\n", rc);
            printServerBlock(server_block);

            // Protocol Version: Lower of the client and server version numbers
            // This defines the set of elements within data structures passed between client and server
            // Must be the same on both sides of the socket

            //protocolVersion = clientVersion;
            if (client_block.version < server_block.version) {
                protocolVersion = client_block.version;
            }


#ifdef ERRORSTACK

            if (server_block.idamerrorstack.nerrors > 0) {
                err = server_block.idamerrorstack.idamerror[0].code;      // Problem on the Server Side!
                break;
            }

#else

            if (server_block.error > 0) {
                addIdamError(&idamerrorstack, CODEERRORTYPE, "idamClient", server_block.error, server_block.msg);
                break;
            }

#endif

#ifdef PERFORMANCETEST
            gettimeofday(&(clientPerformance.tv_end[4]), NULL);

            clientPerformance.npoints = 6;
            strcpy(clientPerformance.label[5],"#5: Send Request and wait for server status");
            gettimeofday(&(clientPerformance.tv_start[5]), NULL);
#endif

            startupStates = 1;

        } // startupStates

#endif  // not SECURITYENABLED


        //-------------------------------------------------------------------------
        // Check the Server version is not older than this client's version

        IDAM_LOGF(LOG_DEBUG, "idamClient: protocolVersion %d\n", protocolVersion);
        IDAM_LOGF(LOG_DEBUG, "idamClient: Client Version  %d\n", client_block.version);
        IDAM_LOGF(LOG_DEBUG, "idamClient: Server Version  %d\n", server_block.version);

        /*
           if(protocolVersion < 7 || server_block.version < 7){
              err = 999;
              addIdamError(&idamerrorstack, CODEERRORTYPE, "idamClient", err,
              "The connected Server has an Incompatible Protocol. It's too old! Please connect to a version 7 server.");
              break;
           }
        */


        //-------------------------------------------------------------------------
        // Flush to EOF the input buffer (start of wait for new data) necessary when Zero data waiting but not an EOF!

        int rc;
        if (!(rc = xdrrec_eof(clientInput))) { // Test for an EOF

            IDAM_LOGF(LOG_DEBUG, "idamClient: xdrrec_eof rc = %d => more input when none expected!\n", rc);

            int count = 0;
            char temp;

            do {
                rc = xdr_char(clientInput, &temp);                  // Flush the input (limit to 64 bytes)

                if (rc) {
                    IDAM_LOGF(LOG_DEBUG, "[%d] [%c]\n", count++, temp);
                }
            } while (rc && count < 64);

            if (count > 0) {   // Error if data is waiting
                err = 999;
                addIdamError(&idamerrorstack, CODEERRORTYPE, "idamClient", err,
                             "Data waiting in the input data buffer when none expected! Please contact the system administrator.");

                IDAM_LOGF(LOG_DEBUG, "[%d] excess data bytes waiting in input buffer!\n", count++);

                break;
            }

            rc = xdrrec_eof(clientInput);          // Test for an EOF

            if (!rc) {
                rc = xdrrec_skiprecord(clientInput);    // Flush the input buffer (Zero data waiting but not an EOF!)
            }

            rc = xdrrec_eof(clientInput);          // Test for an EOF

            if (!rc) {
                err = 999;
                addIdamError(&idamerrorstack, CODEERRORTYPE, "idamClient", err,
                             "Corrupted input data stream! Please contact the system administrator.");

                IDAM_LOG(LOG_DEBUG, "Unable to flush input buffer!!!\n");

                break;
            }

            IDAM_LOG(LOG_DEBUG, "idamClient: xdrrec_eof rc = 1 => no more input, buffer flushed.\n");

        }

        //-------------------------------------------------------------------------
        // Send the Client Block

        int protocol_id = PROTOCOL_CLIENT_BLOCK;      // Send Client Block

        if ((err = protocol2(clientOutput, protocol_id, XDR_SEND, NULL, &client_block)) != 0) {
            addIdamError(&idamerrorstack, CODEERRORTYPE, "idamClient", err, "Protocol 10 Error (Client Block)");
            break;
        }


        //-------------------------------------------------------------------------
        // Send the Client Request

        protocol_id = PROTOCOL_REQUEST_BLOCK;     // This is what the Client Wants

        if ((err = protocol2(clientOutput, protocol_id, XDR_SEND, NULL, request_block)) != 0) {
            addIdamError(&idamerrorstack, CODEERRORTYPE, "idamClient", err, "Protocol 1 Error (Request Block)");
            break;
        }

        //------------------------------------------------------------------------------
        // Pass the PUTDATA structure

        if (request_block->put) {
            protocol_id = PROTOCOL_PUTDATA_BLOCK_LIST;

            if ((err = protocol2(clientOutput, protocol_id, XDR_SEND, NULL, &(request_block->putDataBlockList))) != 0) {
                addIdamError(&idamerrorstack, CODEERRORTYPE, "idamClient", err,
                             "Protocol 1 Error (sending putDataBlockList from Request Block)");
                break;
            }
        }

        //------------------------------------------------------------------------------
        // Send the Full TCP packet and wait for the returned data

        if (!(rc = xdrrec_endofrecord(clientOutput, 1))) {
            err = PROTOCOL_ERROR_7;
            addIdamError(&idamerrorstack, CODEERRORTYPE, "idamClient", err,
                         "Protocol 7 Error (Request Block & putDataBlockList)");
            break;
        }

        IDAM_LOG(LOG_DEBUG, "idamClient: ****** Outgoing tcp packet sent without error. Waiting for data.\n");

        if (!(rc = xdrrec_skiprecord(clientInput))) {
            err = PROTOCOL_ERROR_5;
            addIdamError(&idamerrorstack, CODEERRORTYPE, "idamClient", err,
                         " Protocol 5 Error (Server & Data Structures)");
            break;
        }

        IDAM_LOG(LOG_DEBUG, "idamClient: ****** Incoming tcp packet received without error. Reading...\n");

        //------------------------------------------------------------------------------
        // Receive the Server State/Aknowledgement that the Data has been Accessed
        // Just in case the Server has crashed!

        IDAM_LOG(LOG_DEBUG, "idamClient: Waiting for Server Status Block\n");

        protocol_id = PROTOCOL_SERVER_BLOCK;      // Receive Server Block: Server Aknowledgement

        if ((err = protocol2(clientInput, protocol_id, XDR_RECEIVE, NULL, &server_block)) != 0) {
            IDAM_LOGF(LOG_DEBUG, "idamClient: Protocol 11 Error (Server Block #2) = %d\n", err);

            addIdamError(&idamerrorstack, CODEERRORTYPE, "idamClient", err, " Protocol 11 Error (Server Block #2)");
            // Assuming the server_block is corrupted, replace with a clean copy to avoid future concatonation problems
            server_block.idamerrorstack.nerrors = 0;
            break;
        }

        IDAM_LOG(LOG_DEBUG, "idamClient: Server Block Received\n");
        printServerBlock(server_block);

        serverside = 0;

#ifdef ERRORSTACK

        if (server_block.idamerrorstack.nerrors > 0) {
            IDAM_LOGF(LOG_DEBUG, "idamClient: Server Block passed Server Error State %d\n", err);

            err = server_block.idamerrorstack.idamerror[0].code;      // Problem on the Server Side!

            IDAM_LOGF(LOG_DEBUG, "idamClient: Server Block passed Server Error State %d\n", err);

            serverside = 1;        // Most Server Side errors are benign so don't close the server
            break;
        }

#else

        if (server_block.error > 0) {
            addIdamError(&idamerrorstack, CODEERRORTYPE, "idamClient", server_block.error, server_block.msg);
            err        = server_block.error;
            serverside = 1;
            break;
        }

#endif

#ifdef PERFORMANCETEST
        rc = gettimeofday(&(clientPerformance.tv_end[5]), NULL);

        clientPerformance.npoints = 7;
        strcpy(clientPerformance.label[6],"#6: Receive Meta Data");
        rc = gettimeofday(&(clientPerformance.tv_start[6]), NULL);
#endif

#endif  // not FATCLIENT            // <===== End of Client Server Only Code

        //------------------------------------------------------------------------------
        // Return Database Meta Data if User Requests it

        allocMetaHeap = 0;

        if (client_block.get_meta && !request_block->put) {

#ifndef NOTGENERICENABLED

            // Allocate memory for the Meta Data

            data_system = (DATA_SYSTEM*) malloc(sizeof(DATA_SYSTEM));
            system_config = (SYSTEM_CONFIG*) malloc(sizeof(SYSTEM_CONFIG));
            data_source = (DATA_SOURCE*) malloc(sizeof(DATA_SOURCE));
            signal_rec = (SIGNAL*) malloc(sizeof(SIGNAL));
            signal_desc = (SIGNAL_DESC*) malloc(sizeof(SIGNAL_DESC));

            if (data_system == NULL || system_config == NULL || data_source == NULL ||
                signal_rec == NULL || signal_desc == NULL) {
                err = ERROR_ALLOCATING_META_DATA_HEAP;
                addIdamError(&idamerrorstack, CODEERRORTYPE, "idamClient", err, "Error Allocating Heap for Meta Data");
                break;
            }

            allocMetaHeap = 1;      // Manage Heap if an Error Occurs

#endif

            //------------------------------------------------------------------------------
            // Receive the Data System Record

#ifndef FATCLIENT   // <========================== Client Server Code Only

            protocol_id = PROTOCOL_DATA_SYSTEM;

            if ((err = protocol2(clientInput, protocol_id, XDR_RECEIVE, NULL, data_system)) != 0) {
                addIdamError(&idamerrorstack, CODEERRORTYPE, "idamClient", err, "Protocol 4 Error (Data System)");
                break;
            }

            printDataSystem(*data_system);

            //------------------------------------------------------------------------------
            // Receive the System Configuration Record

            protocol_id = PROTOCOL_SYSTEM_CONFIG;

            if ((err = protocol2(clientInput, protocol_id, XDR_RECEIVE, NULL, system_config)) != 0) {
                addIdamError(&idamerrorstack, CODEERRORTYPE, "idamClient", err, "Protocol 5 Error (System Config)");
                break;
            }

            printSystemConfig(*system_config);

            //------------------------------------------------------------------------------
            // Receive the Data Source Record

            protocol_id = PROTOCOL_DATA_SOURCE;

            if ((err = protocol2(clientInput, protocol_id, XDR_RECEIVE, NULL, data_source)) != 0) {
                addIdamError(&idamerrorstack, CODEERRORTYPE, "idamClient", err, "Protocol 6 Error (Data Source)");
                break;
            }

            printDataSource(*data_source);

            //------------------------------------------------------------------------------
            // Receive the Signal Record

            protocol_id = PROTOCOL_SIGNAL;

            if ((err = protocol2(clientInput, protocol_id, XDR_RECEIVE, NULL, signal_rec)) != 0) {
                addIdamError(&idamerrorstack, CODEERRORTYPE, "idamClient", err, "Protocol 7 Error (Signal)");
                break;
            }

            printSignal(*signal_rec);

            //------------------------------------------------------------------------------
            // Receive the Signal Description Record

            protocol_id = PROTOCOL_SIGNAL_DESC;

            if ((err = protocol2(clientInput, protocol_id, XDR_RECEIVE, NULL, signal_desc)) != 0) {
                addIdamError(&idamerrorstack, CODEERRORTYPE, "idamClient", err, "Protocol 8 Error (Signal Desc)");
                break;
            }

            IDAM_LOG(LOG_DEBUG, "idamClient: Signal Desc Block Received\n");
            printSignalDesc(*signal_desc);

        }  // End of Client/Server Protocol Management

#ifdef PERFORMANCETEST
        rc = gettimeofday(&(clientPerformance.tv_end[6]), NULL);

        clientPerformance.npoints = 8;
        strcpy(clientPerformance.label[7],"#7: Receive Data");
        rc = gettimeofday(&(clientPerformance.tv_start[7]), NULL);
#endif

#else       // <========================== End of Client Server Code Only (not FATCLIENT)
        }

#endif      // <========================== End of Fat Client Code Only

        //------------------------------------------------------------------------------
        // Allocate memory for the Data Block Structure 
	// Re-use existing stale Data Blocks 

        
	data_block_idx = acc_getIdamNewDataHandle();
	
	if(data_block_idx < 0){			// Error 
	   err = -data_block_idx;
	   break;		
	}   
	
        newHandle = 1;        			// Flags Heap has been allocated	
        data_block = getIdamDataBlock(data_block_idx);

        //------------------------------------------------------------------------------
        // Copy the client server property values via the client_block

        copyClientBlock(&(data_block->client_block));

        //------------------------------------------------------------------------------
        // Assign Meta Data to Data Block

        //#ifdef GENERIC_ENABLE
#ifndef NOTGENERICENABLED

        if (client_block.get_meta && allocMetaHeap) {
            data_block->data_system = data_system;
            data_block->system_config = system_config;
            data_block->data_source = data_source;
            data_block->signal_rec = signal_rec;
            data_block->signal_desc = signal_desc;
        }

#endif

#ifndef FATCLIENT   // <========================== Client Server Code Only

        //------------------------------------------------------------------------------
        // Fetch the data Block

        protocol_id = PROTOCOL_DATA_BLOCK;

        if ((err = protocol2(clientInput, protocol_id, XDR_RECEIVE, NULL, data_block)) != 0) {
            IDAM_LOG(LOG_DEBUG, "idamClient: Protocol 2 Error (Failure Receiving Data Block)\n");

            addIdamError(&idamerrorstack, CODEERRORTYPE, "idamClient", err,
                         "Protocol 2 Error (Failure Receiving Data Block)");
            break;
        }

        // Flush (mark as at EOF) the input socket buffer (All regular Data has been received)

        printDataBlock(*data_block);

#ifdef PERFORMANCETEST
        rc = gettimeofday(&(clientPerformance.tv_end[7]), NULL);

        clientPerformance.npoints = 9;
        strcpy(clientPerformance.label[8],"#8: Receive Hierarchical Data");
        rc = gettimeofday(&(clientPerformance.tv_start[8]), NULL);
#endif

        //------------------------------------------------------------------------------
        // Fetch Hierarchical Data Structures

        // manages it's own client/server conversation
        // current buffer status

        // in protocolXML2 ...
        // xdrrec_skiprecord        wait for new record
        //                              xdr_int(xdrs, &packageType)
        //                              xdrrec_endofrecord
        // xdr_int(xdrs, &packageType)

        if (data_block->data_type == TYPE_COMPOUND &&
            data_block->opaque_type != OPAQUE_TYPE_UNKNOWN) {

            if (data_block->opaque_type == OPAQUE_TYPE_XML_DOCUMENT) {
                protocol_id = PROTOCOL_META;
            } else if (data_block->opaque_type == OPAQUE_TYPE_STRUCTURES ||
                       data_block->opaque_type == OPAQUE_TYPE_XDRFILE ||
                       data_block->opaque_type == OPAQUE_TYPE_XDROBJECT) {
                protocol_id = PROTOCOL_STRUCTURES;
            } else {
                protocol_id = PROTOCOL_EFIT;
            }

            IDAM_LOG(LOG_DEBUG, "idamClient: Receiving Hierarchical Data Structure from Server\n");

            if ((err = protocol2(clientInput, protocol_id, XDR_RECEIVE, NULL, data_block)) !=
                0) {
                addIdamError(&idamerrorstack, CODEERRORTYPE, "idamClient", err,
                             "Client Side Protocol Error (Opaque Structure Type)");
                break;
            }

//#ifdef HIERARCHICAL_DATA
//            if (Data_Block[acc_getCurrentDataBlockIndex()].opaque_type != OPAQUE_TYPE_EFIT) {
//                printEFIT(*(EFIT *)Data_Block[acc_getCurrentDataBlockIndex()].opaque_block);
//            }
//#endif
        }

        IDAM_LOG(LOG_DEBUG, "idamClient: Hierarchical Structure Block Received\n");

#ifdef PERFORMANCETEST
        rc = gettimeofday(&(clientPerformance.tv_end[8]), NULL);

        clientPerformance.npoints = 10;
        strcpy(clientPerformance.label[9],"#9: Send Final Protocol");
        rc = gettimeofday(&(clientPerformance.tv_start[9]), NULL);
#endif

#else       // <========================== End of Client Server Code Only (not FATCLIENT)

        //------------------------------------------------------------------------------
        // Fat Client Server

        DATA_BLOCK data_block0;
        err = idamServer(client_block, request_block, &server_block, &data_block0);

        data_block = getIdamDataBlock(data_block_idx); // data blocks may have been realloc'ed
        *data_block = data_block0;

        if (err != 0 || server_block.idamerrorstack.nerrors > 0) {
            if (err == 0) {
                err = SERVER_BLOCK_ERROR;
            }

            IDAM_LOGF(LOG_DEBUG, "idamClient: Error Returned from Data Server %d\n", err);

            addIdamError(&idamerrorstack, CODEERRORTYPE, "idamClient", err, "Error Returned from Data Server");
            break;
        }

        // Decompresss Dimensional Data

        int i;

        for (i = 0; i < data_block->rank; i++) {       // Expand Compressed Regular Vector
            err = uncompressDim(&(data_block->dims[i]));    // Allocate Heap as required
            err = 0;                               // Need to Test for Error Condition!
        }

        printDataBlock(*data_block);

#endif      // <========================== End of FatClient Code Only

        //------------------------------------------------------------------------------
        // Cache the data if the server has passed permission and the application (client) has enabled caching

#ifdef MEMCACHE
#ifdef CACHEDEV

        if (cacheStatus == CACHE_AVAILABLE && clientFlags & CLIENTFLAG_CACHE && Data_Block[acc_getCurrentDataBlockIndex()].cachePermission == PLUGINOKTOCACHE) {
#else

        if (cacheStatus == CACHE_AVAILABLE && clientFlags & CLIENTFLAG_CACHE) {
#endif
            idamCacheWrite(cache, request_block, data_block);
        }

#endif

        //------------------------------------------------------------------------------
        // End of Error Trap Loop

    } while (0);


    // 4 Possible Error States:
    //
    //  err == 0; newHandle = 0;   Cannot Happen: Close Server & Return an Error!
    //  err == 0; newHandle = 1;   Sleep Server & Return Handle
    //  err != 0; newHandle = 0;   Close Server (unless it has occured server side) & Return -err
    //  err != 0; newHandle = 1;   Close Server (unless it has occured server side) & Return Handle (Contains Error)

    IDAM_LOGF(LOG_DEBUG, "idamClient: Error Code at end of Error Trap: %d\n", err);
    IDAM_LOGF(LOG_DEBUG, "idamClient: newHandle                      : %d\n", newHandle);
    IDAM_LOGF(LOG_DEBUG, "idamClient: serverside                     : %d\n", serverside);
    IDAM_LOGF(LOG_DEBUG, "idamClient: clientSocket                   : %d\n", clientSocket);

    //------------------------------------------------------------------------------
    // Server Sleeps: If error then assume Server has Closed Down

#ifndef FATCLIENT   // <========================== Client Server Code Only

    if (clientSocket >= 0) {

        int next_protocol = PROTOCOL_CLOSEDOWN;       // Closedown (Die) is the default

        if ((err == 0 && newHandle) || serverside) {
            next_protocol = PROTOCOL_SLEEP;       // Sleep
            time(&tv_server_start);           // Restart the Server Age Clock
        }

        IDAM_LOGF(LOG_DEBUG, "idamClient: Sending Next Protocol %d\n", next_protocol);
    }

#ifdef PERFORMANCETEST
    rc = gettimeofday(&(clientPerformance.tv_end[9]), NULL);

    clientPerformance.npoints = 11;
    strcpy(clientPerformance.label[10],"#10: House keeping");
    rc = gettimeofday(&(clientPerformance.tv_start[10]), NULL);
#endif

    //------------------------------------------------------------------------------
    // Close all File Handles, Streams, sockets and Free Heap Memory

    //rc = fflush(NULL); // save anything ... the user might not follow correct procedure!

    if (newHandle) {
        IDAM_LOGF(LOG_DEBUG, "idamClient: Handle %d\n", data_block_idx);

        if (err != 0 && !serverside) {
            idamClosedown(0);    // Close Socket & XDR Streams but Not Files
        }

        if (err == 0 && (getIdamDataStatus(data_block_idx)) == MIN_STATUS && !get_bad) {
            // If Data are not usable, flag the client
            addIdamError(&idamerrorstack, CODEERRORTYPE, "idamClient", DATA_STATUS_BAD,
                         "Data Status is BAD ... Data are Not Usable!");

            if (data_block->errcode == 0) {
                // Don't over-rule a server side error
                data_block->errcode = DATA_STATUS_BAD;
                strcpy(data_block->error_msg, "Data Status is BAD ... Data are Not Usable!");
            }
        }

        //------------------------------------------------------------------------------
        // Concatenate Error Message Stacks & Write to the Error Log

        concatIdamError(idamerrorstack, &server_block.idamerrorstack);
        closeIdamError(&idamerrorstack);
        idamErrorLog(client_block, *request_block, server_block.idamerrorstack);

        //------------------------------------------------------------------------------
        // Copy Most Significant Error Stack Message to the Data Block if a Handle was Issued

        if (data_block->errcode == 0 && server_block.idamerrorstack.nerrors > 0) {
            data_block->errcode = getIdamServerErrorStackRecordCode(0);
            strcpy(data_block->error_msg, getIdamServerErrorStackRecordMsg(0));
        }

        //------------------------------------------------------------------------------
        // Normal Exit: Return to Client

#ifdef PERFORMANCETEST
        rc = gettimeofday(&clientPerformance.tv_end[0], NULL);
        rc = gettimeofday(&clientPerformance.tv_end[10], NULL);

        printPerformance(clientPerformance);
#endif

        return data_block_idx;

        //------------------------------------------------------------------------------
        // Abnormal Exit: Return to Client

    } else {

        if (allocMetaHeap) {
            if (data_system != NULL) {
                free((void*) data_system);    // Free Unwanted Meta Data
            }

            if (system_config != NULL) {
                free((void*) system_config);
            }

            if (data_source != NULL) {
                free((void*) data_source);
            }

            if (signal_rec != NULL) {
                free((void*) signal_rec);
            }

            if (signal_desc != NULL) {
                free((void*) signal_desc);
            }

            data_system = NULL;
            system_config = NULL;
            data_source = NULL;
            signal_rec = NULL;
            signal_desc = NULL;
        }

        IDAM_LOGF(LOG_DEBUG, "idamClient: Returning Error %d\n", err);

        if (err != 0 && !serverside) {
            idamClosedown(0);
        }

        concatIdamError(idamerrorstack, &server_block.idamerrorstack);
        closeIdamError(&idamerrorstack);
        idamErrorLog(client_block, *request_block, server_block.idamerrorstack);


#ifdef PERFORMANCETEST
        rc = gettimeofday(&clientPerformance.tv_end[0], NULL);
        rc = gettimeofday(&clientPerformance.tv_end[10], NULL);

        printPerformance(clientPerformance);
#endif

        if (err == 0) {
            return (ERROR_CONDITION_UNKNOWN);
        }

        return -abs(err);                       // Abnormal Exit
    }
}

#else       // <========================== End of Client Server Code Only (not FATCLIENT)

//------------------------------------------------------------------------------
// If an error has occured: Close all File Handles, Streams, sockets and Free Heap Memory

//rc = fflush(NULL); // save anything ... the user might not follow correct procedure!

    if (newHandle) {
        IDAM_LOGF(LOG_DEBUG, "idamClient: Handle %d\n", data_block_idx);

        if (err != 0) {
            idamClosedown(0);
        }

        if (err == 0 && (getIdamDataStatus(data_block_idx)) == MIN_STATUS &&
            !get_bad) { // If Data are not usable, flag the client
            addIdamError(&idamerrorstack, CODEERRORTYPE, "idamClient", DATA_STATUS_BAD,
                         "Data Status is BAD ... Data are Not Usable!");

            if (data_block->errcode ==
                0) {                     // Don't over-rule a server side error
                data_block->errcode = DATA_STATUS_BAD;
                strcpy(data_block->error_msg, "Data Status is BAD ... Data are Not Usable!");
            }
        }

        //------------------------------------------------------------------------------
        // Concatenate Error Message Stacks & Write to the Error Log

        //server_block.idamerrorstack = idamerrorstack;          // Keep Updated: Heap is the same if FatClient
        concatIdamError(idamerrorstack, &server_block.idamerrorstack);
        closeIdamError(&idamerrorstack);

        idamErrorLog(client_block, *request_block, server_block.idamerrorstack);

        //------------------------------------------------------------------------------
        // Copy Most Significant Error Stack Message to the Data Block if a Handle was Issued

        if (data_block->errcode == 0 && server_block.idamerrorstack.nerrors > 0) {
            data_block->errcode = getIdamServerErrorStackRecordCode(0);
            strcpy(data_block->error_msg, getIdamServerErrorStackRecordMsg(0));
        }

        //------------------------------------------------------------------------------
        // Normal Exit: Return to Client

        return data_block_idx;

        //------------------------------------------------------------------------------
        // Abnormal Exit: Return to Client

    } else {

        //#ifdef GENERIC_ENABLE
#ifndef NOTGENERICENABLED
        if (allocMetaHeap) {
            if (data_system != NULL) {
                free((void *) data_system);    // Free Unwanted Meta Data
            }

            if (system_config != NULL) {
                free((void *) system_config);
            }

            if (data_source != NULL) {
                free((void *) data_source);
            }

            if (signal_rec != NULL) {
                free((void *) signal_rec);
            }

            if (signal_desc != NULL) {
                free((void *) signal_desc);
            }

            data_system = NULL;
            system_config = NULL;
            data_source = NULL;
            signal_rec = NULL;
            signal_desc = NULL;
        }

#endif

        IDAM_LOGF(LOG_DEBUG, "idamClient: Returning Error %d\n", err);

        if (err != 0) {
            idamClosedown(0);
        }


        server_block.idamerrorstack = idamerrorstack;
        idamErrorLog(client_block, *request_block, server_block.idamerrorstack);

        if (err == 0) {
            return (ERROR_CONDITION_UNKNOWN);
        }

        return (-abs(err));                       // Abnormal Exit
    }
}

#endif      // <========================== End of FatClient Code Only


void idamFree(int handle)
{

    // Free Heap Memory (Not the Data Blocks themselves: These will be re-used.)

    char* cptr;
    DIMS* ddims;
    int i, rank;

    DATA_BLOCK* data_block = getIdamDataBlock(handle);

    if (data_block == NULL)  return;

        // Free Hierarchical structured data first

        switch (data_block->opaque_type) {
            case (OPAQUE_TYPE_XML_DOCUMENT): {
                if (data_block->opaque_block != NULL) {
                    free((void*) data_block->opaque_block);
                }

                data_block->opaque_count = 0;
                data_block->opaque_block = NULL;
                data_block->opaque_type = OPAQUE_TYPE_UNKNOWN;
                break;
            }

            case (OPAQUE_TYPE_STRUCTURES): {
#ifdef GENERALSTRUCTS

                if (data_block->opaque_block != NULL) {
                    GENERAL_BLOCK* general_block = (GENERAL_BLOCK*) data_block->opaque_block;

                    if (general_block->userdefinedtypelist != NULL) {
                        if (userdefinedtypelist ==
                            general_block->userdefinedtypelist) {  // Is this the current setting?
                            freeUserDefinedTypeList(userdefinedtypelist);
                            free((void*) userdefinedtypelist);
                            userdefinedtypelist = NULL;
                        } else {
                            freeUserDefinedTypeList(general_block->userdefinedtypelist);
                            free((void*) general_block->userdefinedtypelist);
                        }
                    }

                    if (general_block->logmalloclist != NULL) {
                        if (logmalloclist == general_block->logmalloclist) {
                            freeMallocLogList(logmalloclist);
                            free((void*) logmalloclist);
                            logmalloclist = NULL;
                        } else {
                            freeMallocLogList(general_block->logmalloclist);
                            free((void*) general_block->logmalloclist);
                        }
                    }

                    if (general_block->userdefinedtype != NULL) {
                        freeUserDefinedType(general_block->userdefinedtype);
                        free((void*) general_block->userdefinedtype);
                    }

                    free((void*) general_block);
                }

#endif
                data_block->opaque_block = NULL;
                data_block->data_type = TYPE_UNKNOWN;
                data_block->opaque_count = 0;
                data_block->opaque_type = OPAQUE_TYPE_UNKNOWN;
                data_block->data = NULL;

                break;
            }

            case (OPAQUE_TYPE_XDRFILE): {
#ifdef GENERALSTRUCTS

                if (data_block->opaque_block != NULL) {
                    free(data_block->opaque_block);
                }

#endif
                data_block->opaque_block = NULL;
                data_block->data_type = TYPE_UNKNOWN;
                data_block->opaque_count = 0;
                data_block->opaque_type = OPAQUE_TYPE_UNKNOWN;
                data_block->data = NULL;

                break;
            }

            case (OPAQUE_TYPE_XDROBJECT): {
#ifdef GENERALSTRUCTS

                if (data_block->opaque_block != NULL) {
                    free(data_block->opaque_block);
                }

#endif
                data_block->opaque_block = NULL;
                data_block->data_type = TYPE_UNKNOWN;
                data_block->opaque_count = 0;
                data_block->opaque_type = OPAQUE_TYPE_UNKNOWN;
                data_block->data = NULL;

                break;
            }

            default:
                break;
        }

        rank = data_block->rank;
        ddims = data_block->dims;

        if ((cptr = data_block->data) != NULL) {
            free((void*) cptr);
            data_block->data = NULL;    // Prevent another Free
        }

        if ((cptr = data_block->errhi) != NULL) {
            free((void*) cptr);
            data_block->errhi = NULL;
        }

        if ((cptr = data_block->errlo) != NULL) {
            free((void*) cptr);
            data_block->errlo = NULL;
        }

        if ((cptr = data_block->synthetic) != NULL) {
            free((void*) cptr);
            data_block->synthetic = NULL;
        }

        if (data_block->data_system != NULL) {
            free((void*) data_block->data_system);
            data_block->data_system = NULL;
        }

        if (data_block->system_config != NULL) {
            free((void*) data_block->system_config);
            data_block->system_config = NULL;
        }

        if (data_block->data_source != NULL) {
            free((void*) data_block->data_source);
            data_block->data_source = NULL;
        }

        if (data_block->signal_rec != NULL) {
            free((void*) data_block->signal_rec);
            data_block->signal_rec = NULL;
        }

        if (data_block->signal_desc != NULL) {
            free((void*) data_block->signal_desc);
            data_block->signal_desc = NULL;
        }

        if (ddims != NULL && rank > 0) {
            for (i = 0; i < rank; i++) {
                if ((cptr = data_block->dims[i].dim) != NULL) {
                    free((void*) cptr);
                }

                if ((cptr = data_block->dims[i].synthetic) != NULL) {
                    free((void*) cptr);
                }

                if ((cptr = data_block->dims[i].errhi) != NULL) {
                    free((void*) cptr);
                }

                if ((cptr = data_block->dims[i].errlo) != NULL) {
                    free((void*) cptr);
                }

                data_block->dims[i].dim = NULL;    // Prevent another Free
                data_block->dims[i].synthetic = NULL;
                data_block->dims[i].errhi = NULL;
                data_block->dims[i].errlo = NULL;

                if ((cptr = (char*) data_block->dims[i].sams) != NULL) {
                    free((void*) cptr);
                }

                if ((cptr = data_block->dims[i].offs) != NULL) {
                    free((void*) cptr);
                }

                if ((cptr = data_block->dims[i].ints) != NULL) {
                    free((void*) cptr);
                }

                data_block->dims[i].sams = NULL;
                data_block->dims[i].offs = NULL;
                data_block->dims[i].ints = NULL;
            }

            free((void*) ddims);
            data_block->dims = NULL;    // Prevent another Free
        }

    // closeIdamError(&server_block.idamerrorstack);
    
    initDataBlock(data_block);
    data_block->handle = -1;		// Flag this as ready for re-use 
}

void idamFreeAll()
{
    // Free All Heap Memory
    int i;

#ifndef FATCLIENT
    int protocol_id;
#endif

#ifdef MEMCACHE
    // Free Cache connection object
    idamFreeCache();
#endif

    for (i = 0; i < acc_getCurrentDataBlockIndex(); ++i) {
        freeDataBlock(getIdamDataBlock(i));
    }

    acc_freeDataBlocks();

#ifdef GENERALSTRUCTS
    userdefinedtypelist = NULL;              // malloc'd within protocolXML
    logmalloclist = NULL;
#endif

    closeIdamError(&server_block.idamerrorstack);

#ifndef FATCLIENT   // <========================== Client Server Code Only

    // After each data request, the server waits for the CLIENT_BLOCK to be sent.
    // When the client application closes, the socket is closed and the server terminates with a Protocol Error
    // meaning the expected data structure was not received. This error is written to the server log.
    // To avoid this, we can send a CLIENT_BLOCK with a CLOSEDOWN instruction.

    if (clientSocket >= 0) {
        client_block.timeout = 0;                             // Surrogate CLOSEDOWN instruction
        client_block.clientFlags = client_block.clientFlags | CLIENTFLAG_CLOSEDOWN;   // Direct CLOSEDOWN instruction
        protocol_id = PROTOCOL_CLIENT_BLOCK;
        protocol2(clientOutput, protocol_id, XDR_SEND, NULL, &client_block);
        xdrrec_endofrecord(clientOutput, 1);
    }

#else       // <========================== End of Client Server Code Only 

    //----------------------------------------------------------------------------
    // Free List of Structure Definitions

#ifdef GENERALSTRUCTS
    freeUserDefinedTypeList(&parseduserdefinedtypelist);

    freeNTree();
#endif

    // Free Plugin List and Close all open library entries

    freePluginList(&pluginList);

#endif

    idamClosedown(1);        // Close the Socket, XDR Streams and All Files

}
