# direct-io

Direct IO helpers for block devices and regular files on FreeBSD, Linux, macOS
and Windows. *#mechanical-sympathy*

* [Installation](#installation)
* [Direct memory access](#direct-memory-access)
* [Buffer alignment](#buffer-alignment)
* [Synchronous writes](#synchronous-writes)
* [Block device size and sector size](#block-device-size-and-sector-size)
* [Block device path and permissions](#block-device-path-and-permissions)
* [Mandatory locks](#mandatory-locks)
* [Advisory locks](#advisory-locks)
* [Benchmark](#benchmark)

## Installation

```
npm install @ronomon/direct-io
```

## Direct Memory Access

Direct memory access bypasses the filesystem cache and avoids memory copies to
and from kernel space, writing and reading directly to and from the disk device
cache. File I/O is done directly to and from user space buffers regardless of
whether the file descriptor is a block device or regular file. To enable direct
memory access, use the following open flag or method according to the platform:

**O_DIRECT** *(FreeBSD, Linux, Windows)*

Provide as a flag to `fs.open()` when opening a block device or regular file to
enable direct memory access.

On Windows, `O_DIRECT` is supported as of
[libuv 1.16.0](https://github.com/libuv/libuv/commit/4b666bd2d82a51f1c809b2703a91679789c1ec01)
(i.e. Node 9.2.0 and up), where `O_DIRECT` is mapped to
[FILE_FLAG_NO_BUFFERING](https://msdn.microsoft.com/en-us/library/windows/desktop/cc644950(v=vs.85).aspx).

**setF_NOCACHE(fd, value, callback)** *(macOS)*

Turns data caching off or on for an open file descriptor for a block device or
regular file:

* A `value` of `1` turns data caching off (this is the nearest equivalent of
`O_DIRECT` on macOS).
* A `value` of `0` turns data caching back on.

Turning data caching off with `F_NOCACHE` [will not purge any previously cached
pages](https://lists.apple.com/archives/filesystem-dev/2007/Sep/msg00012.html).
Subsequent direct reads will continue to return cached pages if they exist, and
concurrent processes may continue to populate the cache through non-direct
reads. To ensure direct reads on macOS (for example when data scrubbing) you
should set `F_NOCACHE` as soon as possible to avoid populating the cache.

Alternatively, if you want to ensure initial boot conditions with a cold disk
buffer cache, you can purge the entire cache for all files using `sudo purge`.
This will affect system performance.

## Buffer Alignment

When writing or reading to and from a block device or regular file using direct
memory access, you need to make sure that your buffer is aligned correctly or
you may receive an `EINVAL` error or be switched back silently to non-DMA mode.

To be aligned correctly, the address of the allocated memory must be a multiple
of `alignment`, i.e. the physical sector size (not logical sector size) of the
block device. Buffers allocated using Node's `Buffer.alloc()` and related
methods will not meet these alignment requirements. Use `getAlignedBuffer()` to
create aligned buffers:

**getAlignedBuffer(size, alignment)** *(FreeBSD, Linux, macOS, Windows)*

Returns an aligned buffer:

* `size` must be greater than 0, and a multiple of the physical sector size of
the block device (typically 512 bytes or 4096 bytes).
* `size` must be at most `require('buffer').kMaxLength` bytes.
* `alignment` must be at least 8 bytes for portability between 32-bit and 64-bit
systems.
* `alignment` must be a power of two, and a multiple of the physical sector size
of the block device.
* `alignment` must be at most 4194304 bytes for a safe arbitrary upper bound.
* An alignment of 4096 bytes should be compatible with
[Advanced Format](https://en.wikipedia.org/wiki/Advanced_Format) drives as well
as backwards compatible with 512 sector drives. If you want to be sure, you
should use `getBlockDevice()` below to get the actual physical sector size of
the block device.
* Buffers are instances of Node's `Buffer` class and have the same methods and
properties, except that they are also aligned.
* Buffers are allocated using the appropriate call (either
[`posix_memalign`](http://man7.org/linux/man-pages/man3/posix_memalign.3.html)
or [`_aligned_malloc`](https://docs.microsoft.com/en-us/cpp/c-runtime-library/reference/aligned-malloc)
depending on the platform).
* Buffers are zero-filled with `memset()` when allocated for safety.
* Buffers are automatically freed using the appropriate call when garbage
collected in V8, either `free()` or `_aligned_free()` depending on the
platform.
* `getAlignedBuffer()` should be used judiciously as the algorithm that realizes
the alignment constraint can incur significant memory overhead.

Further reading:

* [MSDN Windows File Buffering](https://msdn.microsoft.com/en-us/library/windows/desktop/cc644950.aspx)

## Synchronous Writes

Direct memory access will write directly to the disk device cache but not
necessarily to the disk device storage medium. To ensure that your data is
flushed from the disk device cache to the disk device storage medium, you should
also open the block device or regular file using the `O_DSYNC` or `O_SYNC`
open flags. These are the equivalent of calling `fs.fdatasync()` or `fs.fsync()`
respectively after every write, but with less system call overhead, and with the
advantage that they encourage the disk device to do real work during the
`fs.write()` call, which can be useful when overlapping compute-intensive work
with IO.

Some systems implement the `O_DSYNC` and `O_SYNC` open flags by setting the
Force Unit Access (FUA) flag, which works for SCSI
[but](https://perspectives.mvdirona.com/2008/04/disks-lies-and-damn-disks/)
[not](https://blogs.msdn.microsoft.com/oldnewthing/20170510-00/?p=95505) for
EIDE and SATA drivers.

Conversely, some systems have had bugs where calling `fs.fdatasync()`
or `fs.fsync()` on a regular file would force a flush only if the page cache was
dirty, so that bypassing the page cache using `O_DIRECT` meant the disk device
cache was never flushed.

This means that the `O_DSYNC` and `O_SYNC` open flags are not sufficient on
their own, but should be combined with `fs.fdatasync()` or `fs.fsync()` for
durability or write barriers. This does not mean that these open flags are not
useful. As we have already seen, they can reduce the latency of the eventual
fsync call, eliminating latency spikes.

**O_DSYNC** *(FreeBSD, Linux, macOS, Windows)*

Flushes all data and only required associated metadata to the underlying
hardware.

**O_SYNC** *(FreeBSD, Linux, macOS, Windows)*

Flushes all data and any associated metadata to the underlying hardware.

To understand the difference between `O_DSYNC` and `O_SYNC`, consider two pieces
of file metadata: the file modification timestamp and the file length. All write
operations will update the file modification timestamp, but only writes that add
data to the end of the file will change the file length. The last modification
timestamp is not required to ensure that the data can be read back successfully,
but the file length is needed. Thus, `O_DSYNC` will only flush updates to the
file length metadata, whereas `O_SYNC` will also flush the file modification
timestamp metadata.

On Windows, synchronous writes are supported as of
[libuv 1.16.0](https://github.com/libuv/libuv/commit/4b666bd2d82a51f1c809b2703a91679789c1ec01)
(i.e. Node 9.2.0 and up), where `O_DSYNC` and `O_SYNC` are both mapped to
[FILE_FLAG_WRITE_THROUGH](https://support.microsoft.com/en-za/help/99794/info-file-flag-write-through-and-file-flag-no-buffering).

## Block Device Size and Sector Size

Node's `fs.fstat()` will not work at all for a block device on Windows, and will
not report the correct size for a block device on other platforms. You should
use `getBlockDevice()` instead:

**getBlockDevice(fd, callback)** *(FreeBSD, Linux, macOS, Windows)*

Returns an object with the following properties:

* `logicalSectorSize` - The size of a logical sector in bytes. Some drives will
advertise a backwards compatible logical sector size of 512 bytes while their
physical sector size is in fact 4096 bytes.

* `physicalSectorSize` - The size of a physical sector in bytes. You should use
this to decide on the `size` and `alignment` parameters when getting aligned
buffers so that reads and writes are always a multiple of the physical sector
size. **Some virtual devices may report a `physicalSectorSize` of 0 bytes.**

* `size` - The total size of the block device in bytes.

* `serialNumber` - The serial number reported by the device. *(FreeBSD, Linux)*

## Block Device Path and Permissions

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

## Mandatory Locks

Windows has
[special restrictions](https://support.microsoft.com/en-us/help/942448/changes-to-the-file-system-and-to-the-storage-stack-to-restrict-direct)
concerning writing to block devices. You must lock the block device by providing
the `O_EXLOCK` flag when opening the file descriptor, or else by calling
`setFSCTL_LOCK_VOLUME()` after opening the file descriptor. On other platforms
it is good practice to lock the block device by providing either the `O_EXCL` or
`O_EXLOCK` flag when opening the file descriptor:

**O_EXCL** *(Linux)*

Provide as a flag to `fs.open()` when opening a block device to obtain an
exclusive mandatory (and not just advisory) lock. When opening a regular file,
behavior is undefined. In general, the behavior of `O_EXCL` is undefined if it
is used without `O_CREAT`. There is one exception: on Linux 2.6 and later,
`O_EXCL` can be used without `O_CREAT` if the path refers to a block device. If
the block device is in use by the system e.g. if it is mounted, `fs.open()` will
fail with an `EBUSY` error.

**O_EXLOCK** *(macOS, Windows)*

Provide as a flag to `fs.open()` when opening a block device or regular file to
obtain an exclusive mandatory (and not just advisory) lock.

On macOS, when opening a regular file with `O_EXLOCK`, `fs.open()` will block
until any existing lock is released. While adding `O_NONBLOCK` can avoid this,
it also introduces other IO semantics. Using `O_EXLOCK` should therefore be
limited to opening a block device on macOS. If the block device is in use by the
system, i.e. it is mounted, `fs.open()` will fail with an `EBUSY` error.

On Windows, `O_EXLOCK` is supported as of [libuv 1.17.0](https://github.com/libuv/libuv/commit/1c4de1916e36f8462c48a36ce7c88b247465f3cf)
(i.e. Node 9.3.0 and up), where `O_EXLOCK` is mapped to an
[exclusive sharing mode](https://msdn.microsoft.com/en-us/library/windows/desktop/aa363858(v=vs.85).aspx)
of 0. If the block device or regular file is already open, `fs.open()` will fail
with an `EBUSY` error.

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

## Advisory Locks

**setFlock(fd, value, callback)** *(FreeBSD, Linux, macOS)*

Apply or remove an advisory lock on an open regular file:

* A `value` of `1` locks the regular file using `flock(LOCK_EX | LOCK_NB)`.
* A `value` of `0` unlocks the regular file using `flock(LOCK_UN)`.
* Advisory locks allow cooperating processes to perform consistent operations
but do not guarantee consistency since other processes may not check for the
presence of advisory locks.
* A locked regular file remains locked until the application unlocks the regular
file, or until the file descriptor is closed, either directly through
`fs.close()`, or indirectly when the process terminates.

## Benchmark

The write performance of various block sizes and open flags can vary across
operating systems and between hard drives and solid state drives. Use the
included write benchmark to benchmark various block sizes and open flags on the
local file system (by default) or on a specific block device or regular file:

**WARNING: The write benchmark will erase the contents of the specified block
device or regular file if any.**

```
[sudo] node benchmark.js [device|file]
```

```
$ sudo node benchmark.js /dev/sda

      4096 | /dev/sda | BUFFERED                               |  2461.54 MB/s
      4096 | /dev/sda | BUFFERED + ALIGNED                     |  1855.07 MB/s
      4096 | /dev/sda | O_DIRECT                               |    58.69 MB/s
      4096 | /dev/sda | AMORTIZED_FDATASYNC                    |   122.84 MB/s
      4096 | /dev/sda | AMORTIZED_FDATASYNC + ALIGNED          |   120.87 MB/s
      4096 | /dev/sda | AMORTIZED_FDATASYNC + O_DIRECT         |    57.92 MB/s
      4096 | /dev/sda | AMORTIZED_FSYNC                        |   120.08 MB/s
      4096 | /dev/sda | AMORTIZED_FSYNC + ALIGNED              |   121.79 MB/s
      4096 | /dev/sda | AMORTIZED_FSYNC + O_DIRECT             |    59.70 MB/s
      4096 | /dev/sda | FDATASYNC                              |     0.44 MB/s
      4096 | /dev/sda | FDATASYNC + ALIGNED                    |     0.47 MB/s
      4096 | /dev/sda | FDATASYNC + O_DIRECT                   |     0.47 MB/s
      4096 | /dev/sda | FSYNC                                  |     0.47 MB/s
      4096 | /dev/sda | FSYNC + ALIGNED                        |     0.47 MB/s
      4096 | /dev/sda | FSYNC + O_DIRECT                       |     0.47 MB/s
      4096 | /dev/sda | O_DSYNC                                |     0.47 MB/s
      4096 | /dev/sda | O_DSYNC + ALIGNED                      |     0.47 MB/s
      4096 | /dev/sda | O_DSYNC + O_DIRECT                     |     0.47 MB/s
      4096 | /dev/sda | O_SYNC                                 |     0.47 MB/s
      4096 | /dev/sda | O_SYNC + ALIGNED                       |     0.46 MB/s
      4096 | /dev/sda | O_SYNC + O_DIRECT                      |     0.47 MB/s
      8192 | /dev/sda | BUFFERED                               |  3121.95 MB/s
      8192 | /dev/sda | BUFFERED + ALIGNED                     |  3047.62 MB/s
      8192 | /dev/sda | O_DIRECT                               |    88.58 MB/s
      8192 | /dev/sda | AMORTIZED_FDATASYNC                    |   123.43 MB/s
      8192 | /dev/sda | AMORTIZED_FDATASYNC + ALIGNED          |   120.30 MB/s
      8192 | /dev/sda | AMORTIZED_FDATASYNC + O_DIRECT         |    87.55 MB/s
      8192 | /dev/sda | AMORTIZED_FSYNC                        |   120.08 MB/s
      8192 | /dev/sda | AMORTIZED_FSYNC + ALIGNED              |   121.44 MB/s
      8192 | /dev/sda | AMORTIZED_FSYNC + O_DIRECT             |    90.01 MB/s
      8192 | /dev/sda | FDATASYNC                              |     0.91 MB/s
      8192 | /dev/sda | FDATASYNC + ALIGNED                    |     0.93 MB/s
      8192 | /dev/sda | FDATASYNC + O_DIRECT                   |     0.93 MB/s
      8192 | /dev/sda | FSYNC                                  |     0.93 MB/s
      8192 | /dev/sda | FSYNC + ALIGNED                        |     0.92 MB/s
      8192 | /dev/sda | FSYNC + O_DIRECT                       |     0.92 MB/s
      8192 | /dev/sda | O_DSYNC                                |     0.93 MB/s
      8192 | /dev/sda | O_DSYNC + ALIGNED                      |     0.92 MB/s
      8192 | /dev/sda | O_DSYNC + O_DIRECT                     |     0.93 MB/s
      8192 | /dev/sda | O_SYNC                                 |     0.93 MB/s
      8192 | /dev/sda | O_SYNC + ALIGNED                       |     0.92 MB/s
      8192 | /dev/sda | O_SYNC + O_DIRECT                      |     0.92 MB/s
     16384 | /dev/sda | BUFFERED                               |  3878.79 MB/s
     16384 | /dev/sda | BUFFERED + ALIGNED                     |  3657.14 MB/s
     16384 | /dev/sda | O_DIRECT                               |   125.74 MB/s
     16384 | /dev/sda | AMORTIZED_FDATASYNC                    |   120.98 MB/s
     16384 | /dev/sda | AMORTIZED_FDATASYNC + ALIGNED          |   119.85 MB/s
     16384 | /dev/sda | AMORTIZED_FDATASYNC + O_DIRECT         |   121.90 MB/s
     16384 | /dev/sda | AMORTIZED_FSYNC                        |   121.56 MB/s
     16384 | /dev/sda | AMORTIZED_FSYNC + ALIGNED              |   120.53 MB/s
     16384 | /dev/sda | AMORTIZED_FSYNC + O_DIRECT             |   121.79 MB/s
     16384 | /dev/sda | FDATASYNC                              |     1.84 MB/s
     16384 | /dev/sda | FDATASYNC + ALIGNED                    |     1.84 MB/s
     16384 | /dev/sda | FDATASYNC + O_DIRECT                   |     1.84 MB/s
     16384 | /dev/sda | FSYNC                                  |     1.85 MB/s
     16384 | /dev/sda | FSYNC + ALIGNED                        |     1.84 MB/s
     16384 | /dev/sda | FSYNC + O_DIRECT                       |     1.81 MB/s
     16384 | /dev/sda | O_DSYNC                                |     1.84 MB/s
     16384 | /dev/sda | O_DSYNC + ALIGNED                      |     1.84 MB/s
     16384 | /dev/sda | O_DSYNC + O_DIRECT                     |     1.84 MB/s
     16384 | /dev/sda | O_SYNC                                 |     1.83 MB/s
     16384 | /dev/sda | O_SYNC + ALIGNED                       |     1.81 MB/s
     16384 | /dev/sda | O_SYNC + O_DIRECT                      |     1.84 MB/s
     32768 | /dev/sda | BUFFERED                               |  3657.14 MB/s
     32768 | /dev/sda | BUFFERED + ALIGNED                     |  4129.03 MB/s
     32768 | /dev/sda | O_DIRECT                               |   128.39 MB/s
     32768 | /dev/sda | AMORTIZED_FDATASYNC                    |   120.98 MB/s
     32768 | /dev/sda | AMORTIZED_FDATASYNC + ALIGNED          |   123.31 MB/s
     32768 | /dev/sda | AMORTIZED_FDATASYNC + O_DIRECT         |   121.56 MB/s
     32768 | /dev/sda | AMORTIZED_FSYNC                        |   122.02 MB/s
     32768 | /dev/sda | AMORTIZED_FSYNC + ALIGNED              |   120.64 MB/s
     32768 | /dev/sda | AMORTIZED_FSYNC + O_DIRECT             |   121.67 MB/s
     32768 | /dev/sda | FDATASYNC                              |     3.63 MB/s
     32768 | /dev/sda | FDATASYNC + ALIGNED                    |     3.64 MB/s
     32768 | /dev/sda | FDATASYNC + O_DIRECT                   |     3.62 MB/s
     32768 | /dev/sda | FSYNC                                  |     3.62 MB/s
     32768 | /dev/sda | FSYNC + ALIGNED                        |     3.62 MB/s
     32768 | /dev/sda | FSYNC + O_DIRECT                       |     3.62 MB/s
     32768 | /dev/sda | O_DSYNC                                |     3.62 MB/s
     32768 | /dev/sda | O_DSYNC + ALIGNED                      |     3.63 MB/s
     32768 | /dev/sda | O_DSYNC + O_DIRECT                     |     3.62 MB/s
     32768 | /dev/sda | O_SYNC                                 |     3.63 MB/s
     32768 | /dev/sda | O_SYNC + ALIGNED                       |     3.62 MB/s
     32768 | /dev/sda | O_SYNC + O_DIRECT                      |     3.63 MB/s
     65536 | /dev/sda | BUFFERED                               |  4000.00 MB/s
     65536 | /dev/sda | BUFFERED + ALIGNED                     |  4000.00 MB/s
     65536 | /dev/sda | O_DIRECT                               |   128.13 MB/s
     65536 | /dev/sda | AMORTIZED_FDATASYNC                    |   120.08 MB/s
     65536 | /dev/sda | AMORTIZED_FDATASYNC + ALIGNED          |   120.08 MB/s
     65536 | /dev/sda | AMORTIZED_FDATASYNC + O_DIRECT         |   120.64 MB/s
     65536 | /dev/sda | AMORTIZED_FSYNC                        |   122.96 MB/s
     65536 | /dev/sda | AMORTIZED_FSYNC + ALIGNED              |   122.72 MB/s
     65536 | /dev/sda | AMORTIZED_FSYNC + O_DIRECT             |   121.10 MB/s
     65536 | /dev/sda | FDATASYNC                              |     7.06 MB/s
     65536 | /dev/sda | FDATASYNC + ALIGNED                    |     7.04 MB/s
     65536 | /dev/sda | FDATASYNC + O_DIRECT                   |     7.07 MB/s
     65536 | /dev/sda | FSYNC                                  |     7.06 MB/s
     65536 | /dev/sda | FSYNC + ALIGNED                        |     7.05 MB/s
     65536 | /dev/sda | FSYNC + O_DIRECT                       |     7.05 MB/s
     65536 | /dev/sda | O_DSYNC                                |     7.02 MB/s
     65536 | /dev/sda | O_DSYNC + ALIGNED                      |     7.05 MB/s
     65536 | /dev/sda | O_DSYNC + O_DIRECT                     |     7.04 MB/s
     65536 | /dev/sda | O_SYNC                                 |     7.04 MB/s
     65536 | /dev/sda | O_SYNC + ALIGNED                       |     7.02 MB/s
     65536 | /dev/sda | O_SYNC + O_DIRECT                      |     7.05 MB/s
    131072 | /dev/sda | BUFFERED                               |  3764.71 MB/s
    131072 | /dev/sda | BUFFERED + ALIGNED                     |  4000.00 MB/s
    131072 | /dev/sda | O_DIRECT                               |   131.55 MB/s
    131072 | /dev/sda | AMORTIZED_FDATASYNC                    |   118.19 MB/s
    131072 | /dev/sda | AMORTIZED_FDATASYNC + ALIGNED          |   120.30 MB/s
    131072 | /dev/sda | AMORTIZED_FDATASYNC + O_DIRECT         |   121.33 MB/s
    131072 | /dev/sda | AMORTIZED_FSYNC                        |   119.18 MB/s
    131072 | /dev/sda | AMORTIZED_FSYNC + ALIGNED              |   120.75 MB/s
    131072 | /dev/sda | AMORTIZED_FSYNC + O_DIRECT             |   120.87 MB/s
    131072 | /dev/sda | FDATASYNC                              |    13.32 MB/s
    131072 | /dev/sda | FDATASYNC + ALIGNED                    |    13.37 MB/s
    131072 | /dev/sda | FDATASYNC + O_DIRECT                   |    13.36 MB/s
    131072 | /dev/sda | FSYNC                                  |    13.38 MB/s
    131072 | /dev/sda | FSYNC + ALIGNED                        |    13.39 MB/s
    131072 | /dev/sda | FSYNC + O_DIRECT                       |    13.37 MB/s
    131072 | /dev/sda | O_DSYNC                                |    13.39 MB/s
    131072 | /dev/sda | O_DSYNC + ALIGNED                      |    13.36 MB/s
    131072 | /dev/sda | O_DSYNC + O_DIRECT                     |    13.38 MB/s
    131072 | /dev/sda | O_SYNC                                 |    13.38 MB/s
    131072 | /dev/sda | O_SYNC + ALIGNED                       |    13.38 MB/s
    131072 | /dev/sda | O_SYNC + O_DIRECT                      |    13.37 MB/s
    262144 | /dev/sda | BUFFERED                               |  3764.71 MB/s
    262144 | /dev/sda | BUFFERED + ALIGNED                     |  3368.42 MB/s
    262144 | /dev/sda | O_DIRECT                               |   128.13 MB/s
    262144 | /dev/sda | AMORTIZED_FDATASYNC                    |   120.08 MB/s
    262144 | /dev/sda | AMORTIZED_FDATASYNC + ALIGNED          |   120.64 MB/s
    262144 | /dev/sda | AMORTIZED_FDATASYNC + O_DIRECT         |   123.79 MB/s
    262144 | /dev/sda | AMORTIZED_FSYNC                        |   122.49 MB/s
    262144 | /dev/sda | AMORTIZED_FSYNC + ALIGNED              |   115.11 MB/s
    262144 | /dev/sda | AMORTIZED_FSYNC + O_DIRECT             |   122.84 MB/s
    262144 | /dev/sda | FDATASYNC                              |    23.96 MB/s
    262144 | /dev/sda | FDATASYNC + ALIGNED                    |    24.14 MB/s
    262144 | /dev/sda | FDATASYNC + O_DIRECT                   |    24.37 MB/s
    262144 | /dev/sda | FSYNC                                  |    24.31 MB/s
    262144 | /dev/sda | FSYNC + ALIGNED                        |    24.30 MB/s
    262144 | /dev/sda | FSYNC + O_DIRECT                       |    24.22 MB/s
    262144 | /dev/sda | O_DSYNC                                |    24.23 MB/s
    262144 | /dev/sda | O_DSYNC + ALIGNED                      |    24.22 MB/s
    262144 | /dev/sda | O_DSYNC + O_DIRECT                     |    24.17 MB/s
    262144 | /dev/sda | O_SYNC                                 |    24.27 MB/s
    262144 | /dev/sda | O_SYNC + ALIGNED                       |    24.38 MB/s
    262144 | /dev/sda | O_SYNC + O_DIRECT                      |    24.19 MB/s
    524288 | /dev/sda | BUFFERED                               |  3657.14 MB/s
    524288 | /dev/sda | BUFFERED + ALIGNED                     |  4000.00 MB/s
    524288 | /dev/sda | O_DIRECT                               |   131.01 MB/s
    524288 | /dev/sda | AMORTIZED_FDATASYNC                    |   122.96 MB/s
    524288 | /dev/sda | AMORTIZED_FDATASYNC + ALIGNED          |   121.56 MB/s
    524288 | /dev/sda | AMORTIZED_FDATASYNC + O_DIRECT         |   123.43 MB/s
    524288 | /dev/sda | AMORTIZED_FSYNC                        |   121.90 MB/s
    524288 | /dev/sda | AMORTIZED_FSYNC + ALIGNED              |   121.67 MB/s
    524288 | /dev/sda | AMORTIZED_FSYNC + O_DIRECT             |   124.88 MB/s
    524288 | /dev/sda | FDATASYNC                              |    40.87 MB/s
    524288 | /dev/sda | FDATASYNC + ALIGNED                    |    40.86 MB/s
    524288 | /dev/sda | FDATASYNC + O_DIRECT                   |    40.87 MB/s
    524288 | /dev/sda | FSYNC                                  |    40.97 MB/s
    524288 | /dev/sda | FSYNC + ALIGNED                        |    40.70 MB/s
    524288 | /dev/sda | FSYNC + O_DIRECT                       |    40.61 MB/s
    524288 | /dev/sda | O_DSYNC                                |    40.86 MB/s
    524288 | /dev/sda | O_DSYNC + ALIGNED                      |    40.73 MB/s
    524288 | /dev/sda | O_DSYNC + O_DIRECT                     |    41.04 MB/s
    524288 | /dev/sda | O_SYNC                                 |    40.87 MB/s
    524288 | /dev/sda | O_SYNC + ALIGNED                       |    40.82 MB/s
    524288 | /dev/sda | O_SYNC + O_DIRECT                      |    40.92 MB/s
   1048576 | /dev/sda | BUFFERED                               |  3657.14 MB/s
   1048576 | /dev/sda | BUFFERED + ALIGNED                     |  4129.03 MB/s
   1048576 | /dev/sda | O_DIRECT                               |   131.96 MB/s
   1048576 | /dev/sda | AMORTIZED_FDATASYNC                    |   120.98 MB/s
   1048576 | /dev/sda | AMORTIZED_FDATASYNC + ALIGNED          |   121.90 MB/s
   1048576 | /dev/sda | AMORTIZED_FDATASYNC + O_DIRECT         |   124.51 MB/s
   1048576 | /dev/sda | AMORTIZED_FSYNC                        |   121.90 MB/s
   1048576 | /dev/sda | AMORTIZED_FSYNC + ALIGNED              |   115.42 MB/s
   1048576 | /dev/sda | AMORTIZED_FSYNC + O_DIRECT             |   123.67 MB/s
   1048576 | /dev/sda | FDATASYNC                              |    61.99 MB/s
   1048576 | /dev/sda | FDATASYNC + ALIGNED                    |    62.38 MB/s
   1048576 | /dev/sda | FDATASYNC + O_DIRECT                   |    61.60 MB/s
   1048576 | /dev/sda | FSYNC                                  |    61.96 MB/s
   1048576 | /dev/sda | FSYNC + ALIGNED                        |    61.90 MB/s
   1048576 | /dev/sda | FSYNC + O_DIRECT                       |    61.60 MB/s
   1048576 | /dev/sda | O_DSYNC                                |    62.44 MB/s
   1048576 | /dev/sda | O_DSYNC + ALIGNED                      |    61.84 MB/s
   1048576 | /dev/sda | O_DSYNC + O_DIRECT                     |    61.87 MB/s
   1048576 | /dev/sda | O_SYNC                                 |    61.72 MB/s
   1048576 | /dev/sda | O_SYNC + ALIGNED                       |    61.60 MB/s
   1048576 | /dev/sda | O_SYNC + O_DIRECT                      |    62.38 MB/s
   2097152 | /dev/sda | BUFFERED                               |  3555.56 MB/s
   2097152 | /dev/sda | BUFFERED + ALIGNED                     |  4000.00 MB/s
   2097152 | /dev/sda | O_DIRECT                               |   128.64 MB/s
   2097152 | /dev/sda | AMORTIZED_FDATASYNC                    |   122.61 MB/s
   2097152 | /dev/sda | AMORTIZED_FDATASYNC + ALIGNED          |   120.30 MB/s
   2097152 | /dev/sda | AMORTIZED_FDATASYNC + O_DIRECT         |   122.84 MB/s
   2097152 | /dev/sda | AMORTIZED_FSYNC                        |   120.08 MB/s
   2097152 | /dev/sda | AMORTIZED_FSYNC + ALIGNED              |   122.61 MB/s
   2097152 | /dev/sda | AMORTIZED_FSYNC + O_DIRECT             |   123.31 MB/s
   2097152 | /dev/sda | FDATASYNC                              |    61.72 MB/s
   2097152 | /dev/sda | FDATASYNC + ALIGNED                    |    62.11 MB/s
   2097152 | /dev/sda | FDATASYNC + O_DIRECT                   |    61.87 MB/s
   2097152 | /dev/sda | FSYNC                                  |    61.48 MB/s
   2097152 | /dev/sda | FSYNC + ALIGNED                        |    62.11 MB/s
   2097152 | /dev/sda | FSYNC + O_DIRECT                       |    62.14 MB/s
   2097152 | /dev/sda | O_DSYNC                                |    61.72 MB/s
   2097152 | /dev/sda | O_DSYNC + ALIGNED                      |    62.11 MB/s
   2097152 | /dev/sda | O_DSYNC + O_DIRECT                     |    62.44 MB/s
   2097152 | /dev/sda | O_SYNC                                 |    62.20 MB/s
   2097152 | /dev/sda | O_SYNC + ALIGNED                       |    61.90 MB/s
   2097152 | /dev/sda | O_SYNC + O_DIRECT                      |    62.11 MB/s
   4194304 | /dev/sda | BUFFERED                               |  3459.46 MB/s
   4194304 | /dev/sda | BUFFERED + ALIGNED                     |  3764.71 MB/s
   4194304 | /dev/sda | O_DIRECT                               |   129.95 MB/s
   4194304 | /dev/sda | AMORTIZED_FDATASYNC                    |   120.08 MB/s
   4194304 | /dev/sda | AMORTIZED_FDATASYNC + ALIGNED          |   121.21 MB/s
   4194304 | /dev/sda | AMORTIZED_FDATASYNC + O_DIRECT         |   123.67 MB/s
   4194304 | /dev/sda | AMORTIZED_FSYNC                        |   123.67 MB/s
   4194304 | /dev/sda | AMORTIZED_FSYNC + ALIGNED              |   122.14 MB/s
   4194304 | /dev/sda | AMORTIZED_FSYNC + O_DIRECT             |   122.96 MB/s
   4194304 | /dev/sda | FDATASYNC                              |    83.39 MB/s
   4194304 | /dev/sda | FDATASYNC + ALIGNED                    |    82.85 MB/s
   4194304 | /dev/sda | FDATASYNC + O_DIRECT                   |    83.82 MB/s
   4194304 | /dev/sda | FSYNC                                  |    83.06 MB/s
   4194304 | /dev/sda | FSYNC + ALIGNED                        |    82.90 MB/s
   4194304 | /dev/sda | FSYNC + O_DIRECT                       |    82.85 MB/s
   4194304 | /dev/sda | O_DSYNC                                |    83.06 MB/s
   4194304 | /dev/sda | O_DSYNC + ALIGNED                      |    83.82 MB/s
   4194304 | /dev/sda | O_DSYNC + O_DIRECT                     |    82.47 MB/s
   4194304 | /dev/sda | O_SYNC                                 |    82.63 MB/s
   4194304 | /dev/sda | O_SYNC + ALIGNED                       |    82.10 MB/s
   4194304 | /dev/sda | O_SYNC + O_DIRECT                      |    81.58 MB/s
   8388608 | /dev/sda | BUFFERED                               |  2612.24 MB/s
   8388608 | /dev/sda | BUFFERED + ALIGNED                     |  2976.74 MB/s
   8388608 | /dev/sda | O_DIRECT                               |   132.09 MB/s
   8388608 | /dev/sda | AMORTIZED_FDATASYNC                    |   115.52 MB/s
   8388608 | /dev/sda | AMORTIZED_FDATASYNC + ALIGNED          |   121.10 MB/s
   8388608 | /dev/sda | AMORTIZED_FDATASYNC + O_DIRECT         |   119.18 MB/s
   8388608 | /dev/sda | AMORTIZED_FSYNC                        |   117.32 MB/s
   8388608 | /dev/sda | AMORTIZED_FSYNC + ALIGNED              |   120.75 MB/s
   8388608 | /dev/sda | AMORTIZED_FSYNC + O_DIRECT             |   119.07 MB/s
   8388608 | /dev/sda | FDATASYNC                              |   100.23 MB/s
   8388608 | /dev/sda | FDATASYNC + ALIGNED                    |    98.92 MB/s
   8388608 | /dev/sda | FDATASYNC + O_DIRECT                   |    94.67 MB/s
   8388608 | /dev/sda | FSYNC                                  |    94.81 MB/s
   8388608 | /dev/sda | FSYNC + ALIGNED                        |    95.31 MB/s
   8388608 | /dev/sda | FSYNC + O_DIRECT                       |    95.31 MB/s
   8388608 | /dev/sda | O_DSYNC                                |    93.64 MB/s
   8388608 | /dev/sda | O_DSYNC + ALIGNED                      |    96.39 MB/s
   8388608 | /dev/sda | O_DSYNC + O_DIRECT                     |    95.88 MB/s
   8388608 | /dev/sda | O_SYNC                                 |    93.70 MB/s
   8388608 | /dev/sda | O_SYNC + ALIGNED                       |    97.64 MB/s
   8388608 | /dev/sda | O_SYNC + O_DIRECT                      |    94.60 MB/s
  16777216 | /dev/sda | BUFFERED                               |  1939.39 MB/s
  16777216 | /dev/sda | BUFFERED + ALIGNED                     |  2723.40 MB/s
  16777216 | /dev/sda | O_DIRECT                               |   130.61 MB/s
  16777216 | /dev/sda | AMORTIZED_FDATASYNC                    |   121.67 MB/s
  16777216 | /dev/sda | AMORTIZED_FDATASYNC + ALIGNED          |   114.80 MB/s
  16777216 | /dev/sda | AMORTIZED_FDATASYNC + O_DIRECT         |   123.55 MB/s
  16777216 | /dev/sda | AMORTIZED_FSYNC                        |   120.98 MB/s
  16777216 | /dev/sda | AMORTIZED_FSYNC + ALIGNED              |   122.61 MB/s
  16777216 | /dev/sda | AMORTIZED_FSYNC + O_DIRECT             |   124.03 MB/s
  16777216 | /dev/sda | FDATASYNC                              |   111.21 MB/s
  16777216 | /dev/sda | FDATASYNC + ALIGNED                    |   112.68 MB/s
  16777216 | /dev/sda | FDATASYNC + O_DIRECT                   |   110.34 MB/s
  16777216 | /dev/sda | FSYNC                                  |   111.30 MB/s
  16777216 | /dev/sda | FSYNC + ALIGNED                        |   112.78 MB/s
  16777216 | /dev/sda | FSYNC + O_DIRECT                       |   111.89 MB/s
  16777216 | /dev/sda | O_DSYNC                                |   108.20 MB/s
  16777216 | /dev/sda | O_DSYNC + ALIGNED                      |   111.79 MB/s
  16777216 | /dev/sda | O_DSYNC + O_DIRECT                     |   111.99 MB/s
  16777216 | /dev/sda | O_SYNC                                 |   106.67 MB/s
  16777216 | /dev/sda | O_SYNC + ALIGNED                       |   111.89 MB/s
  16777216 | /dev/sda | O_SYNC + O_DIRECT                      |   111.01 MB/s
  33554432 | /dev/sda | BUFFERED                               |  2245.61 MB/s
  33554432 | /dev/sda | BUFFERED + ALIGNED                     |  2461.54 MB/s
  33554432 | /dev/sda | O_DIRECT                               |   127.62 MB/s
  33554432 | /dev/sda | AMORTIZED_FDATASYNC                    |   118.19 MB/s
  33554432 | /dev/sda | AMORTIZED_FDATASYNC + ALIGNED          |   120.75 MB/s
  33554432 | /dev/sda | AMORTIZED_FDATASYNC + O_DIRECT         |   121.56 MB/s
  33554432 | /dev/sda | AMORTIZED_FSYNC                        |   118.74 MB/s
  33554432 | /dev/sda | AMORTIZED_FSYNC + ALIGNED              |   121.21 MB/s
  33554432 | /dev/sda | AMORTIZED_FSYNC + O_DIRECT             |   123.55 MB/s
  33554432 | /dev/sda | FDATASYNC                              |   113.58 MB/s
  33554432 | /dev/sda | FDATASYNC + ALIGNED                    |   116.58 MB/s
  33554432 | /dev/sda | FDATASYNC + O_DIRECT                   |   115.84 MB/s
  33554432 | /dev/sda | FSYNC                                  |   115.52 MB/s
  33554432 | /dev/sda | FSYNC + ALIGNED                        |   115.32 MB/s
  33554432 | /dev/sda | FSYNC + O_DIRECT                       |   116.05 MB/s
  33554432 | /dev/sda | O_DSYNC                                |   116.68 MB/s
  33554432 | /dev/sda | O_DSYNC + ALIGNED                      |   116.47 MB/s
  33554432 | /dev/sda | O_DSYNC + O_DIRECT                     |   116.26 MB/s
  33554432 | /dev/sda | O_SYNC                                 |   114.59 MB/s
  33554432 | /dev/sda | O_SYNC + ALIGNED                       |   116.36 MB/s
  33554432 | /dev/sda | O_SYNC + O_DIRECT                      |   116.05 MB/s
```
