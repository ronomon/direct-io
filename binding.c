#include <assert.h>
#include <limits.h>
#include <math.h>
#include <node_api.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <uv.h>
#if defined(__APPLE__)
#include <sys/disk.h>
#elif defined(_WIN32)
#include <io.h>
#include <malloc.h>
#include <winioctl.h>
#elif defined(__FreeBSD__) || defined(__FreeBSD_kernel_)
#include <sys/disk.h>
#else
#include <linux/fs.h>
#include <scsi/sg.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#endif

#define RESOURCE_NAME "@ronomon/direct-io"
  
#define DEVICE_SERIAL_MAX 1024

#define OK(call)                                                               \
  assert((call) == napi_ok);

#define THROW(env, message)                                                    \
  do {                                                                         \
    napi_throw_error((env), NULL, (message));                                  \
    return NULL;                                                               \
  } while (0)

static int arg_int(napi_env env, napi_value value, int* integer) {
  assert(*integer == 0);
  double temp = 0;
  if (
    // We get the value as a double so we can check for NaN, Infinity and float:
    // https://github.com/nodejs/node/issues/26323
    napi_get_value_double(env, value, &temp) != napi_ok ||
    temp < 0 ||
    isnan(temp) ||
    // Infinity, also prevent UB for double->int cast below:
    // https://groups.google.com/forum/#!topic/comp.lang.c/rhPzd4bgKJk
    temp > INT_MAX ||
    // Float:
    (double) ((int) temp) != temp
  ) {
    return 0;
  }
  *integer = (int) temp;
  assert(*integer >= 0);
  return 1;
}

void set_int(
  napi_env env,
  napi_value object,
  const char* name,
  const int64_t integer
) {
  assert(integer >= 0);
  napi_value value;
  OK(napi_create_int64(env, integer, &value));
  OK(napi_set_named_property(env, object, name, value));
}

void set_method(
  napi_env env,
  napi_value object,
  const char* name,
  void* method
) {
  napi_value value;
  OK(napi_create_function(env, NULL, 0, method, NULL, &value));
  OK(napi_set_named_property(env, object, name, value));
}

struct task_data {
  int fd;
  int value;
  int device;
  int64_t device_sector_logical;
  int64_t device_sector_physical;
  int64_t device_size;
  char device_serial[DEVICE_SERIAL_MAX];
  size_t device_serial_size;
  napi_ref ref_callback;
  napi_async_work async_work;
  const char* error;
};

void task_assert(struct task_data* task) {
  assert(task->fd >= 0);
  assert(task->value == 0 || task->value == 1);
  assert(task->device == 0 || task->device == 1);
  assert(task->device_sector_logical == 0);
  assert(task->device_sector_physical == 0);
  assert(task->device_size == 0);
  assert(task->device_serial_size == 0);
  assert(task->error == NULL);
}

void task_complete(napi_env env, napi_status status, void* data) {
  struct task_data* task = data;
  if (status == napi_cancelled) {
    task->error = "async work was cancelled";
  } else {
    assert(status == napi_ok);
  }
  int argc = 0;
  napi_value argv[2];
  if (task->error) {
    argc = 1;
    napi_value message;
    OK(napi_create_string_utf8(env, task->error, NAPI_AUTO_LENGTH, &message));
    OK(napi_create_error(env, NULL, message, &argv[0]));
  } else if (task->device == 0) {
    assert(task->device_sector_logical == 0);
    assert(task->device_sector_physical == 0);
    assert(task->device_size == 0);
    assert(task->device_serial_size == 0);
    argc = 1;
    OK(napi_get_undefined(env, &argv[0]));
  } else {
    assert(task->device == 1);
    argc = 2;
    OK(napi_get_undefined(env, &argv[0]));
    OK(napi_create_object(env, &argv[1]));
    set_int(env, argv[1], "logicalSectorSize", task->device_sector_logical);
    set_int(env, argv[1], "physicalSectorSize", task->device_sector_physical);
    set_int(env, argv[1], "size", task->device_size);
    // Trim leading and trailing space from serial number:
    // On Linux, we saw a serial number with leading space trimmed by smartctl.
    char* serial = task->device_serial;
    size_t serial_size = task->device_serial_size;
    while (serial_size > 0 && serial[0] == 32) {
      serial++;
      serial_size--;
    }
    while (serial_size > 0 && serial[serial_size - 1] == 32) serial_size--;
    napi_value serial_string;
    OK(napi_create_string_utf8(env, serial, serial_size, &serial_string));
    OK(napi_set_named_property(env, argv[1], "serialNumber", serial_string));
  }
  napi_value scope;
  OK(napi_get_global(env, &scope));
  napi_value callback;
  OK(napi_get_reference_value(env, task->ref_callback, &callback));
  // Do not assert the return status of napi_call_function():
  // If the callback throws then the return status will not be napi_ok.
  napi_call_function(env, scope, callback, argc, argv, NULL);
  assert(task->ref_callback != NULL);
  assert(task->async_work != NULL);
  OK(napi_delete_reference(env, task->ref_callback));
  OK(napi_delete_async_work(env, task->async_work));
  free(task);
  task = NULL;
}

