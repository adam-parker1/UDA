/*---------------------------------------------------------------
* IDAM Plugin: HDF5 Files 
*
* Input Arguments:	DATA_SOURCE *data_source
*			SIGNAL_DESC *signal_desc
*
* Returns:		readHDF5	0 if read was successful
*					otherwise a Error Code is returned 
*			DATA_BLOCK	Structure with Data from the HDF5 File 
*
* Calls		freeDataBlock	to free Heap memory if an Error Occurs
*
* Notes: 	All memory required to hold data is allocated dynamically
*		in heap storage. Pointers to these areas of memory are held
*		by the passed DATA_BLOCK structure. Local memory allocations
*		are freed on exit. However, the blocks reserved for data are
*		not and MUST BE FREED by the calling routine.
*
* ToDo:		                     
*
*-----------------------------------------------------------------------------*/
#include "readHDF58.h"

#include <stdlib.h>
#include <hdf5.h>
#include <errno.h>
#include <stdlib.h>

#include <server/managePluginFiles.h>
#include <clientserver/initStructs.h>
#include <clientserver/idamTypes.h>

#include "hdf5plugin.h"

#define HDF5_ERROR_OPENING_FILE             200
#define HDF5_ERROR_IDENTIFYING_DATA_ITEM    201
#define HDF5_ERROR_OPENING_DATASPACE        202
#define HDF5_ERROR_ALLOCATING_DIM_HEAP      203
#define HDF5_ERROR_ALLOCATING_DATA_HEAP     204
#define HDF5_ERROR_READING_DATA             205
#define HDF5_ERROR_OPENING_ATTRIBUTE        206
#define HDF5_ERROR_NO_STORAGE_SIZE          207
#define HDF5_ERROR_UNKNOWN_TYPE             208
#define HDF5_ERROR_OPENING_DATASET          209

