//Fuse carvpath filesystem.
//Copyright (C) KLPD 2006  <ocfa@dnpa.nl>
//
//This library is free software; you can redistribute it and/or
//modify it under the terms of the GNU Lesser General Public
//License as published by the Free Software Foundation; either
//version 2.1 of the License, or (at your option) any later version.
//
//This library is distributed in the hope that it will be useful,
//but WITHOUT ANY WARRANTY; without even the implied warranty of
//MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
//Lesser General Public License for more details.
//
//You should have received a copy of the GNU Lesser General Public
//License along with this library; if not, write to the Free Software
//Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307  USA
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <libcarvpath.h>
#include "carvfsmod.h"
#include <fuse.h>
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <dlfcn.h>
#include <inttypes.h>
#include <syslog.h>
#define DIGEST_SIZE 32
#define PREFIX_SIZE ( 8 )
#define PATH_EXTENTION_SIZE 4
#define FILE_PATH_OVERHEAD_SIZE ( PREFIX_SIZE + PATH_EXTENTION_SIZE )
#define MIN_VALID_FRAGSTRING_SIZE 2
#define MIN_CRV_FILEPATH_SIZE  ( FILE_PATH_OVERHEAD_SIZE + MIN_VALID_FRAGSTRING_SIZE )

static carvpath_entity *carvpath_top;
static char topdd_path[40];
static char readme_path[50];
static char *mountpath;
static char readme[8192];
static char *md5;

static void *carvfs_handle=0;  //A global variable holding the opaque image handle returned 
                               //when opening the image with the loadable module.

struct carvpath_module_operations *cp_ops;

//No utime on a read only filesystem.
static int carvfs_utime(const char *path, struct utimbuf *curtime)
{
    syslog(LOG_INFO,"utime called on read only filesystem\n");
    return -EPERM;
}
//No mknod on a read only filesystem.
static int carvfs_mknod(const char *path, mode_t mode, dev_t device)
{
    syslog(LOG_INFO,"mknod called on read only filesystem\n");
    return -EPERM;
}
//No truncating on a read only filesystem.
static int carvfs_truncate (const char *path, off_t offset){
    syslog(LOG_INFO,"truncate called on read only filesystem\n");
    return -EPERM;
}

static int carvfs_readlink(const char *path,char *buf,size_t size) {
   int res = 0; //The default return value is 0 to indicate all is OK.
   memset(buf, 0, size); //Clear the buffer that we need to write to.
   int failure=0;//No failures yet.
   char *toppath= carvpath_get_as_path(carvpath_top); //Get the path of our top entity.
   if (toppath == 0) {
       syslog(LOG_ERR,"Problem fetching top_path : %s\n",carvpath_error_as_string(errno));
       return -EFAULT;
   }
   //All symlinks should be relative to the toplevel path /CarvFS 
   if ((strncmp(path, toppath,strlen(toppath)) == 0) && (path[strlen(toppath)] == '/')) {
        char *basecarvpath=(char *)(path+PREFIX_SIZE); //Strip the prefix to get the realtive carvpath.
        int dofree=0; //Nothing to free initialy
        int regfile=0; //By default we asume its a directory.
        //If we have a '.crv' extention, we need to strip this for libcarvpath usage.
        if (strncmp(path+strlen(path) -PATH_EXTENTION_SIZE,".crv",PATH_EXTENTION_SIZE)==0) {
	   //Create a new basecarvpath without .crv extension.
           basecarvpath=0;
           basecarvpath=calloc(strlen(path) - FILE_PATH_OVERHEAD_SIZE + 1,1); 
           strncpy(basecarvpath,path+PREFIX_SIZE,strlen(path) - FILE_PATH_OVERHEAD_SIZE );
	   //Given that we have just malloced a new basecarvpath, we should remember to free it.
           regfile=dofree=1;
	   //Parse the bare relative carvpath.
           carvpath_entity *parsedpath=carvpath_parse(carvpath_top,basecarvpath,CARVPATH_OOR_FAIL);
           if (parsedpath) {
	      //Fetch the flattened carvpath from the entity and create an absolute path from it.
	      if (snprintf(buf,size,"%s/%s.crv",mountpath,carvpath_get_as_path(parsedpath)) >= size) {
                  syslog(LOG_WARNING,"readlink supplied buffer insuficient for carvpath: %s'\n",carvpath_get_as_path(parsedpath));
                  res=-EINVAL;
	      }
              if (failure) {
                 syslog(LOG_ERR,"Problem freeing carvpath entity : %s\n",carvpath_error_as_string(errno));
              }
              carvpath_free(parsedpath,&failure);
              if (failure) {
                  syslog(LOG_ERR,"Problem freeing parsedpath entity: %s\n",carvpath_error_as_string(errno));
              }
           } else {
             syslog(LOG_INFO,"Readlink on unparsable path: %s : %s\n",basecarvpath,carvpath_error_as_string(errno));
             res = -ENOENT;
           }
        } else {
	  //Only flatenable 'file' paths can be softlinks. Directories can not.
          syslog(LOG_INFO,"readlink on non '.crv' path: '%s'\n",basecarvpath);
          res = -ENOENT;
        }
	//Free the basecarvpath if needed.
        if (dofree) {
           free(basecarvpath);
        }
   } else {
      syslog(LOG_INFO,"readling on non matching sub path: '%s' relative to '%s'\n",path,toppath);
      res = -ENOENT;
   }
   return res;
}