static napi_value task_queue(
  napi_env env,
  void* task_execute,
  int fd,
  int value,
  int device,
  napi_value callback
) {
  struct task_data* task = calloc(1, sizeof(struct task_data));
  if (!task) THROW(env, "insufficient memory");
  task->fd = fd;
  task->value = value;
  task->device = device;
  task->device_sector_logical = 0;
  task->device_sector_physical = 0;
  task->device_size = 0;
  task->device_serial_size = 0;
  task->error = NULL;
  OK(napi_create_reference(env, callback, 1, &task->ref_callback));
  napi_value name;
  OK(napi_create_string_utf8(env, RESOURCE_NAME, NAPI_AUTO_LENGTH, &name));
  OK(napi_create_async_work(
    env,
    NULL,
    name,
    task_execute,
    task_complete,
    task,
    &task->async_work
  ));
  OK(napi_queue_async_work(env, task->async_work));
  return NULL;
}

static napi_value task_args(
  napi_env env,
  napi_callback_info info,
  void* task_execute
) {
  size_t argc = 3;
  napi_value argv[3];
  OK(napi_get_cb_info(env, info, &argc, argv, NULL, NULL));
  napi_value callback = argv[2];
  napi_valuetype callback_type;
  OK(napi_typeof(env, callback, &callback_type));
  int fd = 0;
  int value = 0;
  if (
    argc != 3 ||
    !arg_int(env, argv[0], &fd) ||
    !arg_int(env, argv[1], &value) ||
    callback_type != napi_function ||
    value < 0 ||
    value > 1
  ) {
    THROW(env, "bad arguments, expected: (fd, value=0/1, callback)");
  }
  return task_queue(env, task_execute, fd, value, 0, callback);
}

