#include <nan.h>
#include <stdint.h>
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

#define SERIAL_NUMBER_MAX 256

void free_aligned_buffer(char *buffer, void *hint) {
#if defined(_WIN32)
  _aligned_free(buffer);
#else
  free(buffer);
#endif
}

int read_serial_number(int fd, char *serial_number, int &serial_number_size) {
#if defined(__APPLE__)
  return 0;
#elif defined(_WIN32)
  return 0;
#elif defined(__FreeBSD__) || defined(__FreeBSD_kernel_)
  if (ioctl(fd, DIOCGIDENT, serial_number) != 0) return -1;
  serial_number_size = strlen(serial_number);
  return 0;
#else
  int version;
  // SATA-only alternative: http://www.cplusplus.com/forum/general/191532
  if (ioctl(fd, SG_GET_VERSION_NUM, &version) != 0) {
    printf("Error: read_serial_number: SG_GET_VERSION_NUM\n");
    return -1;
  }
  if (version < 30000) {
    printf("Error: read_serial_number: version < 30000\n");
    return -2;
  }

  sg_io_hdr_t io_hdr;
  unsigned char dxferp[255];
  unsigned char cmdp[6] = { 0x12, 0x01, 0x80, 0x00, sizeof(dxferp), 0x00 };
  unsigned char sbp[32];
  int length;

  memset(&io_hdr, 0, sizeof(sg_io_hdr_t));
  // See: http://www.tldp.org/HOWTO/SCSI-Generic-HOWTO/sg_io_hdr_t.html
  io_hdr.interface_id = 'S';
  io_hdr.dxfer_direction = SG_DXFER_FROM_DEV;
  io_hdr.cmd_len = sizeof(cmdp);
  io_hdr.mx_sb_len = sizeof(sbp);
  io_hdr.dxfer_len = sizeof(dxferp);
  io_hdr.dxferp = dxferp;
  io_hdr.cmdp = cmdp;
  io_hdr.sbp = sbp;
  io_hdr.timeout = 5000;

  if (ioctl(fd, SG_IO, &io_hdr) != 0) {
    printf("Error: read_serial_number: SG_IO\n");
    return -3;
  }
  if ((io_hdr.info & SG_INFO_OK_MASK) != SG_INFO_OK) {
    printf("Error: read_serial_number: SG_INFO_OK_MASK\n");
    return -4;
  }
  if (io_hdr.masked_status) {
    printf("Error: read_serial_number: status=0x%x\n", io_hdr.status);
    printf("Error: read_serial_number: masked=0x%x\n", io_hdr.masked_status);
    return -5;
  }
  if (io_hdr.host_status) {
    printf("Error: read_serial_number: host=0x%x\n", io_hdr.host_status);
    return -6;
  }
  if (io_hdr.driver_status) {
    printf("Error: read_serial_number: driver=0x%x\n", io_hdr.driver_status);
    return -7;
  }
  if (dxferp[1] != 0x80) {
    printf("Error: read_serial_number: dxferp[1] != 0x80\n");
    return -8;
  }
  length = SERIAL_NUMBER_MAX;
  if (dxferp[3] < length) length = dxferp[3];
  while (serial_number_size < length) {
    serial_number[serial_number_size] = (char) dxferp[4 + serial_number_size];
    serial_number_size++;
  }
  return 0;
#endif
}

class GetBlockDeviceWorker : public Nan::AsyncWorker {
 public:
  GetBlockDeviceWorker(
    const int fd,
    Nan::Callback *callback
  ) : Nan::AsyncWorker(callback),
      fd(fd) {}

  ~GetBlockDeviceWorker() {}