//This function basically does a stat on the file or directory.
static int carvfs_getattr(const char *path, struct stat *stbuf)
{
    int res = 0;     //The default return value is 0 to indicate all is OK.    
    int failure=0;   //No failures yet.
    memset(stbuf, 0, sizeof(struct stat));  //Start of with an empty zeroed out stat buffer.
    //The readable root directory
    char *toppath= carvpath_get_as_path(carvpath_top);
    if (toppath == 0) {
       syslog(LOG_ERR,"Problem fetching top_path : %s\n",carvpath_error_as_string(errno));
       return -EFAULT;
    }
    if(strcmp(path, "/") == 0) {
	//The top directory is a simple worls readable directory.
        stbuf->st_mode = S_IFDIR | 0755;
        stbuf->st_nlink = 3;
    }
    //The full image as raw data file
    else if(strcmp(path, topdd_path) == 0) {
	//The topdd is a simple world readable file.
        stbuf->st_mode = S_IFREG | 0444;
        stbuf->st_nlink = 1;
        stbuf->st_size = carvpath_get_size(carvpath_top,&failure); //The file size is the net image size.
        if (failure) {
           syslog(LOG_ERR,"Problem fetching size from carvpath_top : %s\n",carvpath_error_as_string(errno));
           return -EFAULT;
        }
	//Validate the image hasn't grown (in the case of a growable archive.
        off_t realsize=cp_ops->data_size(fuse_get_context()->private_data);
        if (realsize > stbuf->st_size) {
	   //Update the carvpath top node size.
           syslog(LOG_NOTICE,"getattr: archive file has grown, growing top entity to match.\n");
           carvpath_grow_top(carvpath_top,realsize,&failure);
           if (failure) {
              syslog(LOG_ERR,"Problem growing top carvpath : %s\n",carvpath_error_as_string(errno));
              return -EFAULT;
           } else { 
              stbuf->st_size = realsize;
           }
        }    
	//Determine the number of blocks this file will take.
        stbuf->st_blocks = ((stbuf->st_size + 511) /512);
    }
    //A readme file with some basic documentation of usage of CarvFS.
    else if(strcmp(path, readme_path) == 0) {
	  //The README is a simple regular read only file.
          stbuf->st_mode = S_IFREG | 0444;
          stbuf->st_nlink = 1;
          stbuf->st_size = strlen(readme);
          stbuf->st_blocks = ((stbuf->st_size + 511) /512);
    }
    //The top directory for CarvFS operations.
    else if (strcmp(path, toppath) == 0) {
	//The CarvPath operations top dir is a basic directory without r/w rights.
        stbuf->st_mode = S_IFDIR | 0111;
	stbuf->st_nlink = 3;
    }
    //Within the CarvFS carvpath space 
    else if ((strncmp(path, toppath,strlen(toppath)) == 0) && (path[strlen(toppath)] == '/')) {
        //Use the relative carvpath path.
        char *basecarvpath=(char *)(path+PREFIX_SIZE);
        int dofree=0;  //So far there is nothing to free.
        int regfile=0; 
        int needslink=0; 
        //If we have a '.crv' extention, we need to strip this for libcarvpath usage.
        if (strncmp(path+strlen(path) -PATH_EXTENTION_SIZE,".crv",PATH_EXTENTION_SIZE)==0) {
           basecarvpath=0;
           basecarvpath=calloc(strlen(path) - FILE_PATH_OVERHEAD_SIZE + 1,1); 
           strncpy(basecarvpath,path+PREFIX_SIZE,strlen(path) - FILE_PATH_OVERHEAD_SIZE );
           regfile=dofree=1; //We did a calloc so we need to do a free later on.
        }
	//Parse the relative path.
        carvpath_entity *parsedpath=carvpath_parse(carvpath_top,basecarvpath,CARVPATH_OOR_FAIL);
        if (parsedpath == 0) {
           syslog(LOG_INFO,"Problem parsing '%s' : %s : maybe we need to grow first.\n",basecarvpath,carvpath_error_as_string(errno));
           struct stat statdata;
	   //In case of parse failure, make sure we don't need to grow the top entity.
           carvfs_getattr(topdd_path, &statdata);
           syslog(LOG_INFO,"get_attr: Invoking carvpath_parse ones more with string '%s'\n",basecarvpath);
	   //Try a second time to parse the relative carvpath.
           parsedpath=carvpath_parse(carvpath_top,basecarvpath,CARVPATH_OOR_FAIL);
           if (parsedpath == 0) {
               syslog(LOG_INFO,"No, that didn't work! '%s' : %s\n",basecarvpath,carvpath_error_as_string(errno));
	       return -ENOENT;
           }
        }
        //dont know why, but we need to fetch our size over here.
        off_t filesize=0;
        if (parsedpath) {
            filesize=carvpath_get_size(parsedpath,&failure);
            if (failure) {
                syslog(LOG_ERR,"Unable to get size from entity '%s' : %s\n",basecarvpath,carvpath_error_as_string(errno));
                carvpath_free(parsedpath,&failure);
                if (failure) {
                     syslog(LOG_ERR,"Problem freeing entity: %s\n",carvpath_error_as_string(errno));
                }
                return -EFAULT;
            }
        }
	//Calculate our blockcount.
        off_t blockcount=((stbuf->st_size +511) /512);
	//Determine if we need to return as a symlink. We want a symlink if we can express the carvpath in a shorter way.
        if (regfile) {
           if (parsedpath) {
              if (strlen(carvpath_get_as_path(parsedpath)) < (strlen(basecarvpath) + strlen("/CarvFS/") )) {
                 needslink=1;
	      } 
           }
        }
        if (parsedpath) {
            //Successfully parsed carvpath
            if (needslink) {
	       //We are a symlink to a shorter notation.
               stbuf->st_mode = S_IFLNK | 0444;
               stbuf->st_nlink = 1;
               stbuf->st_size = 4096;
               stbuf->st_blocks = 1;
            } else if (regfile) {
	       //We are a simple regular file.
               stbuf->st_mode = S_IFREG | 0444;
	       stbuf->st_nlink = 1;
	       stbuf->st_size = filesize;
               stbuf->st_blocks = blockcount;;
            } else {
	       //We are a sub directory.
               stbuf->st_mode = S_IFDIR | 0111;
	       stbuf->st_nlink = 3;
            }	
            carvpath_free(parsedpath,&failure); 
            if (failure) {
               syslog(LOG_ERR,"Problem freeing entity: %s\n",carvpath_error_as_string(errno));	 
            }   
	} else {
            //Invalid carvpath failed to parse.
            syslog(LOG_INFO,"getattr problem parsing path: '%s'\n",basecarvpath);    
            res = -ENOENT;
	}
        if (dofree)
               free(basecarvpath);
    }
    //Other entities do not exist. 
    else {
       syslog(LOG_INFO,"getattr request for non existing path: '%s'\n",path);
       res = -ENOENT;
    }
    return res;
}