void task_execute_get_block_device_serial(struct task_data* task) {
  assert(task->fd >= 0);
  assert(task->device == 1);
  assert(task->device_serial_size == 0);
  assert(task->error == NULL);
#if defined(__APPLE__) || defined(_WIN32)
  // Leave device serial number as an empty string.
#elif defined(__FreeBSD__) || defined(__FreeBSD_kernel_)
  if (ioctl(task->fd, DIOCGIDENT, task->device_serial) != 0) {
    task->error = "DIOCGIDENT failed";
    return;
  }
  task->device_serial_size = strlen(task->device_serial);
#else
  int version = 0;
  // SATA-only alternative: http://www.cplusplus.com/forum/general/191532
  if (ioctl(task->fd, SG_GET_VERSION_NUM, &version) != 0) {
    task->error = "SG_GET_VERSION_NUM failed";
    return;
  }
  if (version < 30000) {
    task->error = "SG_GET_VERSION_NUM < 30000";
    return;
  }
  // See: http://www.tldp.org/HOWTO/SCSI-Generic-HOWTO/sg_io_hdr_t.html
  unsigned char dxferp[255];
  unsigned char cmdp[6] = { 0x12, 0x01, 0x80, 0x00, sizeof(dxferp), 0x00 };
  unsigned char sbp[32];
  sg_io_hdr_t io_hdr;
  memset(&io_hdr, 0, sizeof(sg_io_hdr_t));
  io_hdr.interface_id = 'S';
  io_hdr.dxfer_direction = SG_DXFER_FROM_DEV;
  io_hdr.cmd_len = sizeof(cmdp);
  io_hdr.mx_sb_len = sizeof(sbp);
  io_hdr.dxfer_len = sizeof(dxferp);
  io_hdr.dxferp = dxferp;
  io_hdr.cmdp = cmdp;
  io_hdr.sbp = sbp;
  io_hdr.timeout = 5000;
  if (ioctl(task->fd, SG_IO, &io_hdr) != 0) {
    task->error = "SG_IO failed";
    return;
  }
  if ((io_hdr.info & SG_INFO_OK_MASK) != SG_INFO_OK) {
    task->error = "SG_INFO_OK_MASK failed";
    return;
  }
  if (io_hdr.masked_status) {
    fprintf(stderr, "io_hdr.status=0x%x\n", io_hdr.status);
    fprintf(stderr, "io_hdr.masked_status=0x%x\n", io_hdr.masked_status);
    task->error = "io_hdr.masked_status was non-zero";
    return;
  }
  if (io_hdr.host_status) {
    fprintf(stderr, "io_hdr.host_status=0x%x\n", io_hdr.host_status);
    task->error = "io_hdr.host_status was non-zero";
    return;
  }
  if (io_hdr.driver_status) {
    fprintf(stderr, "io_hdr.driver_status=0x%x\n", io_hdr.driver_status);
    task->error = "io_hdr.driver_status was non_zero";
    return;
  }
  if (dxferp[1] != 0x80) {
    task->error = "dxferp[1] != 0x80";
    return;
  }
  size_t length = DEVICE_SERIAL_MAX;
  if (dxferp[3] < length) length = dxferp[3];
  assert(length <= DEVICE_SERIAL_MAX);
  assert(task->device_serial_size == 0);
  while (task->device_serial_size < length) {
    task->device_serial[task->device_serial_size] = (
      (char) dxferp[4 + task->device_serial_size]
    );
    task->device_serial_size++;
  }
#endif
}

