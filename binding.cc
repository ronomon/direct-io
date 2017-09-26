#include <nan.h>
#include <stdint.h>
#if defined(__APPLE__)
#include <fcntl.h>
#include <sys/disk.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#elif defined(_WIN32)
#include <io.h>
#include <malloc.h>
#include <winioctl.h>
#else
#include <errno.h>
#include <linux/fs.h>
#include <stdlib.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#endif

void free_aligned_buffer(char *buffer, void *hint) {
#if defined(_WIN32)
  _aligned_free(buffer);
#else
  free(buffer);
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
    if ((st.st_mode & S_IFMT) != S_IFBLK) {
      return SetErrorMessage("fd is not a block device");
    }
#endif
#if defined(__APPLE__)
    if (ioctl(fd, DKIOCGETBLOCKSIZE, &logical_sector_size) == -1) {
      return SetErrorMessage("DKIOCGETBLOCKSIZE failed"); 
    }
    if (ioctl(fd, DKIOCGETPHYSICALBLOCKSIZE, &physical_sector_size) == -1) {
      return SetErrorMessage("DKIOCGETPHYSICALBLOCKSIZE failed");
    }
    uint64_t logical_sectors = 0;
    if (ioctl(fd, DKIOCGETBLOCKCOUNT, &logical_sectors) == -1) {
      return SetErrorMessage("DKIOCGETBLOCKCOUNT failed");
    }
    size = logical_sector_size * logical_sectors;
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
      logical_sector_size = (uint32_t) alignment.BytesPerLogicalSector;
      physical_sector_size = (uint32_t) alignment.BytesPerPhysicalSector;
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
    // See: pipermail/freebsd-hackers/2016-September/049974.html
    if (ioctl(fd, DIOCGSECTORSIZE, &logical_sector_size) == -1) {
      return SetErrorMessage("DIOCGSECTORSIZE failed");
    }
    if (ioctl(fd, DIOCGSTRIPESIZE, &physical_sector_size) == -1) {
      return SetErrorMessage("DIOCGSTRIPESIZE failed");
    }
    if (ioctl(fd, DIOCGMEDIASIZE, &size) == -1) {
      return SetErrorMessage("DIOCGMEDIASIZE failed");
    }
#else
    if (ioctl(fd, BLKSSZGET, &logical_sector_size) == -1) {
      return SetErrorMessage("BLKSSZGET failed");
    }
    if (ioctl(fd, BLKPSZGET, &physical_sector_size) == -1) {
      return SetErrorMessage("BLKPSZGET failed");
    }
    if (ioctl(fd, BLKGETSIZE64, &size) == -1) {
      return SetErrorMessage("BLKGETSIZE64 failed");
    }
#endif
  }

  void HandleOKCallback () {
    Nan::HandleScope scope;
    v8::Local<v8::Object> device = Nan::New<v8::Object>();
    Nan::Set(
      device,
      Nan::New<v8::String>("logicalSectorSize").ToLocalChecked(),
      Nan::New<v8::Number>(logical_sector_size)
    );
    Nan::Set(
      device,
      Nan::New<v8::String>("physicalSectorSize").ToLocalChecked(),
      Nan::New<v8::Number>(physical_sector_size)
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
    callback->Call(2, argv);
  }

 private:
  const int fd;
  uint32_t logical_sector_size;
  uint32_t physical_sector_size;
  uint64_t size;
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
    callback->Call(1, argv);
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
    callback->Call(1, argv);
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
  const size_t size = info[0]->Uint32Value();
  const size_t alignment = info[1]->Uint32Value();
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
  const int fd = info[0]->Uint32Value();
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
  const int fd = info[0]->Uint32Value();
  const int value = info[1]->Uint32Value();
  if (value != 0 && value != 1) return Nan::ThrowError("value must be 0 or 1");
  Nan::Callback *callback = new Nan::Callback(info[2].As<v8::Function>());
  Nan::AsyncQueueWorker(new SetF_NOCACHEWorker(fd, value, callback));
#else
  return Nan::ThrowError("only supported on mac os");
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
  const int fd = info[0]->Uint32Value();
  const int value = info[1]->Uint32Value();
  if (value != 0 && value != 1) return Nan::ThrowError("value must be 0 or 1");
  Nan::Callback *callback = new Nan::Callback(info[2].As<v8::Function>());
  Nan::AsyncQueueWorker(new SetFSCTL_LOCK_VOLUMEWorker(fd, value, callback));
#else
  return Nan::ThrowError("only supported on windows");
#endif
}

NAN_MODULE_INIT(Init) {
#if defined(O_DIRECT)
  NODE_DEFINE_CONSTANT(target, O_DIRECT);
#endif
#if defined(O_DSYNC)
  NODE_DEFINE_CONSTANT(target, O_DSYNC);
#endif
#if defined(O_EXCL)
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
  NAN_EXPORT(target, setFSCTL_LOCK_VOLUME);
}

NODE_MODULE(binding, Init)

// S.D.G.