//Readdir for just the readable '/' directory, other dirs are non readable.
static int carvfs_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
                         off_t offset, struct fuse_file_info *fi)
{
    (void) offset;
    (void) fi;

    if(strcmp(path, "/") != 0) {
	int fileok=0; //By default assume the directory does not excist.
	if (strcmp(path,"/CarvFS") == 0) { //If the directory is the top level carvpath dir, thats ok
          fileok=1;
	}
	else if (strncmp(path,"/CarvFS/",8) == 0) { //Other valid dirs only exist under the top level carvpath dir.
           char *relpath=path+8;
           carvpath_entity *parsedpath=carvpath_parse(carvpath_top,relpath,CARVPATH_OOR_FAIL);
	   if (parsedpath) { //If the relative path is a parsable carvpath, than the dir exists.
             carvpath_free(parsedpath,0);
	     fileok=1;
	   }
	}
        if (fileok) {
	  //Existing dirs don't show any files or directories, they exist by virtue of designation.
          filler(buf, ".", NULL, 0);
	  filler(buf, "..", NULL, 0);
	  return 0;
	} else {	
          syslog(LOG_INFO,"readdir for non existing path: '%s'\n",path);
          return -ENOENT;
	}
    }
    //The CarvFS top dir holds some entities.
    char *toppath= carvpath_get_as_path(carvpath_top);
    if (toppath == 0) {
        syslog(LOG_ERR,"Problem getting top entity as path: %s\n",carvpath_error_as_string(errno));
        return -EFAULT;
    }
    filler(buf, ".", NULL, 0);
    filler(buf, "..", NULL, 0);
    filler(buf, topdd_path + 1, NULL, 0); //The full image as a raw dd.
    filler(buf, toppath + 1, NULL, 0);    //The pseudo dir for carvpaths.
    filler(buf, readme_path + 1, NULL, 0); //A simple readme file.
    return 0;
}