void task_execute_get_block_device_size(struct task_data* task) {
  assert(task->fd >= 0);
  assert(task->device == 1);
  assert(task->device_sector_logical == 0);
  assert(task->device_sector_physical == 0);
  assert(task->device_size == 0);
  assert(task->error == NULL);
#if defined(__APPLE__)
  // See: https://opensource.apple.com/source/xnu/xnu-1699.26.8/bsd/sys/disk.h
  uint32_t logical_sector = 0;
  uint32_t physical_sector = 0;
  uint64_t logical_sectors = 0;
  if (ioctl(task->fd, DKIOCGETBLOCKSIZE, &logical_sector) == -1) {
    task->error = "DKIOCGETBLOCKSIZE failed";
    return;
  }
  if (ioctl(task->fd, DKIOCGETPHYSICALBLOCKSIZE, &physical_sector) == -1) {
    task->error = "DKIOCGETPHYSICALBLOCKSIZE failed";
    return;
  }
  if (ioctl(task->fd, DKIOCGETBLOCKCOUNT, &logical_sectors) == -1) {
    task->error = "DKIOCGETBLOCKCOUNT failed";
    return;
  }
  task->device_sector_logical = (int64_t) logical_sector;
  task->device_sector_physical = (int64_t) physical_sector;
  // Be wary of type promotion, signed * unsigned != signed:
  task->device_size = (int64_t) logical_sector * (int64_t) logical_sectors;
#elif defined(_WIN32)
  HANDLE handle = uv_get_osfhandle(task->fd);
  if (handle == INVALID_HANDLE_VALUE) {
    task->error = "EBADF: bad file descriptor";
    return;
  }
  STORAGE_PROPERTY_QUERY query;
  STORAGE_ACCESS_ALIGNMENT_DESCRIPTOR alignment;
  DISK_GEOMETRY_EX geometry;
  DWORD bytes;
  ZeroMemory(&query, sizeof(query));
  ZeroMemory(&alignment, sizeof(alignment));
  ZeroMemory(&geometry, sizeof(geometry));
  query.QueryType = PropertyStandardQuery;
  query.PropertyId = StorageAccessAlignmentProperty;
  if (
    DeviceIoControl(
      handle, 
      IOCTL_STORAGE_QUERY_PROPERTY, 
      &query, 
      sizeof(query), 
      &alignment,
      sizeof(alignment),
      &bytes,
      NULL
    )
  ) {
    task->device_sector_logical = (int64_t) alignment.BytesPerLogicalSector;
    task->device_sector_physical = (int64_t) alignment.BytesPerPhysicalSector;
  } else {
    task->error = "IOCTL_STORAGE_QUERY_PROPERTY failed";
    return;
  }
  if (
    DeviceIoControl(
      handle,
      IOCTL_DISK_GET_DRIVE_GEOMETRY_EX,
      NULL,
      0,
      &geometry,
      sizeof(geometry),
      &bytes,
      NULL
    )
  ) {
    task->device_size = (int64_t) geometry.DiskSize.QuadPart;
  } else {
    task->error = "IOCTL_DISK_GET_DRIVE_GEOMETRY_EX failed";
    return;
  }
#elif defined(__FreeBSD__) || defined(__FreeBSD_kernel_)
  // https://github.com/freebsd/freebsd/blob/master/sys/sys/disk.h
  unsigned int logical_sector = 0;
  off_t physical_sector = 0;
  off_t size = 0;
  if (ioctl(task->fd, DIOCGSECTORSIZE, &logical_sector) == -1) {
    task->error = "DIOCGSECTORSIZE failed";
    return;
  }
  if (ioctl(task->fd, DIOCGSTRIPESIZE, &physical_sector) == -1) {
    task->error = "DIOCGSTRIPESIZE failed";
    return;
  }
  if (physical_sector < 0) {
    task->error = "physical_sector < 0";
    return;
  }
  if (ioctl(task->fd, DIOCGMEDIASIZE, &size) == -1) {
    task->error = "DIOCGMEDIASIZE failed";
    return;
  }
  if (size < 0) {
    task->error = "size < 0";
    return;
  }
  task->device_sector_logical = (int64_t) logical_sector;
  task->device_sector_physical = (int64_t) physical_sector;
  task->device_size = (int64_t) size;
#else
  // https://github.com/torvalds/linux/blob/master/block/ioctl.c
  // We must use the correct type according to the control code passed:
  // https://stackoverflow.com/questions/19747663/where-are-ioctl-
  // parameters-such-as-0x1268-blksszget-actually-specified
  // The kernel expects size to be a u64 and defines u64 as uint64_t:
  // https://github.com/torvalds/linux/blob/master/tools/include/linux/types.h
  int logical_sector = 0;
  unsigned int physical_sector = 0;
  uint64_t size = 0;
  if (ioctl(task->fd, BLKSSZGET, &logical_sector) == -1) {
    task->error = "BLKSSZGET failed";
    return;
  }
  if (logical_sector < 0) {
    task->error = "logical_sector < 0";
    return;
  }
  if (ioctl(task->fd, BLKPBSZGET, &physical_sector) == -1) {
    task->error = "BLKPBSZGET failed";
    return;
  }
  if (ioctl(task->fd, BLKGETSIZE64, &size) == -1) {
    task->error = "BLKGETSIZE64 failed";
    return;
  }
  task->device_sector_logical = (int64_t) logical_sector;
  task->device_sector_physical = (int64_t) physical_sector;
  task->device_size = (int64_t) size;
#endif 
}

void task_execute_get_block_device(napi_env env, void* data) {
  struct task_data* task = data;
  task_assert(task);
#if !defined(_WIN32)
  // We do not know how to assert that a handle is a block device on Windows.
  struct stat st;
  if (fstat(task->fd, &st) == -1) {
    task->error = "fstat failed";
    return;
  }
  // FreeBSD has character devices and no block devices.
  // Linux has block devices.
  // We allow both:
  if (
    (st.st_mode & S_IFMT) != S_IFBLK &&
    (st.st_mode & S_IFMT) != S_IFCHR
  ) {
    task->error = "fd is not a block or character device";
    return;
  }
#endif
  if (!task->error) task_execute_get_block_device_size(task);
  if (!task->error) task_execute_get_block_device_serial(task);
}