  void Execute() {
#if !defined(_WIN32)
    // WIN32: We know of no way to assert that a handle is a block device.
    struct stat st;
    if (fstat(fd, &st) == -1) {
      return SetErrorMessage("fstat failed");
    }
    // FreeBSD has character devices and no block devices:
    if (
      (st.st_mode & S_IFMT) != S_IFBLK &&
      (st.st_mode & S_IFMT) != S_IFCHR
    ) {
      return SetErrorMessage("fd is not a block or character device");
    }
#endif
#if defined(__APPLE__)
    // https://opensource.apple.com/source/xnu/xnu-1699.26.8/bsd/sys/disk.h
    uint32_t _logical_sector_size;
    uint32_t _physical_sector_size;
    uint64_t _logical_sectors;
    if (ioctl(fd, DKIOCGETBLOCKSIZE, &_logical_sector_size) == -1) {
      return SetErrorMessage("DKIOCGETBLOCKSIZE failed");
    }
    if (ioctl(fd, DKIOCGETPHYSICALBLOCKSIZE, &_physical_sector_size) == -1) {
      return SetErrorMessage("DKIOCGETPHYSICALBLOCKSIZE failed");
    }
    if (ioctl(fd, DKIOCGETBLOCKCOUNT, &_logical_sectors) == -1) {
      return SetErrorMessage("DKIOCGETBLOCKCOUNT failed");
    }
    logical_sector_size = (uint64_t) _logical_sector_size;
    physical_sector_size = (uint64_t) _physical_sector_size;
    size = logical_sector_size * _logical_sectors;
#elif defined(_WIN32)
    HANDLE handle = uv_get_osfhandle(fd);
    if (handle == INVALID_HANDLE_VALUE) {
      return SetErrorMessage("EBADF: bad file descriptor");
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
      logical_sector_size = (uint64_t) alignment.BytesPerLogicalSector;
      physical_sector_size = (uint64_t) alignment.BytesPerPhysicalSector;
    } else {
      return SetErrorMessage("IOCTL_STORAGE_QUERY_PROPERTY failed");
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
      size = static_cast<uint64_t>(geometry.DiskSize.QuadPart);
    } else {
      return SetErrorMessage("IOCTL_DISK_GET_DRIVE_GEOMETRY_EX failed");
    }
#elif defined(__FreeBSD__) || defined(__FreeBSD_kernel_)
    // https://github.com/freebsd/freebsd/blob/master/sys/sys/disk.h
    unsigned int _logical_sector_size;
    off_t _physical_sector_size;
    off_t _size;
    if (ioctl(fd, DIOCGSECTORSIZE, &_logical_sector_size) == -1) {
      return SetErrorMessage("DIOCGSECTORSIZE failed");
    }
    if (ioctl(fd, DIOCGSTRIPESIZE, &_physical_sector_size) == -1) {
      return SetErrorMessage("DIOCGSTRIPESIZE failed");
    }
    if (ioctl(fd, DIOCGMEDIASIZE, &_size) == -1) {
      return SetErrorMessage("DIOCGMEDIASIZE failed");
    }
    logical_sector_size = (uint64_t) _logical_sector_size;
    physical_sector_size = (uint64_t) _physical_sector_size;
    size = (uint64_t) _size;
#else
    // https://github.com/torvalds/linux/blob/master/block/ioctl.c
    // We must use the correct type according to the control code passed:
    // https://stackoverflow.com/questions/19747663/where-are-ioctl-
    // parameters-such-as-0x1268-blksszget-actually-specified
    int _logical_sector_size;
    unsigned int _physical_sector_size;
    if (ioctl(fd, BLKSSZGET, &_logical_sector_size) == -1) {
      return SetErrorMessage("BLKSSZGET failed");
    }
    if (ioctl(fd, BLKPBSZGET, &_physical_sector_size) == -1) {
      return SetErrorMessage("BLKPBSZGET failed");
    }
    // The kernel expects size to be a u64 and defines u64 as uint64_t:
    // https://github.com/torvalds/linux/blob/master/tools/include/linux/types.h
    if (ioctl(fd, BLKGETSIZE64, &size) == -1) {
      return SetErrorMessage("BLKGETSIZE64 failed");
    }
    logical_sector_size = (uint64_t) _logical_sector_size;
    physical_sector_size = (uint64_t) _physical_sector_size;
#endif
    if (read_serial_number(fd, serial_number, serial_number_size) != 0) {
      return SetErrorMessage("read_serial_number failed");
    }
  }