static int carvfs_open(const char *path, struct fuse_file_info *fi)
{
    int okrval=0;
    int failure=0;
    //Files need to be opened read only, we are a read only filesystem.
    if((fi->flags & 3) != O_RDONLY){
        syslog(LOG_WARNING,"open did not specify O_RDONLY while node is read only: '%s'\n",path);
	okrval=-EACCES;
    }
    //the readme file
    if(strcmp(path, readme_path) == 0)
       return 0;
    //the top level file for the comlete content as raw image.
    if(strcmp(path, topdd_path) == 0) 
	return okrval;
    if (strlen(path) < MIN_CRV_FILEPATH_SIZE) {
        syslog(LOG_INFO,"open on path with a to small length to be valid: '%s'\n",path);
	return -ENOENT;
    }
    //Other files can only be opened if they are under the CarvFS carvpath hyrarchy.
    char *toppath=  carvpath_get_as_path(carvpath_top);
    if (toppath == 0) {
        syslog(LOG_ERR,"Problem getting top entity as path: %s\n",carvpath_error_as_string(errno));
        return -EFAULT;
    }
    if (strncmp(path,toppath,strlen(toppath))) {
            syslog(LOG_WARNING,"open on filename outside of the toppath scope: '%s' not part of '%s'\n",path,toppath);
	    return -ENOENT;
    }
    //Use relatice carvpath path.
    char *basecarvpath=(char *)(path+PREFIX_SIZE);
    int dofree=0;
    //Only files can be opened, directories can not.
    if (strncmp(path+strlen(path) -PATH_EXTENTION_SIZE,".crv",PATH_EXTENTION_SIZE)==0) {
       //strip the '.crv' extention for usage with libcarvpath
       basecarvpath=calloc(strlen(path) - FILE_PATH_OVERHEAD_SIZE + 1,1); 
       strncpy(basecarvpath,path+PREFIX_SIZE,strlen(path) - FILE_PATH_OVERHEAD_SIZE);
       dofree=1;
    } else {
       syslog(LOG_INFO,"open no permission on non .crv entity: '%s'\n",path);
       return -EPERM;
    }
    //see if libcarvpath can parse the relative path.
    carvpath_entity *parsedpath=carvpath_parse(carvpath_top,basecarvpath,CARVPATH_OOR_FAIL);
    if (parsedpath == 0) {
        syslog(LOG_INFO,"Problem parsing carvpath '%s': %s\n",basecarvpath,carvpath_error_as_string(errno));
    }
    if (dofree)
	    free(basecarvpath);
    //we free the parsed libcarvpath path, this will NOT set parsedpath to NULL so we can still check
    carvpath_free(parsedpath,&failure);
    if (failure) {
        syslog(LOG_ERR,"Problem freeing parsedpath entity: %s\n",carvpath_error_as_string(errno));
    }
    if (parsedpath)
	    return 0;
    syslog(LOG_INFO,"open unparsable carvpath : '%s'\n",path);
    return -ENOENT;
}
//No writing to a read only filesystem.
static int carvfs_write(const char *path,const char *buf, size_t size, off_t offset,
                      struct fuse_file_info *fi)
{
    return -EPERM;
}

