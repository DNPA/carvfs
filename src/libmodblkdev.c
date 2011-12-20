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


struct handleinfo {
  int fd;
  off_t totalsize;
};


void * blkdev_open_image(int pargc, const char *pargv[]) {
  struct stat buf;
  unsigned int i = 0;
  off_t total = 0;
  int fd = open(pargv[0], O_RDONLY);
  if (fd == -1) {
     return 0;   
  }
  off_t totalsize=lseek(fd, 0,SEEK_END)+1;
  if (fd == (off_t) -1) {
     return 0;
  }
  struct handleinfo *hi = malloc(sizeof(struct handleinfo));
  if (hi == 0) {
     return 0;
  }
  hi->fd=fd;
  hi->totalsize=totalsize;
  return hi;
}


size_t blkdev_read_random(void *handle,char *buf,size_t size,off_t offset) {
  int fd = ((struct handleinfo *)handle)->fd;
  if (lseek(fd, offset,SEEK_SET) == (off_t) -1) {
     return 0;
  }
  return read(fd, buf, size);
}

off_t blkdev_data_size(void *handle){
  return ((struct handleinfo *)handle)->totalsize;
}

void blkdev_close_image(void *handle){
  int fd = ((struct handleinfo *)handle)->fd;
  close(fd);
  free(handle);
}


size_t blkdev_meta_count(void *handle) {
   return 0;
}

size_t blkdev_meta_index(void *handle,meta_id id){
    return 0;
}

char *blkdev_meta_get_name(void *handle,size_t index) {
   return 0;
}

int blkdev_meta_get_val(void *handle,size_t index,char **result) {
  *result=0;
  return 0;
}

meta_type blkdev_meta_get_type(void *handle,size_t index) {
  return METATYPE_LATIN;
}


int carvpath_module_init(struct carvpath_module_operations *ops) {
   ops->open_image=blkdev_open_image;
   ops->close_image=blkdev_close_image;
   ops->data_size=blkdev_data_size;
   ops->read_random=blkdev_read_random;
   ops->meta_count=blkdev_meta_count;
   ops->meta_index=blkdev_meta_index;
   ops->meta_get_name=blkdev_meta_get_name;
   ops->meta_get_type=blkdev_meta_get_type;
   ops->meta_get_val=blkdev_meta_get_val;
   return 1;
}