  void HandleOKCallback () {
    Nan::HandleScope scope;
    v8::Local<v8::Object> device = Nan::New<v8::Object>();
    Nan::Set(
      device,
      Nan::New<v8::String>("logicalSectorSize").ToLocalChecked(),
      Nan::New<v8::Number>(static_cast<double>(logical_sector_size))
    );
    Nan::Set(
      device,
      Nan::New<v8::String>("physicalSectorSize").ToLocalChecked(),
      Nan::New<v8::Number>(static_cast<double>(physical_sector_size))
    );
    Nan::Set(
      device,
      Nan::New<v8::String>("serialNumber").ToLocalChecked(),
      Nan::New<v8::String>(serial_number, serial_number_size).ToLocalChecked()
    );
    Nan::Set(
      device,
      Nan::New<v8::String>("size").ToLocalChecked(),
      Nan::New<v8::Number>(static_cast<double>(size))
    );
    v8::Local<v8::Value> argv[] = {
      Nan::Undefined(),
      device
    };
    callback->Call(2, argv, async_resource);
  }

 private:
  const int fd;
  uint64_t logical_sector_size;
  uint64_t physical_sector_size;
  uint64_t size;
  char serial_number[SERIAL_NUMBER_MAX];
  int serial_number_size = 0;
};

#if defined(__APPLE__)
class SetF_NOCACHEWorker : public Nan::AsyncWorker {
 public:
  SetF_NOCACHEWorker(
    const int fd,
    const int value,
    Nan::Callback *callback
  ) : Nan::AsyncWorker(callback),
      fd(fd),
      value(value) {}

  ~SetF_NOCACHEWorker() {}

  void Execute() {
    if (fcntl(fd, F_NOCACHE, value) != 0) {
      if (errno == EBADF) {
        SetErrorMessage("EBADF: bad file descriptor, fcntl");
      } else {
        SetErrorMessage("unexpected error, fcntl");
      }
    };
  }

  void HandleOKCallback() {
    Nan::HandleScope scope;
    v8::Local<v8::Value> argv[] = {
      Nan::Undefined()
    };
    callback->Call(1, argv, async_resource);
  }

 private:
  const int fd;
  const int value;
};
#endif

#if !defined(_WIN32)
class SetFlockWorker : public Nan::AsyncWorker {
 public:
  SetFlockWorker(
    const int fd,
    const int value,
    Nan::Callback *callback
  ) : Nan::AsyncWorker(callback),
      fd(fd),
      value(value) {}

  ~SetFlockWorker() {}

  void Execute() {
    // Place an exclusive lock. Only one process may hold an exclusive lock for
    // a given file at a given time. A call to flock() may block if an
    // incompatible lock is held by another process. We use LOCK_NB to make a
    // non-blocking request.
    int result = flock(fd, value == 0 ? LOCK_UN : LOCK_EX | LOCK_NB);
    if (result != 0) {
      if (errno == EWOULDBLOCK) {
        SetErrorMessage("EWOULDBLOCK, the file is already locked");
      } else if (errno == EBADF) {
        SetErrorMessage("EBADF, fd is an invalid file descriptor");
      } else if (errno == EINTR) {
        SetErrorMessage("EINTR, the call was interrupted by a signal");
      } else if (errno == EINVAL) {
        SetErrorMessage("EINVAL, fd does not refer to a file");
      } else if (errno == ENOTSUP) {
        SetErrorMessage("ENOTSUP, fd is not of the correct type");
      } else {
        SetErrorMessage("unable to obtain an exclusive lock");
      }
    }
  }

  void HandleOKCallback () {
    Nan::HandleScope scope;
    v8::Local<v8::Value> argv[] = {
      Nan::Undefined()
    };
    callback->Call(1, argv, async_resource);
  }

 private:
  const int fd;
  const int value;
};
#endif

#if defined(_WIN32)
class SetFSCTL_LOCK_VOLUMEWorker : public Nan::AsyncWorker {
 public:
  SetFSCTL_LOCK_VOLUMEWorker(
    const int fd,
    const int value,
    Nan::Callback *callback
  ) : Nan::AsyncWorker(callback),
      fd(fd),
      value(value) {}

