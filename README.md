# direct-io

Direct IO helpers for block devices and regular files on FreeBSD, Linux, macOS
and Windows. *#mechanical-sympathy*

## Installation

```
npm install @ronomon/direct-io
```

## Direct Memory Access

Direct memory access bypasses the filesystem cache and avoids memory copies to
and from kernel-space, writing and reading directly to and from the disk device
cache. File I/O is done directly to and from user-space buffers regardless of
whether the file descriptor is a block device or regular file. To enable direct
memory access, use the following flag or method according to the respective
platform:

**O_DIRECT** *(FreeBSD, Linux)*

Provide as a flag to `fs.open()` when opening a block device or regular file to
enable direct memory access.

**setF_NOCACHE(fd, value, callback)** *(macOS)*

Turns data caching off or on for an open file descriptor for a block device or
regular file:

* A `value` of `1` turns data caching off (this is the equivalent of `O_DIRECT`
on macOS).
* A `value` of `0` turns data caching back on.

**Coming Soon** *(Windows)*

Please note that it is not yet possible to use Node to open a block device or
regular file for direct memory access on Windows. Please see
[Node issue 15433](https://github.com/nodejs/node/issues/15433) and
[Libuv issue 1550](https://github.com/libuv/libuv/issues/1550) for more
information. I will add support for direct memory access on Windows as soon as
these issues are resolved.

## Buffer Alignment

When writing or reading to and from a block device or regular file using direct
memory access, you need to make sure that your buffer is aligned correctly or
you may receive an `EINVAL` error or be switched back silently to non-DMA mode.

To be aligned correctly, the address of the allocated memory must be a multiple
of `alignment`, i.e. the physical sector size (not logical sector size) of the
block device. Buffers allocated using Node's `Buffer.alloc()` and related
methods will typically not meet these alignment requirements. Use
`getAlignedBuffer()` to create properly aligned buffers:

**getAlignedBuffer(size, alignment)** *(FreeBSD, Linux, macOS, Windows)*

Returns an aligned buffer:

* `size` must be greater than 0.
* `alignment` must be greater than 0, a power of two, and a multiple of the
physical sector size of the block device (typically 512 bytes or 4096 bytes).
* An alignment of 4096 bytes should be compatible with
[Advanced Format](https://en.wikipedia.org/wiki/Advanced_Format) drives as well
as backwards compatible with 512 sector drives. If you want to be sure, you
should use `getBlockDevice()` below to get the actual physical sector size of
the block device.
* Buffers are allocated using the appropriate C++ call (either
[`posix_memalign`](http://man7.org/linux/man-pages/man3/posix_memalign.3.html)
or [`_aligned_malloc`](https://docs.microsoft.com/en-us/cpp/c-runtime-library/reference/aligned-malloc)
depending on the platform).
* Buffers are not zero-filled. The contents of newly created aligned Buffers are
unknown and *may contain sensitive data*.
* Buffers are automatically freed using the appropriate C++ call when garbage
collected in V8 (either `free()` or `_aligned_free()` depending on the
platform).

Further reading:

* [MSDN Windows File Buffering](https://msdn.microsoft.com/en-us/library/windows/desktop/cc644950.aspx)

## Synchronous Writes

Direct Memory Access will write directly to the disk device cache but not
necessarily to the disk device storage medium. To ensure that your data is
flushed from the disk device cache to the disk device storage medium, you should
also open the block device or regular file using the `O_DSYNC` or `O_SYNC`
flags. These are the equivalent of calling `fs.fdatasync()` or `fs.fsync()`
respectively after every write. Please note that some file systems have had bugs
in the past where calling `fs.fdatasync()` or `fs.fsync()` on a regular file
would only force a flush if the page cache is dirty, so that bypassing the page
cache using `O_DIRECT` means the disk device cache is never actually flushed.

**O_DSYNC** *(FreeBSD, Linux, macOS)*

Flushes all data and only required associated metadata to the underlying
hardware.

**O_SYNC** *(FreeBSD, Linux, macOS)*

Flushes all data and any associated metadata to the underlying hardware.

To understand the difference between `O_DSYNC` and `O_SYNC`, consider two pieces
of file metadata: the file modification timestamp and the file length. All write
operations will update the file modification timestamp, but only writes that add
data to the end of the file will change the file length. The last modification
timestamp is not needed to ensure that a read completes successfully, but the
file length is needed. Thus, `O_DSYNC` will only flush updates to the file
length metadata, whereas `O_SYNC` will also flush the file modification
timestamp metadata.

**Coming Soon** *(Windows)*

Please note that it is not yet possible to use Node to open a block device or
regular file for synchronous writes on Windows. Please see
[Node issue 15433](https://github.com/nodejs/node/issues/15433) and
[Libuv issue 1550](https://github.com/libuv/libuv/issues/1550) for more
information. I will add support for synchronous writes on Windows as soon as
these issues are resolved.

## Block Device Size and Sector Sizes

Please note that `fs.fstat()` will not work at all on Windows for a block
device, and will not report the correct size for a block device on other
platforms. You should use `getBlockDevice()` instead:

**getBlockDevice(fd, callback)** *(FreeBSD, Linux, macOS, Windows)*

Returns an object with the following properties:

* `logicalSectorSize` - The size of a logical sector in bytes. Many drives
advertise a backwards compatible logical sector size of 512 bytes when in fact
their physical sector size is 4096 bytes.

* `physicalSectorSize` - The size of a physical sector in bytes. You should use
this as the `alignment` parameter when getting aligned buffers so that reads and
writes are always a multiple of the physical sector size.

* `size` - The total size of the block device in bytes.

## Opening a Block Bevice

You will need sudo or administrator privileges to open a block device. You can
use `fs.open(path, flags)` to open a block device, where the path you provide
will depend on the platform:

`/dev/sda` *(FreeBSD, Linux)*

`/dev/disk1` *(macOS)*

`\\.\PhysicalDrive1` *(Windows)*

You can use these shell commands to see which block devices are available:

`$ camcontrol devlist` *(FreeBSD)*

`$ lsblk` *(Linux)*

`$ diskutil list` *(macOS)*

`$ wmic diskdrive list brief` *(Windows)*

## Locking a Block Device

Windows has
[special restrictions](https://support.microsoft.com/en-us/help/942448/changes-to-the-file-system-and-to-the-storage-stack-to-restrict-direct)
concerning writing to block devices. You must lock the block device using
`setFSCTL_LOCK_VOLUME()` immediately after opening the file descriptor. On other
platforms it is good practice to lock the block device by providing either the
`O_EXCL` or `O_EXLOCK` flag when opening:

**O_EXCL** *(Linux)*

Provide as a flag to `fs.open()` when opening a block device to obtain an
exclusive mandatory (and not just advisory) lock. When opening a regular file,
behavior is undefined. In general, the behavior of `O_EXCL` is undefined if it is used without
`O_CREAT`. There is one exception: on Linux 2.6 and later, `O_EXCL` can be used
without `O_CREAT` if the path refers to a block device. If the block device is
in use by the system e.g. if it is mounted, `fs.open()` will fail with an
`EBUSY` error.

**O_EXLOCK** *(macOS)*

Provide as a flag to `fs.open()` when opening a block device or regular file to
obtain an exclusive mandatory (and not just advisory) lock. When opening a
regular file, `fs.open()` will block until any existing lock is released. While
adding `O_NONBLOCK` can avoid this, it also introduces other IO semantics. Using
`O_EXLOCK` should therefore be limited to opening a block device.

**setFSCTL_LOCK_VOLUME(fd, value, callback)** *(Windows)*

Locks a block device on Windows if not in use:

* A `value` of `1` locks the block device using
[FSCTL_LOCK_VOLUME](https://msdn.microsoft.com/en-us/library/windows/desktop/aa364575.aspx).
* A `value` of `0` unlocks the block device using
[FSCTL_UNLOCK_VOLUME](https://msdn.microsoft.com/en-us/library/windows/desktop/aa364814.aspx)
if it was previously locked.
* A locked block device can be accessed only through the file descriptor that
locked it.
* A locked block device remains locked until the application unlocks the block
device, or until the file descriptor is closed, either directly through
`fs.close()`, or indirectly when the process terminates.
* If the specified block device is a system volume or contains a page file, the
operation will fail.
* If there are any open files on the block device, the operation will fail.
Conversely, success of this operation indicates that there are no open files.
* The system will flush all cached data to the block device before locking it.
* The NTFS file system treats a locked block device as a dismounted volume. The
`FSCTL_DISMOUNT_VOLUME` control code functions similarly but does not check for
open files before dismounting.
* Without a successful lock operation, a dismounted volume may be remounted by
any process at any time.