//This function reads a chunk of data from a file.
static int carvfs_read(const char *path, char *buf, size_t size, off_t offset,
                      struct fuse_file_info *fi)
{
    //There are 3 different types of file we need to handle, the readme file, 
    //the full image dd and files with a valid carvpath and '.crv' extension.
    int failure=0;
    char *fakedata=0;
    if(strcmp(path,readme_path) == 0) {
       fakedata=readme;
    }
    if (fakedata) {
        size_t remaining=strlen(fakedata) - offset;
        if (remaining < 0) {
	    //No read past end of file
            return 0;
        }
        size_t fetchsize=size; //Read at most the indicated amounth of bytes.
        if (remaining < fetchsize)  //If there are less bytes remaining than requested, read those remaining.
            fetchsize=remaining;
        strncpy(buf,fakedata+offset,fetchsize); //Copy the relevant bytes from our readme data string.
        return fetchsize; //Return the number of bytes read.
    }
    //The top raw data path maps directly to module lib
    if(strcmp(path,topdd_path) == 0) //If we read from the top level dd, use a direct mapping to the image.
        return cp_ops->read_random(fuse_get_context()->private_data,buf,size,offset); 
    //Check for CarvPath paths being to short to be valid.
    if (strlen(path) < MIN_CRV_FILEPATH_SIZE) {
        syslog(LOG_INFO,"read from entity with to small a path size '%s'\n",path);
        return -ENOENT; 
    }
    //Other paths need to be relative to the CarvFS top.
    char *toppath=  carvpath_get_as_path(carvpath_top);
    if (toppath == 0) {
       syslog(LOG_ERR,"Problem fetching top_path : %s\n",carvpath_error_as_string(errno));
       return -EFAULT;
    }
    //Check if the path is relative to the CarvFS pseudo dir.
    if (strncmp(path,toppath,strlen(toppath))) {
        syslog(LOG_WARNING,"read from entity '%s' not within top path '%s'\n",path,toppath);
	return -ENOENT;
    }
    //Check if the file has a proper '.crv' suffix.
    if (strncmp(path+strlen(path) -PATH_EXTENTION_SIZE,".crv",PATH_EXTENTION_SIZE)){
        syslog(LOG_WARNING,"read from non .crv entity '%s'\n",path);
	return -ENOENT;
    }
    //Create a relative extention less carvpath version of the fuse path.
    char *basecarvpath=calloc(strlen(path) - FILE_PATH_OVERHEAD_SIZE + 1,1);
    strncpy(basecarvpath,path+PREFIX_SIZE,strlen(path) - FILE_PATH_OVERHEAD_SIZE);
    //Try to parse the relative carvpath with libcarvpath.
    carvpath_entity *parsedpath=carvpath_parse(carvpath_top,basecarvpath,CARVPATH_OOR_FAIL);
    if (parsedpath == 0) {
        syslog(LOG_INFO,"read unable to parse path '%s' : %s\n",basecarvpath,carvpath_error_as_string(errno));
	return -ENOENT;
    }
    