  ~SetFSCTL_LOCK_VOLUMEWorker() {}

  void Execute() {
    HANDLE handle = uv_get_osfhandle(fd);
    if (handle == INVALID_HANDLE_VALUE) {
      return SetErrorMessage("EBADF: bad file descriptor");
    }
    DWORD bytes;
    if (
      !DeviceIoControl(
        handle,
        value ? FSCTL_LOCK_VOLUME : FSCTL_UNLOCK_VOLUME,
        NULL,
        0,
        NULL,
        0,
        &bytes,
        NULL
      )
    ) {
      if (value) {
        return SetErrorMessage("FSCTL_LOCK_VOLUME failed");
      } else {
        return SetErrorMessage("FSCTL_UNLOCK_VOLUME failed");
      }
    }
  }

  void HandleOKCallback() {
    Nan::HandleScope scope;
    v8::Local<v8::Value> argv[] = {
      Nan::Undefined()
    };
    callback->Call(1, argv, async_resource);
  }

 private:
  const int fd;
  const int value;
};
#endif

NAN_METHOD(getAlignedBuffer) {
  if (
    info.Length() != 2 ||
    !info[0]->IsUint32() ||
    !info[1]->IsUint32()
  ) {
    return Nan::ThrowError(
      "bad arguments, expected: (uint32 size, uint32 alignment)"
    );
  }
  const size_t size = Nan::To<uint32_t>(info[0]).FromJust();
  const size_t alignment = Nan::To<uint32_t>(info[1]).FromJust();
  if (size == 0) return Nan::ThrowError("size must not be 0");
  if (alignment == 0) return Nan::ThrowError("alignment must not be 0");
  if (alignment & (alignment - 1)) {
    return Nan::ThrowError("alignment must be a power of 2");
  }
  if (alignment % sizeof(void *)) {
    return Nan::ThrowError("alignment must be a multiple of pointer size");
  }
#if defined(_WIN32)
  void *ptr = _aligned_malloc(size, alignment);
  if (ptr == NULL) return Nan::ThrowError("insufficient memory");
#else
  void *ptr;
  int result = posix_memalign(&ptr, alignment, size);
  if (result == EINVAL) return Nan::ThrowError("bad alignment argument");
  if (result == ENOMEM) return Nan::ThrowError("insufficient memory");
  if (result != 0) return Nan::ThrowError("unexpected error, posix_memalign");
#endif
  info.GetReturnValue().Set(
    Nan::NewBuffer(
      (char *) ptr,
      size,
      free_aligned_buffer,
      NULL
    ).ToLocalChecked()
  );
}

NAN_METHOD(getBlockDevice) {
  if (
    info.Length() != 2 ||
    !info[0]->IsUint32() ||
    !info[1]->IsFunction()
  ) {
    return Nan::ThrowError(
      "bad arguments, expected: (uint32 fd, function callback)"
    );
  }
  const int fd = Nan::To<uint32_t>(info[0]).FromJust();
  Nan::Callback *callback = new Nan::Callback(info[1].As<v8::Function>());
  Nan::AsyncQueueWorker(new GetBlockDeviceWorker(fd, callback));
}

NAN_METHOD(setF_NOCACHE) {
#if defined(__APPLE__)
  if (
    info.Length() != 3 ||
    !info[0]->IsUint32() ||
    !info[1]->IsUint32() ||
    !info[2]->IsFunction()
  ) {
    return Nan::ThrowError(
      "bad arguments, expected: (uint32 fd, uint32 value, function callback)"
    );
  }
  const int fd = Nan::To<uint32_t>(info[0]).FromJust();
  const int value = Nan::To<uint32_t>(info[1]).FromJust();
  if (value != 0 && value != 1) return Nan::ThrowError("value must be 0 or 1");
  Nan::Callback *callback = new Nan::Callback(info[2].As<v8::Function>());
  Nan::AsyncQueueWorker(new SetF_NOCACHEWorker(fd, value, callback));
#else
  return Nan::ThrowError("only supported on mac os");
#endif
}