int getHDF5(DATA_SOURCE* data_source, SIGNAL_DESC* signal_desc, DATA_BLOCK* data_block)
{
    hid_t file_id = -1, dataset_id = -1, space_id = -1, datatype_id = -1, att_id = -1, grp_id = -1;
    hid_t classtype, nativetype;
    herr_t status;
    hsize_t shape[64];
    size_t typesize;
    int err = 0, natt, i, ndata, size, precision, issigned;
    char* data = NULL;

    H5O_info_t dataset_info;
    H5O_type_t dataset_type;

//----------------------------------------------------------------------
// Disable HDF5 Error Printing

    H5Eset_auto2(H5E_DEFAULT, NULL, NULL);

//---------------------------------------------------------------------- 
// Error Trap Loop

    do {

        errno = 0;

//----------------------------------------------------------------------
// Is the HDF5 file already open for reading? If not then open it. 
// The handle hid_t is an integer (ref: H5Ipublic.h) - use the Integer specific API 

        if ((file_id = getOpenIdamPluginFileInt(&pluginFileList, data_source->path)) < 0) {
            file_id = H5Fopen(data_source->path, H5F_ACC_RDONLY, H5P_DEFAULT);
            if ((int) file_id < 0 || errno != 0) {
                err = HDF5_ERROR_OPENING_FILE;
                if (errno != 0) addIdamError(&idamerrorstack, SYSTEMERRORTYPE, "readHDF5", errno, "");
                addIdamError(&idamerrorstack, CODEERRORTYPE, "readHDF5", err, "Error Opening HDF5 File");
                break;
            }
            addIdamPluginFileInt(&pluginFileList, data_source->path, file_id);        // Register the File Handle
        }

//---------------------------------------------------------------------- 
// Register the API function needed to close the file

// herr_t H5Fclose(hid_t files_id);
// herr_t and hid_t are both integers
// The return code herr_t in an integer (ref: H5public.h) - use the Integer specific API

        static unsigned short isRegistered = 0;

        if (!isRegistered) {
            void* close = (void*)&H5Fclose;
            registerIdamPluginFileClose(&pluginFileList, close);
            isRegistered = 1;
        }


//---------------------------------------------------------------------- 
// Open the Dataset

        if ((dataset_id = H5Dopen2(file_id, signal_desc->signal_name, H5P_DEFAULT)) < 0) {

// Check it's not a group level attribute

            char grpname[MAXNAME];
            char attname[MAXNAME];
            char dataset[MAXNAME];
            char* p = NULL, * d = NULL;

            strcpy(grpname, signal_desc->signal_name);
            p = strrchr(grpname, '/');
            p[0] = '\0';
            strcpy(attname, &p[1]);
            strcat(grpname, "/");

            if ((grp_id = H5Gopen2(file_id, grpname, H5P_DEFAULT)) >= 0) {
                if ((att_id = H5Aopen_name(grp_id, attname)) >= 0) {
                    err = readHDF5Att(file_id, grpname, att_id, attname, data_block);
                    break;
                }
            }

// Or check its not an attribute of the dataset

            strcpy(dataset, signal_desc->signal_name);
            if ((p = strrchr(dataset, '/')) != NULL) {
                if ((d = strrchr(dataset, '.')) != NULL) {
                    if (d > p) {
                        strcpy(attname, &d[1]);
                        d[0] = '\0';
                        if ((dataset_id = H5Dopen2(file_id, dataset, H5P_DEFAULT)) >= 0) {
                            if ((att_id = H5Aopen_name(dataset_id, attname)) >= 0) {
                                err = readHDF5Att(file_id, dataset, att_id, attname, data_block);
                                break;
                            }
                        }
                    }
                }
            }

// Must be an error!	 

            err = HDF5_ERROR_OPENING_DATASET;
            addIdamError(&idamerrorstack, CODEERRORTYPE, "readHDF5", err, "Error Opening the Signal Dataset");
            break;
        }

//---------------------------------------------------------------------- 
// Identify the Dataset Type

        if ((status = H5Oget_info(dataset_id, &dataset_info)) < 0) {
            err = HDF5_ERROR_IDENTIFYING_DATA_ITEM;
            addIdamError(&idamerrorstack, CODEERRORTYPE, "readHDF5", err, "Error Accessing Signal Dataset Information");
            break;
        }

        dataset_type = dataset_info.type;

//---------------------------------------------------------------------- 
// Size, Shape and Rank

        if (dataset_type == H5O_TYPE_DATASET) {                    // Dataset Object
            if ((space_id = H5Dget_space(dataset_id)) < 0) {
                err = HDF5_ERROR_OPENING_DATASPACE;
                addIdamError(&idamerrorstack, CODEERRORTYPE, "readHDF5", err,
                             "Error Opening the Dataspace for the Dataset");
                break;
            }
            data_block->rank = (int) H5Sget_simple_extent_dims(space_id, (hsize_t*) shape, 0);
            size = (int) H5Dget_storage_size(dataset_id);        // Amount of Storage required for the Data
            datatype_id = H5Dget_type(dataset_id);            // Identify the Data's type
            nativetype = H5Tget_native_type(datatype_id, H5T_DIR_ASCEND);  // the Native Datatype
            precision = (int) H5Tget_precision(datatype_id);        // Atomic Datatype's precision
            typesize = (int) H5Tget_size(datatype_id);            // Type Size (Bytes)
            classtype = H5Tget_class(datatype_id);            // Class
            issigned = H5Tget_sign(datatype_id) != H5T_SGN_NONE;    // Whether or Not the Type is Signed
            H5Sclose(space_id);
        } else {                                // Assume an Attribute Object
            if ((space_id = H5Aget_space(dataset_id)) < 0) {
                err = HDF5_ERROR_OPENING_DATASPACE;
                addIdamError(&idamerrorstack, CODEERRORTYPE, "readHDF5", err,
                             "Error Opening the Dataspace for the Attribute");
                break;
            }
            data_block->rank = (int) H5Sget_simple_extent_dims(space_id, (hsize_t*) shape, 0);
            size = (int) H5Aget_storage_size(dataset_id);        // Amount of Storage required for the Attribute
            datatype_id = H5Aget_type(dataset_id);
            nativetype = (hid_t) -1;
            precision = (int) H5Tget_precision(datatype_id);        // Atomic Datatype's precision
            typesize = (int) H5Tget_size(datatype_id);            // Type Size (Bytes)
            classtype = H5Tget_class(datatype_id);            // Class
            issigned = H5Tget_sign(datatype_id) != H5T_SGN_NONE;    // Whether or Not the Type is Signed
            H5Sclose(space_id);
        }

        idamLog(LOG_DEBUG, "file_id     = %d\n", (int) file_id);
        idamLog(LOG_DEBUG, "datatype_id = %d\n", (int) datatype_id);
        idamLog(LOG_DEBUG, "rank        = %d\n", data_block->rank);
        idamLog(LOG_DEBUG, "size        = %d\n", size);
        idamLog(LOG_DEBUG, "nativetype  = %d\n", nativetype);
        idamLog(LOG_DEBUG, "precision   = %d\n", precision);
        idamLog(LOG_DEBUG, "typesize    = %zu\n", typesize);
        idamLog(LOG_DEBUG, "classtype   = %d\n", (int) classtype);
        idamLog(LOG_DEBUG, "issigned    = %d\n", (int) issigned);

        idamLog(LOG_DEBUG, "Integer Class ?  %d\n", H5T_INTEGER == classtype);
        idamLog(LOG_DEBUG, "Float Class ?    %d\n", H5T_FLOAT == classtype);
        idamLog(LOG_DEBUG, "Array Class ?    %d\n", H5T_ARRAY == classtype);
        idamLog(LOG_DEBUG, "Time Class ?     %d\n", H5T_TIME == classtype);
        idamLog(LOG_DEBUG, "String Class ?   %d\n", H5T_STRING == classtype);
        idamLog(LOG_DEBUG, "Bitfield Class ? %d\n", H5T_BITFIELD == classtype);
        idamLog(LOG_DEBUG, "Opaque Class ?   %d\n", H5T_OPAQUE == classtype);
        idamLog(LOG_DEBUG, "Compound Class ? %d\n", H5T_COMPOUND == classtype);
        idamLog(LOG_DEBUG, "Reference Class ?%d\n", H5T_REFERENCE == classtype);
        idamLog(LOG_DEBUG, "Enumerated Class?%d\n", H5T_ENUM == classtype);
        idamLog(LOG_DEBUG, "VLen Class ?     %d\n", H5T_VLEN == classtype);
        idamLog(LOG_DEBUG, "No Class ?       %d\n", H5T_NO_CLASS == classtype);

        idamLog(LOG_DEBUG, "Native Char?     %d\n", H5T_NATIVE_CHAR == nativetype);
        idamLog(LOG_DEBUG, "Native Short?    %d\n", H5T_NATIVE_SHORT == nativetype);
        idamLog(LOG_DEBUG, "Native Int?      %d\n", H5T_NATIVE_INT == nativetype);
        idamLog(LOG_DEBUG, "Native Long?     %d\n", H5T_NATIVE_LONG == nativetype);
        idamLog(LOG_DEBUG, "Native LLong?    %d\n", H5T_NATIVE_LLONG == nativetype);
        idamLog(LOG_DEBUG, "Native UChar?    %d\n", H5T_NATIVE_UCHAR == nativetype);
        idamLog(LOG_DEBUG, "Native SChar?    %d\n", H5T_NATIVE_SCHAR == nativetype);
        idamLog(LOG_DEBUG, "Native UShort?   %d\n", H5T_NATIVE_USHORT == nativetype);
        idamLog(LOG_DEBUG, "Native UInt?     %d\n", H5T_NATIVE_UINT == nativetype);
        idamLog(LOG_DEBUG, "Native ULong?    %d\n", H5T_NATIVE_ULONG == nativetype);
        idamLog(LOG_DEBUG, "Native ULLong?   %d\n", H5T_NATIVE_ULLONG == nativetype);
        idamLog(LOG_DEBUG, "Native Float?    %d\n", H5T_NATIVE_FLOAT == nativetype);
        idamLog(LOG_DEBUG, "Native Double?   %d\n", H5T_NATIVE_DOUBLE == nativetype);
        idamLog(LOG_DEBUG, "Native LDouble?  %d\n", H5T_NATIVE_LDOUBLE == nativetype);

        if (size == 0) {
            if (dataset_type == H5O_TYPE_DATASET) {
                H5D_space_status_t status;
                H5Dget_space_status(dataset_id, &status);
                err = 0;
                switch (status) {
                    case (H5D_SPACE_STATUS_NOT_ALLOCATED):
                        //addIdamError(&idamerrorstack, CODEERRORTYPE, "readHDF5", err, "No Storage Allocated within the File for this data item");
                        break;
                    case (H5D_SPACE_STATUS_PART_ALLOCATED):
                        //addIdamError(&idamerrorstack, CODEERRORTYPE, "readHDF5", err, "Incomplete Storage Allocated within the File for this data item");
                        break;
                    case (H5D_SPACE_STATUS_ALLOCATED):
                        err = HDF5_ERROR_NO_STORAGE_SIZE;
                        addIdamError(&idamerrorstack, CODEERRORTYPE, "readHDF5", err,
                                     "Storage Allocated within the File for "
                                             "this data item but Zero Storage Size returned!");
                        break;
                    default:
                        err = HDF5_ERROR_NO_STORAGE_SIZE;
                        addIdamError(&idamerrorstack, CODEERRORTYPE, "readHDF5", err,
                                     "No Storage Size returned for this data item");
                        break;
                }
            } else {
                addIdamError(&idamerrorstack, CODEERRORTYPE, "readHDF5", err,
                             "No Storage Size returned for this data item");
            }
            if (err != 0) break;
        }

//----------------------------------------------------------------------
// Allocate & Initialise Dimensional Structures

        if (data_block->rank > 0) {
            if ((data_block->dims = (DIMS*) malloc(data_block->rank * sizeof(DIMS))) == NULL) {
                err = HDF5_ERROR_ALLOCATING_DIM_HEAP;
                addIdamError(&idamerrorstack, CODEERRORTYPE, "readHDF5", err,
                             "Problem Allocating Dimension Heap Memory");
                break;
            }
        }

        for (i = 0; i < data_block->rank; i++) initDimBlock(&data_block->dims[i]);

// Create Index elements for the Dimensions

        for (i = 0; i < data_block->rank; i++) {
            data_block->dims[i].compressed = 1;
            data_block->dims[i].method = 0;
            data_block->dims[i].dim_n = (int) shape[data_block->rank - i - 1];
            data_block->dims[i].dim0 = 0;
            data_block->dims[i].diff = 1;
            data_block->dims[i].data_type = TYPE_INT;        // No Standard to enable identification of the dims
            data_block->dims[i].dim = NULL;
            strcpy(data_block->dims[i].dim_label, "array index");
            data_block->dims[i].dim_units[0] = '\0';
        }

        data_block->order = -1;    // Don't know the t-vector (or any other!)

//--------------------------------------------------------------------------------------------         
// Identify the Data's Type

        switch (classtype) {
            case H5T_INTEGER:
                switch (precision) {
                    case 8:
                        data_block->data_type = issigned ? TYPE_CHAR : TYPE_UNSIGNED_CHAR;
                        break;
                    case 16:
                        data_block->data_type = issigned ? TYPE_SHORT : TYPE_UNSIGNED_SHORT;
                        break;
                    case 32:
                        data_block->data_type = issigned ? TYPE_INT : TYPE_UNSIGNED;
                        break;
                    case 64:
                        data_block->data_type = issigned ? TYPE_LONG64 : TYPE_UNSIGNED_LONG64;
                        break;
                    default:
                        data_block->data_type = TYPE_UNKNOWN;
                        break;
                }
                break;
            case H5T_FLOAT:
                switch (precision) {
                    case 32:
                        data_block->data_type = TYPE_FLOAT;
                        break;
                    case 64:
                        data_block->data_type = TYPE_DOUBLE;
                        break;
                    default:
                        data_block->data_type = TYPE_UNKNOWN;
                        break;
                }
                break;
            default:
                data_block->data_type = TYPE_UNKNOWN;
                break;
        }

        if (data_block->data_type == TYPE_UNKNOWN) {
            err = HDF5_ERROR_UNKNOWN_TYPE;
            addIdamError(&idamerrorstack, CODEERRORTYPE, "readHDF5", err, "Unknown Data Type for this data item");
            break;

        }

//--------------------------------------------------------------------------------------------         
// Attributes associated with the dataset object

// *** deprecated in version 1.8 

        natt = H5Aget_num_attrs(dataset_id);

        for (i = 0; i < natt; i++) {        // Fetch Attribute Names
            char att_name[STRING_LENGTH] = "";
            char att_buff[STRING_LENGTH] = "";
            hid_t att_id = -1;
            if ((att_id = H5Aopen_idx(dataset_id, (unsigned int) i)) < 0) {
                err = HDF5_ERROR_OPENING_ATTRIBUTE;
                addIdamError(&idamerrorstack, CODEERRORTYPE, "readHDF5", err,
                             "Problem Allocating Dimension Heap Memory");
                break;
            }
            hid_t att_type = H5Aget_type(att_id);
            int att_size = H5Aget_name(att_id, (size_t) STRING_LENGTH, att_name);
            H5Aread(att_id, att_type, (void*) att_buff);
            H5Aclose(att_id);

            idamLog(LOG_DEBUG, "%d attribute[%d]: %s\n", i, (int) att_size, att_name);
            idamLog(LOG_DEBUG, "%d type: %d\n", i, (int) att_type);
            idamLog(LOG_DEBUG, "Value: %s\n", att_buff);
            idamLog(LOG_DEBUG, "H5T_STRING     ?   %d\n", H5T_STRING == att_type);
            idamLog(LOG_DEBUG, "H5T_CSET_ASCII ?   %d\n", H5T_CSET_ASCII == att_type);
            idamLog(LOG_DEBUG, "H5T_C_S1       ?   %d\n", H5T_C_S1 == att_type);

            if (!strcasecmp(att_name, "units")) strcpy(data_block->data_units, att_buff);
            if (!strcasecmp(att_name, "label")) strcpy(data_block->data_label, att_buff);
            if (!strcasecmp(att_name, "description")) strcpy(data_block->data_desc, att_buff);

            if (strlen(data_block->data_label) == 0 && strlen(data_block->data_desc) > 0) {
                strcpy(data_block->data_label, data_block->data_desc);
                data_block->data_desc[0] = '\0';
            }
        }


//--------------------------------------------------------------------------------------------            
// Correct for Zero Dataset Size when space not fully allocated: Calculate to Access fill values 

        if (size == 0 && dataset_type == H5O_TYPE_DATASET) {
            size = 1;
            for (i = 0; i < data_block->rank; i++)size = size * (int) shape[i];
            switch (data_block->data_type) {
                case TYPE_FLOAT:
                    size = size * sizeof(float);
                    break;
                case TYPE_DOUBLE:
                    size = size * sizeof(double);
                    break;
                case TYPE_UNSIGNED_CHAR:
                    size = size * sizeof(unsigned char);
                    break;
                case TYPE_CHAR:
                    size = size * sizeof(char);
                    break;
                case TYPE_UNSIGNED_SHORT:
                    size = size * sizeof(unsigned short);
                    break;
                case TYPE_SHORT:
                    size = size * sizeof(short);
                    break;
                case TYPE_UNSIGNED:
                    size = size * sizeof(unsigned int);
                    break;
                case TYPE_INT:
                    size = size * sizeof(int);
                    break;
                case TYPE_UNSIGNED_LONG64:
                    size = size * sizeof(unsigned long long);
                    break;
                case TYPE_LONG64:
                    size = size * sizeof(long long);
                    break;
            }
        }

//--------------------------------------------------------------------------------------------            
// Allocate Heap for the Data and Read the Data

        switch (data_block->data_type) {
            case TYPE_FLOAT:
                ndata = size / sizeof(float);
                data = (char*) malloc(size);
                if (data != NULL)
                    status = H5Dread(dataset_id, H5T_NATIVE_FLOAT, H5S_ALL, H5S_ALL, H5P_DEFAULT, (void*) data);
                break;
            case TYPE_DOUBLE:
                ndata = size / sizeof(double);
                data = (char*) malloc(size);
                if (data != NULL)
                    status = H5Dread(dataset_id, H5T_NATIVE_DOUBLE, H5S_ALL, H5S_ALL, H5P_DEFAULT, (void*) data);
                break;
            case TYPE_UNSIGNED_CHAR:
                ndata = size / sizeof(unsigned char);
                data = (char*) malloc(size);
                if (data != NULL)
                    status = H5Dread(dataset_id, H5T_NATIVE_UCHAR, H5S_ALL, H5S_ALL, H5P_DEFAULT, (void*) data);
                break;
            case TYPE_CHAR:
                ndata = size / sizeof(char);
                data = (char*) malloc(size);
                if (data != NULL)
                    status = H5Dread(dataset_id, H5T_NATIVE_CHAR, H5S_ALL, H5S_ALL, H5P_DEFAULT, (void*) data);
                break;
            case TYPE_UNSIGNED_SHORT:
                ndata = size / sizeof(unsigned short);
                data = (char*) malloc(size);
                if (data != NULL)
                    status = H5Dread(dataset_id, H5T_NATIVE_USHORT, H5S_ALL, H5S_ALL, H5P_DEFAULT, (void*) data);
                break;
            case TYPE_SHORT:
                ndata = size / sizeof(short);
                data = (char*) malloc(size);
                if (data != NULL)
                    status = H5Dread(dataset_id, H5T_NATIVE_SHORT, H5S_ALL, H5S_ALL, H5P_DEFAULT, (void*) data);
                break;
            case TYPE_UNSIGNED:
                ndata = size / sizeof(unsigned int);
                data = (char*) malloc(size);
                if (data != NULL)
                    status = H5Dread(dataset_id, H5T_NATIVE_UINT, H5S_ALL, H5S_ALL, H5P_DEFAULT, (void*) data);
                break;
            case TYPE_INT:
                ndata = size / sizeof(int);
                data = (char*) malloc(size);
                if (data != NULL)
                    status = H5Dread(dataset_id, H5T_NATIVE_INT, H5S_ALL, H5S_ALL, H5P_DEFAULT, (void*) data);
                break;
            case TYPE_UNSIGNED_LONG64:
                ndata = size / sizeof(unsigned long long int);
                data = (char*) malloc(size);
                if (data != NULL)
                    status = H5Dread(dataset_id, H5T_NATIVE_ULLONG, H5S_ALL, H5S_ALL, H5P_DEFAULT, (void*) data);
                break;
            case TYPE_LONG64:
                ndata = size / sizeof(long long int);
                data = (char*) malloc(size);
                if (data != NULL)
                    status = H5Dread(dataset_id, H5T_NATIVE_LLONG, H5S_ALL, H5S_ALL, H5P_DEFAULT, (void*) data);
                break;
            default:
                break;
        }

        if (data == NULL) {
            err = HDF5_ERROR_ALLOCATING_DATA_HEAP;
            addIdamError(&idamerrorstack, CODEERRORTYPE, "readHDF5", err, "Problem Allocating Data Heap Memory");
            break;
        }

        if (status < 0) {
            err = HDF5_ERROR_READING_DATA;
            addIdamError(&idamerrorstack, CODEERRORTYPE, "readHDF5", err, "Problem Reading Data from the File");
            break;
        }

        data_block->data_n = ndata;
        data_block->data = data;

//----------------------------------------------------------------------
//----------------------------------------------------------------------
// End of Error Trap Loop

    } while (0);

//---------------------------------------------------------------------- 
// Housekeeping (files are closed via the plugin reset method)

    if (datatype_id >= 0) H5Tclose(datatype_id);
    if (dataset_id >= 0) H5Dclose(dataset_id);
    if (space_id >= 0) H5Sclose(space_id);
    if (grp_id >= 0) H5Gclose(grp_id);
    if (att_id >= 0) H5Aclose(att_id);

    H5garbage_collect();

    return err;
}