#if defined(__APPLE__)
void task_execute_set_f_nocache(napi_env env, void* data) {
  struct task_data* task = data;
  task_assert(task);
  if (fcntl(task->fd, F_NOCACHE, task->value) != 0) {
    if (errno == EBADF) {
      task->error = "EBADF: bad file descriptor, fcntl";
    } else {
      task->error = "unexpected error, fcntl";
    }
  }
}
#endif

#if !defined(_WIN32)
void task_execute_set_flock(napi_env env, void* data) {
  struct task_data* task = data;
  task_assert(task);
  // Place an exclusive lock. Only one process may hold an exclusive lock for
  // a given file at a given time. A call to flock() may block if an
  // incompatible lock is held by another process. We use LOCK_NB to make a
  // non-blocking request.
  int result = flock(task->fd, task->value == 0 ? LOCK_UN : LOCK_EX | LOCK_NB);
  if (result != 0) {
    if (errno == EWOULDBLOCK) {
      task->error = "EWOULDBLOCK, the file is already locked";
    } else if (errno == EBADF) {
      task->error = "EBADF, fd is an invalid file descriptor";
    } else if (errno == EINTR) {
      task->error = "EINTR, the call was interrupted by a signal";
    } else if (errno == EINVAL) {
      task->error = "EINVAL, fd does not refer to a file";
    } else if (errno == ENOTSUP) {
      task->error = "ENOTSUP, fd is not of the correct type";
    } else {
      task->error = "unable to obtain an exclusive lock";
    }
  }
}
#endif

#if defined(_WIN32)
void task_execute_set_fsctl_lock_volume(napi_env env, void* data) {
  struct task_data* task = data;
  task_assert(task);
  HANDLE handle = uv_get_osfhandle(task->fd);
  if (handle == INVALID_HANDLE_VALUE) {
    task->error = "EBADF: bad file descriptor";
    return;
  }
  DWORD bytes;
  if (
    !DeviceIoControl(
      handle,
      task->value ? FSCTL_LOCK_VOLUME : FSCTL_UNLOCK_VOLUME,
      NULL,
      0,
      NULL,
      0,
      &bytes,
      NULL
    )
  ) {
    if (task->value) {
      task->error = "FSCTL_LOCK_VOLUME failed";
    } else {
      task->error = "FSCTL_UNLOCK_VOLUME failed";
    }
  }
}
#endif

void free_aligned(napi_env env, void* ptr, void* hint) {
  assert(ptr != NULL);
#if defined(_WIN32)
  _aligned_free(ptr);
#else
  free(ptr);
#endif
  ptr = NULL;
}

static napi_value get_aligned_buffer(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value argv[2];
  OK(napi_get_cb_info(env, info, &argc, argv, NULL, NULL));
  int int_size = 0;
  int int_alignment = 0;
  if (
    argc != 2 ||
    !arg_int(env, argv[0], &int_size) ||
    !arg_int(env, argv[1], &int_alignment)
  ) {
    THROW(env, "bad arguments, expected: (size, alignment)");
  }
  assert(int_size >= 0);
  assert(int_alignment >= 0);
  size_t size = (size_t) int_size;
  size_t alignment = (size_t) int_alignment;
  if (size == 0) THROW(env, "size must not be 0");
  if (size > 2147483647) THROW(env, "size must be at most 2147483647 bytes");
  if (alignment == 0) THROW(env, "alignment must not be 0");
  if (alignment & (alignment - 1)) THROW(env, "alignment must be a power of 2");
  // Ensure portability for software tested only on 32-bit systems:
  if (alignment < 8) THROW(env, "alignment must be at least 8 bytes");
  // Detect excessive alignment values:
  if (alignment > 4194304) {
    THROW(env, "alignment must be at most 4194304 bytes");
  }
  if (alignment < sizeof(void *)) {
    THROW(env, "alignment must be at least as large as a pointer");
  }
#if defined(_WIN32)
  void *ptr = _aligned_malloc(size, alignment);
  if (ptr == NULL) THROW(env, "insufficient memory");
#else
  void *ptr = NULL;
  int result = posix_memalign(&ptr, alignment, size);
  if (result == EINVAL) THROW(env, "bad alignment argument");
  if (result == ENOMEM) THROW(env, "insufficient memory");
  if (result != 0) THROW(env, "unexpected error, posix_memalign");
#endif
  assert(ptr != NULL);
  // Check that pointer is aligned as we expect:
  assert(((uintptr_t)(const void *)(ptr) & (alignment - 1)) == 0);
  memset(ptr, 0, size);
  napi_value buffer;
  OK(napi_create_external_buffer(env, size, ptr, free_aligned, NULL, &buffer));
  return buffer;
}

