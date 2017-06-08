#include "geomSignalMap.h"

#include <stdlib.h>
#include <strings.h>

#include <structures/struct.h>
#include <structures/accessors.h>
#include <server/modules/netcdf4/readCDF4.h>
#include <clientserver/stringUtils.h>
#include <clientserver/udaTypes.h>
#include <clientserver/initStructs.h>

/////////
// Check which signal id s are available for this exp number,
// and flag those in the is_available element of the data structure.
// Requires connecting to main IDAM db (rather than geom db)
/////////	
int checkAvailableSignals(int shot, int n_all, int** signal_ids, int** is_available)
{

    IDAM_LOG(LOG_DEBUG, "Checking for signal ids in IDAM\n");

    int n_signals_available = 0;

    PGconn* DBConnect = startSQL();
    PGresult* DBQuery_IDAM = NULL;

    if (DBConnect == NULL) {
        IDAM_LOG(LOG_ERROR, "Connection to IDAM database failed.\n");
        return 0;
    }

    int i;
    for (i = 0; i < n_all; i++) {
        char query_idam[MAXSQL];

        sprintf(query_idam,
                "SELECT ds.exp_number FROM data_source ds, signal s, signal_desc sd"
                        " WHERE ds.exp_number=%d"
                        "   AND ds.source_id=s.source_id"
                        "   AND s.signal_desc_id=sd.signal_desc_id"
                        "   AND sd.signal_desc_id=%d;",
                shot, (*signal_ids)[i]);

        if ((DBQuery_IDAM = PQexec(DBConnect, query_idam)) == NULL) {
            IDAM_LOG(LOG_ERROR, "IDAM database query failed.\n");
            continue;
        }

        if (PQresultStatus(DBQuery_IDAM) != PGRES_TUPLES_OK && PQresultStatus(DBQuery_IDAM) != PGRES_COMMAND_OK) {
            PQclear(DBQuery_IDAM);
            IDAM_LOG(LOG_ERROR, "Database query failed.\n");
            continue;
        }

        int nRows_idam = PQntuples(DBQuery_IDAM);

        if (nRows_idam == 0) {
            PQclear(DBQuery_IDAM);
            continue;
        }

        n_signals_available = n_signals_available + 1;
        (*is_available)[i] = 1;

        PQclear(DBQuery_IDAM);
    }

    PQfinish(DBConnect);

    if (n_signals_available == 0) {
        IDAM_LOG(LOG_DEBUG, "None of the signals for this geometry component are available for this shot\n");
    }

    return n_signals_available;

}