/*
//--------------------------------------------------------------------------------------------         
// Identify the Data's Type

int readHDF5IdamType(H5T_class_t classtype, int precision, int issigned)
{

    switch (classtype) {
        case H5T_INTEGER:
            switch (precision) {
                case 8:
                    return (issigned ? TYPE_CHAR : TYPE_UNSIGNED_CHAR);
                case 16:
                    return (issigned ? TYPE_SHORT : TYPE_UNSIGNED_SHORT);
                case 32:
                    return (issigned ? TYPE_INT : TYPE_UNSIGNED);
                case 64:
                    return (issigned ? TYPE_LONG64 : TYPE_UNSIGNED_LONG64);
                default:
                    return (TYPE_UNKNOWN);
            }

        case H5T_FLOAT:
            switch (precision) {
                case 32:
                    return (TYPE_FLOAT);
                case 64:
                    return (TYPE_DOUBLE);
                default:
                    return (TYPE_UNKNOWN);
            }

        case H5T_STRING:
            return (TYPE_CHAR);

        default:
            return (TYPE_UNKNOWN);
    }
    return (TYPE_UNKNOWN);
}


int readHDF5Att(hid_t file_id, char* object, hid_t att_id, char* attname, DATA_BLOCK* data_block)
{
    H5T_class_t classtype;
    int err = 0, rc, i;
    char* data = NULL;
    hid_t datatype_id, space_id;
    int size = 0, precision = 0, issigned = 0;
    hsize_t shape[64];

// Get the Size & Dimensionality 

    if ((space_id = H5Aget_space(att_id)) < 0) {
        err = 999;
        addIdamError(&idamerrorstack, CODEERRORTYPE, "readHDF5Att", err,
                     "Error Querying for Attribute Space Information");
        return err;
    }

    data_block->rank = (int) H5Sget_simple_extent_dims(space_id, shape, 0);    // Shape of Dimensions
    H5Sclose(space_id);

    size = (int) H5Aget_storage_size(att_id);                    // Amount of Storage required for the Attribute

    if (size == 0) {
        err = 999;
        addIdamError(&idamerrorstack, CODEERRORTYPE, "readHDF5Att", err, "Attribute Size is Zero!");
        return err;
    }

// Get the Precision and if signed   

    datatype_id = H5Aget_type(att_id);
    precision = (int) H5Tget_precision(datatype_id);        // Atomic Datatype's precision
    classtype = H5Tget_class(datatype_id);            // Class
    issigned = H5Tget_sign(datatype_id) != H5T_SGN_NONE;    // Whether or Not the Type is Signed

    H5Tclose(datatype_id);

// Identify the IDAM type

    data_block->data_type = readHDF5IdamType(classtype, precision, issigned);

    if (data_block->data_type == TYPE_UNKNOWN) {
        err = 999;
        addIdamError(&idamerrorstack, CODEERRORTYPE, "readHDF5Att", err, "Attribute Data Type is Unknown!");
        return err;
    }

// Allocate Heap for the Data

    if (classtype == H5T_STRING) {
        data = (char*) malloc(size + 1);
    } else {
        data = (char*) malloc(size);
    }

    if (data == NULL) {
        err = 999;
        addIdamError(&idamerrorstack, CODEERRORTYPE, "readHDF5Att", err,
                     "Unable to Allocate HEAP Memory for Attribute Data");
        return err;
    }

// Read the data into the Appropriate Data Type

    rc = 0;

    switch (data_block->data_type) {
        case TYPE_FLOAT:
            data_block->data_n = size / sizeof(float);
            rc = H5Aread(att_id, H5T_NATIVE_FLOAT, (void*) data);
            break;
        case TYPE_DOUBLE:
            data_block->data_n = size / sizeof(double);
            rc = H5Aread(att_id, H5T_NATIVE_DOUBLE, (void*) data);
            break;
        case TYPE_UNSIGNED_CHAR:
            data_block->data_n = size / sizeof(unsigned char);
            rc = H5Aread(att_id, H5T_NATIVE_UCHAR, (void*) data);
            break;
        case TYPE_CHAR:
            data_block->data_n = size / sizeof(char);
            if (classtype == H5T_STRING) {
                rc = H5LTget_attribute_string(file_id, object, attname, (char*) data);
            } else {
                rc = H5Aread(att_id, H5T_NATIVE_CHAR, (void*) data);
            }
            break;
        case TYPE_UNSIGNED_SHORT:
            data_block->data_n = size / sizeof(unsigned short);
            rc = H5Aread(att_id, H5T_NATIVE_USHORT, (void*) data);
            break;
        case TYPE_SHORT:
            data_block->data_n = size / sizeof(short);
            rc = H5Aread(att_id, H5T_NATIVE_SHORT, (void*) data);
            break;
        case TYPE_UNSIGNED:
            data_block->data_n = size / sizeof(unsigned int);
            rc = H5Aread(att_id, H5T_NATIVE_UINT, (void*) data);
            break;
        case TYPE_INT:
            data_block->data_n = size / sizeof(int);
            rc = H5Aread(att_id, H5T_NATIVE_INT, (void*) data);
            break;
        case TYPE_UNSIGNED_LONG64:
            data_block->data_n = size / sizeof(unsigned long long int);
            rc = H5Aread(att_id, H5T_NATIVE_ULLONG, (void*) data);
            break;
        case TYPE_LONG64:
            data_block->data_n = size / sizeof(long long int);
            rc = H5Aread(att_id, H5T_NATIVE_LLONG, (void*) data);
            break;
        default:
            rc = 1;
            break;
    }

    if (rc < 0) {
        err = 999;
        addIdamError(&idamerrorstack, CODEERRORTYPE, "readHDF5Att", err, "Error reading Attribute Data");
        free((void*) data);
        return err;
    }

// Fill out the DATA_BLOCK structure  

    data_block->order = -1;
    data_block->data = (char*) data;
    strcpy(data_block->data_units, "");
    strcpy(data_block->data_label, attname);
    strcpy(data_block->data_desc, object);

    if (data_block->rank >= 1 && data_block->data_n > 1) {
        if ((data_block->dims = (DIMS*) malloc(data_block->rank * sizeof(DIMS))) == NULL) {
            err = HDF5_ERROR_ALLOCATING_DIM_HEAP;
            addIdamError(&idamerrorstack, CODEERRORTYPE, "readHDF5", err, "Problem Allocating Dimension Heap Memory");
            return err;
        }

        for (i = 0; i < data_block->rank; i++) {
            initDimBlock(&data_block->dims[i]);
            data_block->dims[i].compressed = 1;
            data_block->dims[i].method = 0;
            data_block->dims[i].dim_n = (int) shape[data_block->rank - i - 1];
            data_block->dims[i].dim0 = 0;
            data_block->dims[i].diff = 1;
            data_block->dims[i].data_type = TYPE_INT;        // No Standard to enable identification of the dims
            data_block->dims[i].dim = NULL;
            strcpy(data_block->dims[i].dim_label, "array index");
            data_block->dims[i].dim_units[0] = '\0';
        }
    } else data_block->rank = 0;

    return 0;

}
*/