static napi_value get_block_device(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value argv[2];
  OK(napi_get_cb_info(env, info, &argc, argv, NULL, NULL));
  napi_value callback = argv[1];
  napi_valuetype callback_type;
  OK(napi_typeof(env, callback, &callback_type));
  int fd = 0;
  if (
    argc != 2 ||
    !arg_int(env, argv[0], &fd) ||
    callback_type != napi_function
  ) {
    THROW(env, "bad arguments, expected: (fd, callback)");
  }
  return task_queue(env, task_execute_get_block_device, fd, 0, 1, callback);
}

static napi_value set_f_nocache(napi_env env, napi_callback_info info) {
#if defined(__APPLE__)
  return task_args(env, info, task_execute_set_f_nocache);
#else
  THROW(env, "only supported on mac os");
#endif
}

static napi_value set_flock(napi_env env, napi_callback_info info) {
#if defined(_WIN32)
  THROW(env, "not supported on windows");
#else
  return task_args(env, info, task_execute_set_flock);
#endif
}

static napi_value set_fsctl_lock_volume(napi_env env, napi_callback_info info) {
#if defined(_WIN32)
  return task_args(env, info, task_execute_set_fsctl_lock_volume);
#else
  THROW(env, "only supported on windows");
#endif
}

static napi_value Init(napi_env env, napi_value exports) {
  // We require assert() for safety (our asserts are not side-effect free):
#ifdef NDEBUG
  fprintf(stderr, "NDEBUG compile flag not supported\n");
  abort();
#endif
  // We use an int to represent the size of an aligned buffer.
  // INT_MAX must therefore be sufficient for Node's own buffer.kMaxLength:
  assert(INT_MAX >= 2147483647);
  // On Linux, some versions of libuv did not define UV_FS_O_DIRECT correctly:
  // As a result, UV_FS_O_DIRECT was set to 0 so we must get O_DIRECT ourselves.
  // See: https://github.com/libuv/libuv/issues/2420
#if defined(__linux__) && defined(__arm__)
  int o_direct = 0x10000;
#elif defined(__linux__) && defined(__m68k__)
  int o_direct = 0x10000;
#elif defined(__linux__) && defined(__mips__)
  int o_direct = 0x08000;
#elif defined(__linux__) && defined(__powerpc__)
  int o_direct = 0x20000;
#elif defined(__linux__) && defined(__s390x__)
  int o_direct = 0x04000;
#elif defined(__linux__) && defined(__x86_64__)
  int o_direct = 0x04000;
#else
  int o_direct = UV_FS_O_DIRECT;
#endif
  // On Windows, libuv maps these flags as follows:
  // UV_FS_O_DIRECT > FILE_FLAG_NO_BUFFERING
  // UV_FS_O_DSYNC  > FILE_FLAG_WRITE_THROUGH
  // UV_FS_O_EXLOCK > SHARING MODE=0
  // UV_FS_O_SYNC   > FILE_FLAG_WRITE_THROUGH
  set_int(env, exports, "O_DIRECT", o_direct);
  set_int(env, exports, "O_DSYNC", UV_FS_O_DSYNC);
  set_int(env, exports, "O_EXCL", UV_FS_O_EXCL);
  set_int(env, exports, "O_EXLOCK", UV_FS_O_EXLOCK);
  set_int(env, exports, "O_SYNC", UV_FS_O_SYNC);
  set_method(env, exports, "getAlignedBuffer", get_aligned_buffer);
  set_method(env, exports, "getBlockDevice", get_block_device);
  set_method(env, exports, "setF_NOCACHE", set_f_nocache);
  set_method(env, exports, "setFlock", set_flock);
  set_method(env, exports, "setFSCTL_LOCK_VOLUME", set_fsctl_lock_volume);
  return exports;
}

NAPI_MODULE(NODE_GYP_MODULE_NAME, Init)

// S.D.G.