NAN_METHOD(setFlock) {
#if defined(_WIN32)
  return Nan::ThrowError("not supported on windows");
#else
  if (
    info.Length() != 3 ||
    !info[0]->IsUint32() ||
    !info[1]->IsUint32() ||
    !info[2]->IsFunction()
  ) {
    return Nan::ThrowError(
      "bad arguments, expected: (uint32 fd, uint32 value, function callback)"
    );
  }
  const int fd = Nan::To<uint32_t>(info[0]).FromJust();
  const int value = Nan::To<uint32_t>(info[1]).FromJust();
  if (value != 0 && value != 1) return Nan::ThrowError("value must be 0 or 1");
  Nan::Callback *callback = new Nan::Callback(info[2].As<v8::Function>());
  Nan::AsyncQueueWorker(new SetFlockWorker(fd, value, callback));
#endif
}

NAN_METHOD(setFSCTL_LOCK_VOLUME) {
#if defined(_WIN32)
  if (
    info.Length() != 3 ||
    !info[0]->IsUint32() ||
    !info[1]->IsUint32() ||
    !info[2]->IsFunction()
  ) {
    return Nan::ThrowError(
      "bad arguments, expected: (uint32 fd, uint32 value, function callback)"
    );
  }
  const int fd = Nan::To<uint32_t>(info[0]).FromJust();
  const int value = Nan::To<uint32_t>(info[1]).FromJust();
  if (value != 0 && value != 1) return Nan::ThrowError("value must be 0 or 1");
  Nan::Callback *callback = new Nan::Callback(info[2].As<v8::Function>());
  Nan::AsyncQueueWorker(new SetFSCTL_LOCK_VOLUMEWorker(fd, value, callback));
#else
  return Nan::ThrowError("only supported on windows");
#endif
}

NAN_MODULE_INIT(Init) {
// On Windows, libuv maps these flags as follows:
// UV_FS_O_DIRECT  >  FILE_FLAG_NO_BUFFERING
// UV_FS_O_DSYNC   >  FILE_FLAG_WRITE_THROUGH
// UV_FS_O_EXLOCK  >  SHARING MODE = 0
// UV_FS_O_SYNC    >  FILE_FLAG_WRITE_THROUGH
#if defined(_WIN32) && defined(UV_FS_O_DIRECT) && !defined(O_DIRECT)
# define O_DIRECT  UV_FS_O_DIRECT
#endif
#if defined(_WIN32) && defined(UV_FS_O_DSYNC) && !defined(O_DSYNC)
# define O_DSYNC   UV_FS_O_DSYNC
#endif
#if defined(_WIN32) && defined(UV_FS_O_EXLOCK) && !defined(O_EXLOCK)
# define O_EXLOCK  UV_FS_O_EXLOCK
#endif
#if defined(_WIN32) && defined(UV_FS_O_SYNC) && !defined(O_SYNC)
# define O_SYNC    UV_FS_O_SYNC
#endif

#if defined(O_DIRECT)
  NODE_DEFINE_CONSTANT(target, O_DIRECT);
#endif
#if defined(O_DSYNC)
  NODE_DEFINE_CONSTANT(target, O_DSYNC);
#endif
#if defined(O_EXCL) && !defined(_WIN32)
  NODE_DEFINE_CONSTANT(target, O_EXCL);
#endif
#if defined(O_EXLOCK)
  NODE_DEFINE_CONSTANT(target, O_EXLOCK);
#endif
#if defined(O_SYNC)
  NODE_DEFINE_CONSTANT(target, O_SYNC);
#endif
  NAN_EXPORT(target, getAlignedBuffer);
  NAN_EXPORT(target, getBlockDevice);
  NAN_EXPORT(target, setF_NOCACHE);
  NAN_EXPORT(target, setFlock);
  NAN_EXPORT(target, setFSCTL_LOCK_VOLUME);
}

NODE_MODULE(binding, Init)

// S.D.G.