int do_signal_file(IDAM_PLUGIN_INTERFACE* idam_plugin_interface)
{

    ////////////////
    // Retrieve user inputs
    const char* signal = NULL;
    FIND_REQUIRED_STRING_VALUE(idam_plugin_interface->request_block->nameValueList, signal);

    IDAM_LOGF(LOG_DEBUG, "Using signal name: %s\n", signal);

    const char* file = NULL;
    FIND_STRING_VALUE(idam_plugin_interface->request_block->nameValueList, file);

    int version = -1;
    FIND_INT_VALUE(idam_plugin_interface->request_block->nameValueList, version);

    int shot = idam_plugin_interface->request_block->exp_number;
    int keep_all = findValue(&idam_plugin_interface->request_block->nameValueList, "keep_all");

    //////////////////////////////
    // Open the connection
    // CURRENTLY HARDCODED IN WHILE I'M TESTING
    // .... Once this is actually in the new MAST-U db, will need to use idam functions as in readMeta to open connection.
    IDAM_LOG(LOG_DEBUG, "trying to get connection\n");

    char* db_host = getenv("GEOM_DB_HOST");
    char* db_port_str = getenv("GEOM_DB_PORT");
    int db_port = -1;
    if (db_port_str != NULL) db_port = atoi(db_port_str);
    char* db_name = getenv("GEOM_DB_NAME");
    char* db_user = getenv("GEOM_DB_USER");

    if (db_host == NULL || db_port_str == NULL || db_name == NULL || db_user == NULL) {
        RAISE_PLUGIN_ERROR("Geom db host, port, name and user env variables were not set.\n");
    }

    PGconn* DBConnect = openDatabase(db_host, db_port, db_name, db_user);

    PGresult* DBQuery = NULL;

    if (PQstatus(DBConnect) != CONNECTION_OK) {
        PQfinish(DBConnect);
        RAISE_PLUGIN_ERROR("Connection to mastgeom database failed.\n");
    }

    char* signal_for_query = (char*)malloc((2 * strlen(signal) + 1) * sizeof(char));
    int err = 0;
    PQescapeStringConn(DBConnect, signal_for_query, signal, strlen(signal), &err);

    IDAM_LOGF(LOG_DEBUG, "signal_for_query %s\n", signal_for_query);

    /////////////////////
    // Query to find filename containing the data signal asked for
    char query[MAXSQL];

    if (version < 0) {
        sprintf(query,
                "SELECT dgs.file_name, dgs.version, dgsm.signal_alias, dgsm.signal_desc_id"
                        " FROM datasignal_geomdata_source dgs, datasignal_geomdata_source_map dgsm"
                        " WHERE dgsm.signal_alias LIKE lower('%s/%%')"
                        "   OR dgsm.var_name LIKE lower('%s/%%')"
                        "   AND dgs.start_shot<=%d AND dgs.end_shot>%d"
                        "   AND dgsm.datasignal_geomdata_source_id=dgs.datasignal_geomdata_source_id"
                        "   AND dgs.version=(SELECT max(datasignal_geomdata_source.version)"
                        "     FROM datasignal_geomdata_source, datasignal_geomdata_source_map"
                        "     WHERE datasignal_geomdata_source_map.signal_alias LIKE lower('%s/%%')"
                        "       OR datasignal_geomdata_source_map.var_name LIKE lower('%s/%%')"
                        "       AND datasignal_geomdata_source.start_shot<=%d"
                        "       AND datasignal_geomdata_source.end_shot>%d"
                        "       AND datasignal_geomdata_source.datasignal_geomdata_source_id=datasignal_geomdata_source_map.datasignal_geomdata_source_id);",
                signal_for_query, signal_for_query, shot, shot, signal_for_query, signal_for_query, shot, shot);
    } else {
        sprintf(query,
                "SELECT dgs.file_name, dgs.version, dgsm.signal_alias, dgsm.signal_desc_id"
                        " FROM datasignal_geomdata_source dgs, datasignal_geomdata_source_map dgsm"
                        " WHERE dgsm.signal_alias LIKE lower('%s/%%')"
                        "   OR dgsm.var_name LIKE lower('%s/%%')"
                        "   AND dgs.start_shot<=%d"
                        "   AND dgs.end_shot>%d"
                        "   AND dgsm.datasignal_geomdata_source_id=dgs.datasignal_geomdata_source_id"
                        "   AND dgs.version=%d;",
                signal_for_query, signal_for_query, shot, shot, version);
    }

    IDAM_LOGF(LOG_DEBUG, "query is %s\n", query);

    if ((DBQuery = PQexec(DBConnect, query)) == NULL) {
        RAISE_PLUGIN_ERROR("Database query failed.\n");
    }

    if (PQresultStatus(DBQuery) != PGRES_TUPLES_OK && PQresultStatus(DBQuery) != PGRES_COMMAND_OK) {
        PQclear(DBQuery);
        PQfinish(DBConnect);
        RAISE_PLUGIN_ERROR("Database query failed.\n");
    }

    int nRows = PQntuples(DBQuery);
    IDAM_LOGF(LOG_DEBUG, "nRows returned from db : %d\n", nRows);

    if (nRows == 0) {
        PQclear(DBQuery);
        PQfinish(DBConnect);
        RAISE_PLUGIN_ERROR("No rows were found in database matching query\n");
    }

    char** all_sig_alias = (char**)malloc((nRows) * sizeof(char*));
    int* all_sig_id = (int*)malloc((nRows) * sizeof(int));
    int* is_available = (int*)malloc((nRows) * sizeof(int));

    int s_file = PQfnumber(DBQuery, "file_name");
    int s_sig_alias = PQfnumber(DBQuery, "signal_alias");
    int s_sig_id = PQfnumber(DBQuery, "signal_desc_id");

    // From the first row: get the filename, and check what type we are dealing with
    // (ie. is this the signal itself? or a group?)
    // For all rows, retrieve signal aliases & ids
    char* file_path = getenv("MAST_GEOM_DATA");
    char* file_db;
    char* signal_type = NULL;

    int i = 0;

    for (i = 0; i < nRows; i++) {
        // filename (1st row only)
        if (i == 0) {
            if (!PQgetisnull(DBQuery, i, s_file)) {
                file_db = PQgetvalue(DBQuery, 0, s_file);
                char* filename = (char*)malloc(sizeof(char) * (strlen(file_db) + strlen(file_path) + 1));
                strcpy(filename, file_path);
                strcat(filename, file_db);
                file = filename;
            }
        }

        // sig ids
        if (!PQgetisnull(DBQuery, i, s_sig_id)) {
            all_sig_id[i] = atoi(PQgetvalue(DBQuery, i, s_sig_id));
        } else {
            all_sig_id[i] = -1;
        }
        is_available[i] = 0;

        // signal alias
        if (!PQgetisnull(DBQuery, i, s_sig_alias)) {
            char* sigAlias = PQgetvalue(DBQuery, i, s_sig_alias);
            all_sig_alias[i] = (char*)malloc(sizeof(char) * (strlen(sigAlias) + 1));
            strcpy(all_sig_alias[i], sigAlias);

            // signal type (1st row only)
            if (i == 0) {
                if (!strncmp(sigAlias, signal_for_query, strlen(sigAlias) - 1)) {
                    signal_type = (char*)malloc(sizeof(char) * 7 + 1);
                    strcpy(signal_type, "element");
                } else {
                    signal_type = (char*)malloc(sizeof(char) * 5 + 1);
                    strcpy(signal_type, "group");
                }
            }
        }
    }

    //Close db connection
    PQclear(DBQuery);
    PQfinish(DBConnect);
    free(signal_for_query);

    /////////////////////////////////
    // Check which signal aliases are actually available for this shot, in the IDAM db
    int n_signals_available = checkAvailableSignals(shot, nRows, &all_sig_id, &is_available);

    IDAM_LOGF(LOG_DEBUG, "n sig available %d\n", n_signals_available);

    if (n_signals_available == 0 && keep_all == 0) {
        RAISE_PLUGIN_ERROR("None of the signals in this file are available for this shot.\n");
    }

    /////////////////////////////////
    // Read in the file
    DATA_SOURCE* data_source = idam_plugin_interface->data_source;
    SIGNAL_DESC* signal_desc = idam_plugin_interface->signal_desc;
    strcpy(signal_desc->signal_name, signal);
    strcpy(data_source->path, file);

    DATA_BLOCK data_block_file;
    initDataBlock(&data_block_file);

    REQUEST_BLOCK* request_block = idam_plugin_interface->request_block;
    err = readCDF(*data_source, *signal_desc, *request_block, &data_block_file);

    IDAM_LOGF(LOG_DEBUG, "Read in file signal %s\n", signal_desc->signal_name);

    if (err != 0) {
        RAISE_PLUGIN_ERROR("Error reading geometry data!\n");
    }

    if (data_block_file.data_type != TYPE_COMPOUND) {
        RAISE_PLUGIN_ERROR("Non-structured type returned from data reader!\n");
    }

    /////////////////////////////
    // Combine datablock and signal_type into one structure
    // to be returned
    USERDEFINEDTYPE* udt = data_block_file.opaque_block;

    struct DATAPLUSTYPE {
        void* data;
        char* signal_type;
        char** signal_alias_available;
    };
    typedef struct DATAPLUSTYPE DATAPLUSTYPE;

    USERDEFINEDTYPE parentTree;
    COMPOUNDFIELD field;
    int offset = 0;

    //User defined type to describe data structure
    initUserDefinedType(&parentTree);
    parentTree.idamclass = TYPE_COMPOUND;
    strcpy(parentTree.name, "DATAPLUSTYPE");
    strcpy(parentTree.source, "netcdf");
    parentTree.ref_id = 0;
    parentTree.imagecount = 0;
    parentTree.image = NULL;
    parentTree.size = sizeof(DATAPLUSTYPE);

    //Compound field for calibration file
    initCompoundField(&field);
    strcpy(field.name, "data");
    field.atomictype = TYPE_UNKNOWN;
    strcpy(field.type, udt->name);
    strcpy(field.desc, "data");

    field.pointer = 1;
    field.count = 1;
    field.rank = 0;
    field.shape = NULL;            // Needed when rank >= 1

    field.size = field.count * sizeof(void*);
    field.offset = newoffset(offset, field.type);
    field.offpad = padding(offset, field.type);
    field.alignment = getalignmentof(field.type);
    offset = field.offset + field.size;    // Next Offset
    addCompoundField(&parentTree, field);

    // For signal_type
    initCompoundField(&field);
    defineField(&field, "signal_type", "signal_type", &offset, SCALARSTRING);
    addCompoundField(&parentTree, field);

    // Available signal aliases
    initCompoundField(&field);
    defineField(&field, "signal_alias_available", "signal_alias_available", &offset, ARRAYSTRING);
    addCompoundField(&parentTree, field);

    addUserDefinedType(userdefinedtypelist, parentTree);

    DATAPLUSTYPE* data;

    size_t stringLength = strlen(signal_type) + 1;
    data = (DATAPLUSTYPE*)malloc(sizeof(DATAPLUSTYPE));
    data->signal_type = (char*)malloc(stringLength * sizeof(char));
    data->data = data_block_file.data;
    strcpy(data->signal_type, signal_type);

    addMalloc((void*)data, 1, sizeof(DATAPLUSTYPE), "DATAPLUSTYPE");
    addMalloc((void*)data->signal_type, 1, stringLength * sizeof(char), "char");

    if (n_signals_available > 0) {
        data->signal_alias_available = (char**)malloc(n_signals_available * sizeof(char*));
        int i_avail = 0;
        for (i = 0; i < nRows; i++) {
            if (is_available[i] == 1) {
                size_t strLength = strlen(all_sig_alias[i]) + 1;
                data->signal_alias_available[i_avail] = (char*)malloc(strLength * sizeof(char));
                addMalloc(data->signal_alias_available[i_avail], (int)strLength, sizeof(char), "char");
                strcpy(data->signal_alias_available[i_avail], all_sig_alias[i]);
                i_avail = i_avail + 1;
            }
            if (i_avail > n_signals_available - 1) {
                break;
            }
        }
        addMalloc((void*)data->signal_alias_available, n_signals_available, sizeof(char*), "STRING *");
    } else {
        data->signal_alias_available = (char**)malloc(sizeof(char*));
        data->signal_alias_available[0] = (char*)malloc(5 * sizeof(char));
        strcpy(data->signal_alias_available[0], "None");
        addMalloc(data->signal_alias_available[0], 5, sizeof(char), "char");
        addMalloc((void*)data->signal_alias_available, 1, sizeof(char*), "STRING *");
    }

    //Return data
    DATA_BLOCK* data_block = idam_plugin_interface->data_block;

    initDataBlock(data_block);

    data_block->data_type = TYPE_COMPOUND;
    data_block->rank = 0;            // Scalar structure (don't need a DIM array)
    data_block->data_n = 1;
    data_block->data = (char*)data;

    strcpy(data_block->data_desc, "Data plus type");
    strcpy(data_block->data_label, "Data plus type");
    strcpy(data_block->data_units, "");

    data_block->opaque_type = OPAQUE_TYPE_STRUCTURES;
    data_block->opaque_count = 1;
    data_block->opaque_block = (void*)findUserDefinedType("DATAPLUSTYPE", 0);

    // Free heap data associated with the DATA_BLOCKS
    // Nothing to free?
    data_block_file.data = NULL;

    return 0;
}


