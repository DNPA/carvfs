CarvFS user space file-system for usage with zero-storage (in-place) carving tools. 

This product has been developed by the Open Computer Forensics Architecture project team of Dutch National Police Agency.
The project aims to facilitate the usage of Raw and Encase and Raw images in OCFA (or any other CF environment), 
without the need the export (copy out)  the data. While developed for usage within the OCFA framework, CarvFS is
a stand-alone filesystem that should proof usable with any zero-storage carving efford independent from the OCFA
framework.
 
CarvFs should minimize the storage needs while processing the evidence, and thus make efficient file carving possible.

INTRODUCTION

CarvFs consists of a program for overlaying a carvpath directory structure over a (forensic) disk image file. 
Before building the user space carvfs tools you will first need to install the following:

fuse:	http://fuse.sourceforge.net/
        User space file system. Possible this is already a part of your Linux distribution. Otherwise you have also to install the kernel headers.

libcarvpath:	http://carvpath.sourceforge.net/ The carv path library. 

PREPARATION
Please make sure that you have the fuse kernel module loaded:

  modprobe fuse

Create a mountpoint:

  mkdir /mnt/carvfs

MOUNTING

CarvFs is a modular system. The base installation only builds and installs the 'raw' and 'blkdev' module. 
Consult the carvpath sourceforge pages for other modules, including the carvfs-modewf module
that uses libewf to give carvfs access to EWF files.

If you have the ewf module installed, you should be able to mount an encase image with CarvFs
as follows:

  carvfs /mnt/carvfs ewf auto /home/kees/img.e01 /home/kees/img.e02 /home/kees/img.e03

For raw imade files, 'auto' won't be able to automatically determine the image digest, so you shall
need to supply it on the command line. You can determine the digest of a raw dd file using
the command :

  sha1sum /home/kees/img2.dd

If you don't want to use real digests, nothing is forcing you to do so, a simple do-it-yourself enumerator
will do just fine:

  carvfs /mnt/carvfs raw 0 /home/kees/img.e01

After mounting, the /mnt/carvfs/<digest> directory should contain the following entries:
CarvFS.crv:	A raw image representation of the image.
CarvFS/:	The root for carvpath access to the image (see below)
README:	A short introduction on carvfs usage.

CarvPath usage:

The power of CarvFS lays in that it can provide zero-storage carving possibilities. 
The way it does this is by representing hierarchical fragment lists as relative paths under the CarvFS/ subtree.
Thus if a forensic tool uses the same 'libcarvpath' library that CarvFS does, it should be able to do carving by reference rather than carving by copy-out. 
The tool will using libcarvpath simply output a list of paths that CarvFS can use to represent the user and/or other tools with virtual files that effectively 
take zero additional storage.

Now lets explain how the carvpath representation works

In carvpath representation each directory level represents a fragment list level. It is further important to note that in order to distinguish between fragment 
lists as 'intermediate level' and fragment lists as 'file', the file representation uses a file extension '.crv'. 
You could thus have a path where under CarvFS the top directory represents a partition, the second level directory represents a large chunk of unallocated space 
on filesystem that is on the partition, and the lowest level represents a potential file that a low level carving tool located on this unallocated space.
Different fragments in a carvpath level are separated using the underscore (_) character. A single fragment consists of an offset and a size separated by a plus (+).
It is important to note that older versions of LibCarvPath used the collon (:) instead of the plus sign, but for future system interoperation concerns with
MS-Windows platforms, the notation has been changed.
 
Next to normal fragments, many filesystems have the notion of sparse holes. 
Sparse holes are represented in a carvpath as the size of the sparse hole followed prefixes with an 'S'.

Examples
Thus a partition could for example be addressed as:
CarvFS/16384+128303104.crv

A bunch of fragments on this partition as:
CarvFS/16384+128303104/8192+1024_12228_1024.crv

If on these a file would be located:
CarvFS/16384+128303104/8192+1024_12228_1024/0+1944.crv

Or a file with a big sparse holes could be there instead:
CarvFS/16384+128303104/8192+1024_12228+1024/0+1024_S14336_1024+712.crv

All of the abouve 'files' will be available for use by other tools that expect files to work on, but non would require any storage on top of the storage 
used by the image files.
 