    //The single chunk we want to read may actualy be more than one single chunk. We derive a new
    //carvpath entity from the parsed entity using the offset and size supplied by read.
    //We ask libcarvpath to truncate the entity as not to fail if the requested data chunk
    //overlaps with the logical end of file.
    carvpath_entity *readchunk=carvpath_derive(parsedpath,offset,size,CARVPATH_OOR_TRUNCATE);
    if (readchunk == 0) {
        syslog(LOG_INFO,"read unable to derive chunk path '%s' offset=%" PRId64 " size=%" PRId64 " :  %s\n",basecarvpath,offset,size,carvpath_error_as_string(errno));
	carvpath_free(parsedpath,&failure);
        if (failure) {
             syslog(LOG_ERR,"Problem freeing parsedpath entity: %s\n",carvpath_error_as_string(errno));
        }
        return -EFAULT;
    }
    //Now we need to flatten the derived entity in order for the fragments to directly map
    //to fragments on the lowest level that maps to module lib.
    //fprintf(stderr,"DEBUG: '%s'\n",carvpath_get_as_path(readchunk));
    carvpath_entity *readchunk_fragments=carvpath_flatten(readchunk);
    if (readchunk_fragments == 0) {
        syslog(LOG_WARNING,"read unable to flatten chunk path '%s' offset=%" PRId64 " size=%" PRId64 " : %s \n",basecarvpath,offset,size,carvpath_error_as_string(errno));
    }
    //fprintf(stderr,"DEBUG: '%s'\n",carvpath_get_as_path(readchunk_fragments));
    size_t fragcount=carvpath_get_fragcount(readchunk_fragments,&failure);
    if (failure) {
        syslog(LOG_WARNING,"read unable to fetch fragcount from flattened  chunk path '%s' offset=%" PRId64 " size=%" PRId64 ": %s \n",basecarvpath,offset,size,carvpath_error_as_string(errno));
    }
    size_t fragno;
    off_t rpoint=0;
    //Read all fragments into the read buffer. 
    //TESTME: we have not tested this yet on anything that returned other than a single fragment.
    //Although this code should work, we need to make a test for it yet.
    for (fragno=0;fragno < fragcount;fragno++) {
       //Get the relevant info (offset,size,type) from the fragment.
       off_t carvfs_offset =carvpath_fragment_get_offset(readchunk_fragments,fragno,&failure);
       if (failure) {
           syslog(LOG_ERR,"Problem getting offset from fragment: %s\n",carvpath_error_as_string(errno));
       }
       off_t carvfs_size   =carvpath_fragment_get_size(readchunk_fragments,fragno,&failure);
       if (failure) {
           syslog(LOG_ERR,"Problem getting size from fragment: %s\n",carvpath_error_as_string(errno));
       }
       size_t readcount=0;
       int issparse=carvpath_fragment_is_sparse(readchunk_fragments,fragno,&failure);
       if (failure) {
           syslog(LOG_ERR,"Problem getting type from fragment: %s\n",carvpath_error_as_string(errno));
       }
       if (issparse==0) {
	  //Fetch the data from the image and write it to the buffer.
          readcount = cp_ops->read_random(fuse_get_context()->private_data,buf+rpoint,carvfs_size,carvfs_offset);
       } else {
	 //Or fill the buffer section with zeroes when the fragment is indicated to be sparse.
         memset(buf+rpoint, 0, carvfs_size);
         readcount=carvfs_size;
       }
       if (readcount != carvfs_size) {
	  //The loadable module was unable to retreive the carvfs_size number if bytes at the carvfs_offset offset, this is a serious error in the loadable module and/or image file.
          syslog(LOG_WARNING,"read unable to derive chunk fragment for path '%s' offset=%" PRId64 " size=%" PRId64 " : fragno=%" PRId64 " fragsize=%" PRId64 " readcount=%" PRId64 "\n",basecarvpath,offset,size,fragno,carvfs_size,readcount);
          carvpath_free(readchunk_fragments,&failure);
          if (failure) {
             syslog(LOG_ERR,"Problem freeing readchunk_fragments entity: %s\n",carvpath_error_as_string(errno));
          }
          carvpath_free(readchunk,0);
          if (failure) {
             syslog(LOG_ERR,"Problem freeing readchunk entity: %s\n",carvpath_error_as_string(errno));
          }
          carvpath_free(parsedpath,0); 
          if (failure) {
             syslog(LOG_ERR,"Problem freeing parsedpath entity: %s\n",carvpath_error_as_string(errno));
          }   
	  return -EFAULT;
       }
       //Increment the pointer within our output buffer so the next chunk ends up at the proper place.
       rpoint+=readcount;
    }
    free(basecarvpath);
    carvpath_free(readchunk_fragments,&failure);
    if (failure) {
        syslog(LOG_ERR,"Problem freeing readchunk_fragments entity: %s\n",carvpath_error_as_string(errno));
    }
    carvpath_free(readchunk,&failure);
    if (failure) {
         syslog(LOG_ERR,"Problem freeing readchunk entity: %s\n",carvpath_error_as_string(errno));
    }
    carvpath_free(parsedpath,&failure);
    if (failure) {
         syslog(LOG_ERR,"Problem freeing parsedpath entity: %s\n",carvpath_error_as_string(errno));
    }
    return rpoint;
}