int do_signal_filename(IDAM_PLUGIN_INTERFACE* idam_plugin_interface)
{

    ///////////////////
    // getSignalFilename : retrieve name of filename containing information about
    //                    data signals associated with a particular geomsignal group or element.
    //     Arguments:
    //         - geomsignal : Group or signal for which to retrieve corresponding signal filename with signal info
    //         - version : file version number. If not set then the latest will be returned.
    ///////////////////

    /////////////
    // Retrieve user inputs
    const char* geomsignal = NULL;
    FIND_REQUIRED_STRING_VALUE(idam_plugin_interface->request_block->nameValueList, geomsignal);

    int shot = idam_plugin_interface->request_block->exp_number;

    int version = -1;
    FIND_INT_VALUE(idam_plugin_interface->request_block->nameValueList, version);

    // Struct to put geom aliases and signal aliases, signal ids into
    typedef struct DataStructFull {
        char** file_name;
        char** geom_alias;
        char** signal_alias;
        char** var_name;
        int* signal_id;
        int* is_available;
    } DATASTRUCTFULL;

    DATASTRUCTFULL* data;
    data = (DATASTRUCTFULL*)malloc(sizeof(DATASTRUCTFULL));

    char* file_path = getenv("MAST_GEOM_DATA");

    ////////////////////
    // Query to find data signals and filename associated with given geom group
    IDAM_LOG(LOG_DEBUG, "trying to get connection\n");
    char* db_host = getenv("GEOM_DB_HOST");
    char* db_port_str = getenv("GEOM_DB_PORT");
    int db_port = -1;
    if (db_port_str != NULL) db_port = atoi(db_port_str);
    char* db_name = getenv("GEOM_DB_NAME");
    char* db_user = getenv("GEOM_DB_USER");

    if (db_host == NULL || db_port_str == NULL || db_name == NULL || db_user == NULL) {
        RAISE_PLUGIN_ERROR("Geom db host, port, name and user env variables were not set.\n");
    }

    PGconn* DBConnect = openDatabase(db_host, db_port, db_name, db_user);
    PGresult* DBQuery = NULL;

    if (PQstatus(DBConnect) != CONNECTION_OK) {
        PQfinish(DBConnect);
        RAISE_PLUGIN_ERROR("Connection to mastgeom database failed.\n");
    }

    char* signal_for_query = (char*)malloc((2 * strlen(geomsignal) + 1) * sizeof(char));
    int err = 0;
    PQescapeStringConn(DBConnect, signal_for_query, geomsignal, strlen(geomsignal), &err);

    IDAM_LOGF(LOG_DEBUG, "signal_for_query %s\n", signal_for_query);

    char query[MAXSQL];

    sprintf(query,
            "SELECT geomgroup_geomsignal_map.geomsignal_alias, datasignal_geomdata_source.file_name,"
                    " datasignal_geomdata_source_map.signal_alias, datasignal_geomdata_source_map.var_name,"
                    " datasignal_geomdata_source_map.signal_desc_id, datasignal_geomdata_source.version"
                    " FROM datasignal_geomdata_source"
                    "   INNER JOIN datasignal_geomdata_source_map"
                    "     ON datasignal_geomdata_source.datasignal_geomdata_source_id=datasignal_geomdata_source_map.datasignal_geomdata_source_id"
                    "   INNER JOIN datasignal_geomsignal_map"
                    "     ON datasignal_geomsignal_map.signal_desc_id=datasignal_geomdata_source_map.signal_desc_id"
                    "   INNER JOIN geomgroup_geomsignal_map"
                    "     ON geomgroup_geomsignal_map.geomgroup_geomsignal_map_id=datasignal_geomsignal_map.geomgroup_geomsignal_map_id"
                    "   INNER JOIN config_data_source"
                    "     ON config_data_source.config_data_source_id=geomgroup_geomsignal_map.config_data_source_id"
                    " WHERE config_data_source.start_shot<=%d"
                    "   AND config_data_source.end_shot>%d"
                    "   AND datasignal_geomdata_source.start_shot<=%d"
                    "   AND datasignal_geomdata_source.end_shot>%d"
                    "   AND geomgroup_geomsignal_map.geomsignal_alias LIKE lower('%s/%%')"
                    "   AND config_data_source.version=("
                    "       SELECT max(config_data_source.version)"
                    "       FROM config_data_source"
                    "       WHERE config_data_source.start_shot<=%d"
                    "         AND config_data_source.end_shot>%d)",
            shot, shot, shot, shot, signal_for_query, shot, shot);

    if (version >= 0) {
        strcat(query, " AND dgs.version=");
        char ver_str[10];
        sprintf(ver_str, "%d", version);
        strcat(query, ver_str);
    } else {
        char version_statement[MAXSQL];
        sprintf(version_statement,
                " AND datasignal_geomdata_source.version=("
                        " SELECT max(datasignal_geomdata_source.version)"
                        "   FROM datasignal_geomdata_source"
                        "   WHERE datasignal_geomdata_source.start_shot<=%d"
                        "     AND datasignal_geomdata_source.end_shot>%d)",
                shot, shot);
        strcat(query, version_statement);
    }
    strcat(query, ";");

    IDAM_LOGF(LOG_DEBUG, "query is %s\n", query);

    if ((DBQuery = PQexec(DBConnect, query)) == NULL) {
        RAISE_PLUGIN_ERROR("Database query failed.\n");
    }

    if (PQresultStatus(DBQuery) != PGRES_TUPLES_OK && PQresultStatus(DBQuery) != PGRES_COMMAND_OK) {
        PQclear(DBQuery);
        PQfinish(DBConnect);
        RAISE_PLUGIN_ERROR("Database query failed.\n");
    }

    int nRows = PQntuples(DBQuery);

    if (nRows == 0) {
        PQclear(DBQuery);
        PQfinish(DBConnect);
        RAISE_PLUGIN_ERROR("No rows were found in database matching query\n");
    }

    //Setup geom_alias and signal_alias fields in the data struct
    data->file_name = (char**)malloc((nRows) * sizeof(char*));
    data->geom_alias = (char**)malloc((nRows) * sizeof(char*));
    data->signal_alias = (char**)malloc((nRows) * sizeof(char*));
    data->var_name = (char**)malloc((nRows) * sizeof(char*));
    data->signal_id = (int*)malloc((nRows) * sizeof(int));
    data->is_available = (int*)malloc((nRows) * sizeof(int));

    int s_id = PQfnumber(DBQuery, "signal_desc_id");
    int s_geom_alias = PQfnumber(DBQuery, "geomsignal_alias");
    int s_sig_alias = PQfnumber(DBQuery, "signal_alias");
    int s_var_name = PQfnumber(DBQuery, "var_name");
    int s_file = PQfnumber(DBQuery, "file_name");

    int i = 0;

    for (i = 0; i < nRows; i++) {
        data->is_available[i] = 0;

        // Signal id
        if (!PQgetisnull(DBQuery, i, s_id)) {
            data->signal_id[i] = atoi(PQgetvalue(DBQuery, i, s_id));
        } else {
            data->signal_id[i] = -1;
        }

        // Geom alias
        if (!PQgetisnull(DBQuery, i, s_geom_alias)) {
            size_t stringLength = strlen(PQgetvalue(DBQuery, i, s_geom_alias)) + 1;
            data->geom_alias[i] = (char*)malloc(stringLength * sizeof(char));
            strcpy(data->geom_alias[i], PQgetvalue(DBQuery, i, s_geom_alias));
        }

        // Signal alias
        if (!PQgetisnull(DBQuery, i, s_sig_alias)) {
            size_t stringLength = strlen(PQgetvalue(DBQuery, i, s_sig_alias)) + 1;
            data->signal_alias[i] = (char*)malloc(stringLength * sizeof(char));
            strcpy(data->signal_alias[i], PQgetvalue(DBQuery, i, s_sig_alias));
        }

        // Varname
        if (!PQgetisnull(DBQuery, i, s_sig_alias)) {
            size_t stringLength = strlen(PQgetvalue(DBQuery, i, s_var_name)) + 1;
            data->var_name[i] = (char*)malloc(stringLength * sizeof(char));
            strcpy(data->var_name[i], PQgetvalue(DBQuery, i, s_var_name));
        }

        // File name
        if (!PQgetisnull(DBQuery, i, s_file)) {
            size_t stringLength = strlen(PQgetvalue(DBQuery, i, s_file)) + strlen(file_path) + 1;
            data->file_name[i] = (char*)malloc(stringLength * sizeof(char));
            strcpy(data->file_name[i], file_path);
            strcat(data->file_name[i], PQgetvalue(DBQuery, i, s_file));
        }
    }

    //Close db connection
    PQclear(DBQuery);
    PQfinish(DBConnect);
    free(signal_for_query);

    /////////
    // Check which signal id s are available for this exp number,
    /////////
    int n_signals_available = checkAvailableSignals(shot, nRows, &data->signal_id, &data->is_available);

    if (n_signals_available == 0) {
        RAISE_PLUGIN_ERROR("None of the signals for this geometry component are available for this shot\n");
    }

    /////////////////////
    // Only keep filenames and other info for those data signals that are available for the requested shot.
    // Copy info into struct that will be returned to user
    typedef struct DataStruct {
        char** filenames;
        char** geom_alias;
        char** signal_alias;
        char** var_name;
    } DATASTRUCT;

    DATASTRUCT* data_out;
    data_out = (DATASTRUCT*)malloc(sizeof(DATASTRUCT));
    addMalloc((void*)data_out, 1, sizeof(DATASTRUCT), "DATASTRUCT");

    data_out->filenames = (char**)malloc((n_signals_available) * sizeof(char*));
    addMalloc((void*)data_out->filenames, n_signals_available, sizeof(char*), "STRING *");
    data_out->geom_alias = (char**)malloc((n_signals_available) * sizeof(char*));
    addMalloc((void*)data_out->geom_alias, n_signals_available, sizeof(char*), "STRING *");
    data_out->signal_alias = (char**)malloc((n_signals_available) * sizeof(char*));
    addMalloc((void*)data_out->signal_alias, n_signals_available, sizeof(char*), "STRING *");
    data_out->var_name = (char**)malloc((n_signals_available) * sizeof(char*));
    addMalloc((void*)data_out->var_name, n_signals_available, sizeof(char*), "STRING *");

    // Transfer data to data_out
    int out_index = 0;
    for (i = 0; i < nRows; i++) {
        if (data->is_available[i] > 0) {
            size_t stringLength = strlen(data->file_name[i]) + 1;
            data_out->filenames[out_index] = (char*)malloc(stringLength * sizeof(char));
            strcpy(data_out->filenames[out_index], data->file_name[i]);
            addMalloc((void*)data_out->filenames[out_index], (int)stringLength, sizeof(char), "char");

            stringLength = strlen(data->geom_alias[i]) + 1;
            data_out->geom_alias[out_index] = (char*)malloc(stringLength * sizeof(char));
            strcpy(data_out->geom_alias[out_index], data->geom_alias[i]);
            addMalloc((void*)data_out->geom_alias[out_index], (int)stringLength, sizeof(char), "char");

            stringLength = strlen(data->signal_alias[i]) + 1;
            data_out->signal_alias[out_index] = (char*)malloc(stringLength * sizeof(char));
            strcpy(data_out->signal_alias[out_index], data->signal_alias[i]);
            addMalloc((void*)data_out->signal_alias[out_index], (int)stringLength, sizeof(char), "char");

            stringLength = strlen(data->var_name[i]) + 1;
            data_out->var_name[out_index] = (char*)malloc(stringLength * sizeof(char));
            strcpy(data_out->var_name[out_index], data->var_name[i]);
            addMalloc((void*)data_out->var_name[out_index], (int)stringLength, sizeof(char), "char");

            out_index = out_index + 1;
        }
    }

    //////////////////////////////
    /// Need to free memory in temporary data struct
    //////////////////////////////
    free(data->geom_alias);
    free(data->signal_alias);
    free(data->var_name);
    free(data->signal_id);
    free(data->is_available);
    free(data);

    USERDEFINEDTYPE parentTree;
    COMPOUNDFIELD field;
    int offset = 0;

    //////////////////
    //User defined type to describe data structure
    initUserDefinedType(&parentTree);
    parentTree.idamclass = TYPE_COMPOUND;
    strcpy(parentTree.name, "DATASTRUCT");
    strcpy(parentTree.source, "netcdf");
    parentTree.ref_id = 0;
    parentTree.imagecount = 0;
    parentTree.image = NULL;
    parentTree.size = sizeof(DATASTRUCT);

    //Compound field for calibration file
    initCompoundField(&field);
    strcpy(field.name, "filenames");
    defineField(&field, "filenames", "filenames", &offset, ARRAYSTRING);
    addCompoundField(&parentTree, field);

    initCompoundField(&field);
    strcpy(field.name, "geom_alias");
    defineField(&field, "geom_alias", "geom_alias", &offset, ARRAYSTRING);
    addCompoundField(&parentTree, field);

    initCompoundField(&field);
    strcpy(field.name, "signal_alias");
    defineField(&field, "signal_alias", "signal_alias", &offset, ARRAYSTRING);
    addCompoundField(&parentTree, field);

    initCompoundField(&field);
    strcpy(field.name, "var_name");
    defineField(&field, "var_name", "var_name", &offset, ARRAYSTRING);
    addCompoundField(&parentTree, field);

    addUserDefinedType(userdefinedtypelist, parentTree);

    // Put file name into signal block, to return to user.
    DATA_BLOCK* data_block = idam_plugin_interface->data_block;

    initDataBlock(data_block);

    data_block->data_type = TYPE_COMPOUND;
    data_block->rank = 0;            // Scalar structure (don't need a DIM array)
    data_block->data_n = 1;
    data_block->data = (char*)data_out;

    strcpy(data_block->data_desc, "Filenames");
    strcpy(data_block->data_label, "Filenames");
    strcpy(data_block->data_units, "");

    data_block->opaque_type = OPAQUE_TYPE_STRUCTURES;
    data_block->opaque_count = 1;
    data_block->opaque_block = (void*)findUserDefinedType("DATASTRUCT", 0);

    return 0;

}