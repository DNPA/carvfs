#ifndef CARVFSMOD_H
#define CARVFSMOD_H
#include <sys/types.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef enum
{
   METATYPE_LATIN=0,
   METATYPE_INT,
   METATYPE_ISO8601,
   METATYPE_FLOAT
} meta_type;

typedef enum 
{
  META_ID_CASE=0,
  META_ID_EVIDENCE_SOURCE,
  META_ID_ITEM,
  META_ID_MD5,
  META_ID_SHA1
} meta_id;

typedef struct carvpath_module_operations {
   //Open the image file(s) indicated by argc,argv as one image. 
   void * (*open_image)(int argc,const char *argv[]);
   //Close the earlier opened image.
   void   (*close_image)(void *handle);
   //Fetch the full size of the opened image.
   off_t  (*data_size)(void *handle);
   //Read a chunk of data from the opened image.
   size_t (*read_random)(void *handle,char *buf,size_t size,off_t offset);
   //Get the count of metadata items that are embedded in the image.
   size_t (*meta_count)(void *handle);
   //Get the index of specific top level metadata fields
   size_t (*meta_index)(void *handle,meta_id id);
   //Get the name of metadata with a specific index.
   char * (*meta_get_name)(void *handle,size_t index);
   //Get the type of metadata with a specific index.
   meta_type (*meta_get_type)(void *handle,size_t index);
   //Get the utf8/ascii representation of the metadata value with a specific index
   int (*meta_get_val)(void *handle,size_t index,char **metaval);
} carvpath_module_operations;


//A loadable module should export a function "carvpath_module_init" that uses the folowing
//function fingerprint. This function MUST fill all fields of the abouve structure.
typedef int (carvpath_module_initfunc)(carvpath_module_operations *ops);
#ifdef __cplusplus
}
#endif
#endif