static void carvfs_destroy(void *handle) 
{
   //Invoke the close_image method provided by the loadable module on the image.
   cp_ops->close_image(handle); 
}

//The carvfs_init function returns a global. It should be possible to do this more cleanly in the future.
static void * carvfs_init(void) 
{
  return (void *) carvfs_handle;    
}

//This structure binds the filesystem callbacks to the above functions.
static struct fuse_operations carvfs_oper = {
    .getattr	= carvfs_getattr,
    .readdir	= carvfs_readdir,
    .open	= carvfs_open,
    .read	= carvfs_read,
    .readlink   = carvfs_readlink,
    .write	= carvfs_write,
    .utime      = carvfs_utime,
    .mknod      = carvfs_mknod,
    .truncate   = carvfs_truncate,
    .init       = carvfs_init,
    .destroy    = carvfs_destroy,
};

int main(int argc, char *argv[])
{
    mountpath=0;
    md5=0;
    int failure=0;
    int fuseargcount=1;
    char *fuseargv[6]; //Up to 6 arguments in the fuse arguments array.
    fuseargv[0]=argv[0];
    fuseargv[1]=argv[1];        //This might be a -d, otherwise it will be overwritten.
    char *imgtype=argv[2];      //Without -d, the 2nd argument is the image type.
    char *mpdigest=argv[3];     //Without -d, the 3rd argument is the unique id of the image.
    if (argc < 4) { //There should be at least 4 arguments to the command line.
      fprintf(stderr,"usage:\n carvfs [-d] <mountpoint> <imgtype> (auto|<digest>) <file> [<file> *]\n");
      fprintf(stderr,"     imgtype can be one of: \"raw\" or \"blkdev\"\n");
      fprintf(stderr,"     and if installed, \"ewf\"\n");
      exit(1);
    }
    if (argv[1][0] == '-') {
      //If the first argument starts with a minus, than we have the wrong imgtype and unique id of the image, and we will need to
      //take the 3rd and 4th argument rather than the 2nd and 3rd one.
      fuseargcount=2;
      imgtype=argv[3];
      mpdigest=argv[4];
    }
    //Create a new empty and zeroed out carvpath_module_operations structure.
    cp_ops=calloc(1,sizeof(carvpath_module_operations)); 
    //Determine the library to try and open as loadable module.
    char imgtypelib[80];
    sprintf(imgtypelib,"libmod%s.so",imgtype);
    //Try to load the loadable module.
    void *handle=dlopen(imgtypelib,RTLD_NOW);
    if (handle == 0) {
        fprintf(stderr,"unable to load image format library %s %s\n",imgtypelib,dlerror());
        exit(1);
    }
    //Fetch the initialisation function function pointer from the just loaded loadable module.
    carvpath_module_initfunc *initmodule=(carvpath_module_initfunc *) dlsym(handle, "carvpath_module_init");  
    if (initmodule ==0) {
       fprintf(stderr,"unable to resolve carvpath_module_init symbol for disk image format library\n");
       exit(1);
    }
    //Invoke the initialisation function for the loaded loadable module. This function should set the cp_ops fields to
    //proper values.
    (*initmodule)(cp_ops);  
    //Find the proper arguments to pass to the loaded loadable module from the commandline arguments.
    const char **pargv=(const char **)(argv+3+fuseargcount);
    int pargc=argc-3-fuseargcount;
    carvfs_handle=0;
    //Invoke the open_image function that was set by the loadable module.
    carvfs_handle=cp_ops->open_image(pargc,pargv);
    if (carvfs_handle == 0) {
      fprintf(stderr,"problem opening image file with module lib\n");
      return 1;
    }
    //Fetch the (initial) image size of the just opened image file ftom the loadable module.
    off_t imgsize= cp_ops->data_size(carvfs_handle);
    if (imgsize == 0) {
      fprintf(stderr,"module lib returned zero size for image\n");
      return 1;
    }
    if (imgsize < 0) {
      fprintf(stderr,"module lib returned invalid size for image : errno=%d\n",errno);
      return 1;
    }
    //If the unique id commandline argument specifies 'auto' than try to get the md5 of the image from the loadable module.
    if (strcmp(mpdigest,"auto")==0) {
      cp_ops->meta_get_val(carvfs_handle,cp_ops->meta_index(carvfs_handle,META_ID_MD5),&md5);
      mpdigest=md5;
    }
    if (mpdigest == 0) {
       fprintf(stderr,"module lib provided no digest metadata, and auto was given on the command line\n");
       return 1;
    }
    //Define the standard top level file names.
    sprintf(topdd_path,"/CarvFS.crv"); //Note should always remain <topcarvpath>.crv
    sprintf(readme_path,"/README");
    //In order to allow for reproducable tree, we let all images be mounted
    //giving the same base path, and using a subdir determined by the
    //digest in the ewf or aff file that gets created if needed.
    mountpath = malloc(strlen(argv[fuseargcount]) +  DIGEST_SIZE  + 4);
    sprintf(mountpath,"%s/%s",argv[fuseargcount],mpdigest);
    if (mkdir(mountpath,0755)== -1) {
      if (errno != EEXIST) {
         fprintf(stderr,"Unable to create missing mountpoint \"%s\"\n",mountpath);
         return 1;
      }
    }
    //Make the mountpoint and its parent dir world readable.
    chmod(mountpath,0755);
    chmod(argv[fuseargcount],0755);
    //Define the read content for pseudo files defined ar root level.
    sprintf(readme,"This file gives a short intro into the usage of CarvFS.\n\
CarvFS is a userspace filesystem that can map a fragment path that indicates\n\
a fragment or set of fragments within an ewf/aff/raw image to a virtual file.\n\
From the ./CarvFS/ subdir you can make a relative path to indicate any desired set of fragments\n\
on the mounted ewf/aff/raw image. Please note that files under the ./CarvFS/ are readable, but directories\n\
are not. This means that you can not use ls, but stat and any tool that works on a file will work just fine\n\
The syntax for these relative paths is quite simple. The '/' character\n\
denotes a level of entities, where further fragment paths can be defined relative to the\n\
entity defined left of the '/' character. The '_' character is used as a fragment seperator.\n\
You can define an entity to exist of multiple fragments of its parent entity this way.\n\
A single fragment is defined by <offset>+<size>.\n\
With the thus defined path syntax it becomes possible to use CarvFS to do zero-storage carving.\n\
If you want to addapt your carving tool to work with CarvFS, than you should look into using\n\
libcarvpath in your carving tool.\n");
    fuseargv[fuseargcount]="-o";     //After the argv[0] and optionaly a -d argument set some options:
    fuseargv[fuseargcount+1]="allow_other";  //* allow all users to access the filesystem.
    fuseargv[fuseargcount+2]="-s";           //Run the filesystem as single threaded.
    fuseargv[fuseargcount+3]=mountpath;      //Set the proper mountpoint for the filesystem.
    carvpath_library *cplib=carvpath_init(1,0); //Initialize the carvpath library. 
                                                //Use longtokendb support, dont run in windows compatibility mode.
    if (cplib == 0) {
       fprintf(stderr,"libcarvpath could not be initialized with longtokendb support: %s\n",carvpath_error_as_string(errno));
       return 1;
    }
    //Initialize our top level carvpath entity using the logical size of our image.
    carvpath_top=carvpath_top_entity(imgsize,"/CarvFS",cplib);
    if (carvpath_top == 0) {
       fprintf(stderr,"libcarvpath could not create top entity from %" PRId64 " /CarvFS. Possible problem with longtokendb: %s\n",imgsize,carvpath_error_as_string(errno));
       return 1;
    }
    //Return the just created mountpoint to the user.
    fprintf(stdout,"%s\n",mountpath);
    fflush(stdout);
    //Open the syslog facility to send our debug and error messages to.
    openlog("carvfs",LOG_PID | LOG_NDELAY, LOG_DAEMON);
    //Nor run the filesystem.
    int rval= fuse_main(4+fuseargcount, fuseargv, &carvfs_oper);
    //FIXME: this does not work.
    unlink(mountpath);
    //Free the top level carvpath entity.
    carvpath_free(carvpath_top,&failure);
    if (failure) {
        syslog(LOG_ERR,"Problem freeing top entity: %s\n",carvpath_error_as_string(errno));   
    }
    //Cleanly close the longtokendb.
    carvpath_finish(cplib);
    return rval;
}


