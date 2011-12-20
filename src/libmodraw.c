#define _GNU_SOURCE
#include "carvfsmod.h"
#include<stdio.h>
#include<sys/types.h>
#include<sys/stat.h>
#include<unistd.h>
#include<stdlib.h>
#include<fcntl.h>
#include<string.h>

#include<errno.h>


struct filebounds {
  int fd;
  off_t start;
  off_t end;
};

struct handleinfo {
  unsigned int nmbr_files;
  off_t totalsize;
  struct filebounds *fb;
};


void * raw_open_image(int pargc, const char *pargv[]) {

  struct stat buf;
  unsigned int i = 0;
  off_t total = 0;
  struct handleinfo *hi = malloc(sizeof(struct handleinfo));

  hi->totalsize = 0;
  hi->nmbr_files = pargc;
  hi->fb = malloc(pargc*sizeof(struct filebounds));

  for (i = 0; i < pargc; i++){
   
    if(!access(pargv[i], R_OK)) {
      if (stat(pargv[i], &buf) == 0){
	hi->fb[i].start = total;
	hi->fb[i].fd = open(pargv[i], O_RDONLY);
	hi->fb[i].end = hi->fb[i].start + buf.st_size - 1; 
	total = hi->fb[i].end + 1;
      } else {
	return 0;
      }
    } else {
      return 0;
    }
  }

  hi->totalsize = total;
  return hi;
}


size_t raw_read_random(void *handle,char *buf,size_t size,off_t offset) {
  struct filebounds *fb = ((struct handleinfo *)handle)->fb;
  unsigned int nmbr_files = ((struct handleinfo *)handle)->nmbr_files;
  unsigned int i = 0;
  size_t numread = 0;
  while (i < nmbr_files && size > numread){
    if (fb[i].end >= offset){
      if (fb[i].start < offset)
        lseek(fb[i].fd, offset - fb[i].start ,SEEK_SET);
      else lseek(fb[i].fd, 0, SEEK_SET);
      numread += read(fb[i].fd, &buf[numread], size - numread);
    }
    i++;
  }
  return numread;
}

off_t raw_data_size(void *handle){
  struct handleinfo *hi=(struct handleinfo *) handle; 
  if (hi->nmbr_files == 1) {
    struct filebounds *fb = hi->fb; 
    int lastfd=fb[0].fd;
    struct stat buf;
    fstat(lastfd,&buf);
    fb[0].end=buf.st_size-1;
    return buf.st_size;
  }
  return hi->totalsize;
}

void raw_close_image(void *handle){
  struct filebounds *fb = ((struct handleinfo *)handle)->fb;
  unsigned int i = 0;
  for (i = 0; i < ((struct handleinfo *)handle)->nmbr_files; i++){
    close(fb[i].fd);
  }
  free(fb);
  free(handle);
}


size_t raw_meta_count(void *handle) {
   return 0;
}

size_t raw_meta_index(void *handle,meta_id id){
    return 0;
}

char *raw_meta_get_name(void *handle,size_t index) {
   return 0;
}

int raw_meta_get_val(void *handle,size_t index,char **result) {
  *result=0;
  return 0;
}

meta_type raw_meta_get_type(void *handle,size_t index) {
  return METATYPE_LATIN;
}


int carvpath_module_init(struct carvpath_module_operations *ops) {
   ops->open_image=raw_open_image;
   ops->close_image=raw_close_image;
   ops->data_size=raw_data_size;
   ops->read_random=raw_read_random;
   ops->meta_count=raw_meta_count;
   ops->meta_index=raw_meta_index;
   ops->meta_get_name=raw_meta_get_name;
   ops->meta_get_type=raw_meta_get_type;
   ops->meta_get_val=raw_meta_get_val;
   return 1;
}













