var Node = {
  fs: require('fs'),
  path: require('path')
};

var Queue = require('@ronomon/queue');
var binding = require('.');

const ALIGNED = 1;
const AMORTIZED_FDATASYNC = 2;
const AMORTIZED_FSYNC = 4;
const BUFFERED = 8;
const FDATASYNC = 16;
const FSYNC = 32;
const O_DIRECT = 64;
const O_DSYNC = 128;
const O_SYNC = 256;

const PATH_DEFAULT = Node.path.resolve(module.filename, '..', 'file');

const SIZE = 128 * 1024 * 1024;

const BLOCKS = [
  4096,
  8192,
  16384,
  32768,
  65536,
  131072,
  262144,
  524288,
  1048576,
  2097152,
  4194304,
  8388608,
  16777216,
  33554432
];

const FLAGS = [
  BUFFERED,
  BUFFERED | ALIGNED,
  O_DIRECT,
  AMORTIZED_FDATASYNC,
  AMORTIZED_FDATASYNC | ALIGNED,
  AMORTIZED_FDATASYNC | O_DIRECT,
  AMORTIZED_FSYNC,
  AMORTIZED_FSYNC | ALIGNED,
  AMORTIZED_FSYNC | O_DIRECT,
  FDATASYNC,
  FDATASYNC | ALIGNED,
  FDATASYNC | O_DIRECT,
  FSYNC,
  FSYNC | ALIGNED,
  FSYNC | O_DIRECT,
  O_DSYNC,
  O_DSYNC | ALIGNED,
  O_DSYNC | O_DIRECT,
  O_SYNC,
  O_SYNC | ALIGNED,
  O_SYNC | O_DIRECT
];

var BLOCK_MIN = BLOCKS[0];
var BLOCK_MAX = BLOCKS[BLOCKS.length - 1];

var args = process.argv.slice(2);
var argsIndex = 0;
while (argsIndex < args.length) {
  var arg = args[argsIndex];
  if (/^--/.test(arg)) {
    if (/^--block-(max|min)=\d+$/.test(arg)) {
      var value = parseInt(arg.split('=')[1], 10);
      if (value < BLOCK_MIN) throw new Error(arg + ' < BLOCK_MIN=' + BLOCK_MIN);
      if (value > BLOCK_MAX) throw new Error(arg + ' > BLOCK_MAX=' + BLOCK_MAX);
      if (/^--block-max=\d+$/.test(arg)) {
        BLOCK_MAX = value;
      } else {
        BLOCK_MIN = value;
      }
      args.splice(argsIndex, 1);
    } else {
      throw new Error('unsupported arg: ' + arg);
    }
  } else {
    argsIndex++;
  }
}

var path = args.pop() || PATH_DEFAULT;

function open(path, options, end) {
  var flags = Node.fs.constants.O_RDWR;
  var type = parseType(path);
  if (type === 1) flags |= Node.fs.constants.O_CREAT;
  if (options.flags & O_DIRECT) flags |= binding.O_DIRECT;
  if (options.flags & O_DSYNC) flags |= binding.O_DSYNC;
  if (options.flags & O_SYNC) flags |= binding.O_SYNC;
  if (type === 0) {
    if (process.platform === 'darwin') {
      flags |= binding.O_EXLOCK;
    } else if (process.platform !== 'win32') {
      flags |= binding.O_EXCL;
    }
  }
  Node.fs.open(path, flags,
    function(error, fd) {
      if (error) return end(error);
      function close(error) {
        Node.fs.closeSync(fd);
        end(error);
      }
      if ((options.flags & O_DIRECT) && process.platform === 'darwin') {
        binding.setF_NOCACHE(fd, 1,
          function(error) {
            if (error) return close(error);
            end(undefined, fd);
          }
        );
      } else if (type === 0 && process.platform === 'win32') {
        binding.setFSCTL_LOCK_VOLUME(fd, 1,
          function(error) {
            if (error) return close(error);
            end(undefined, fd);
          }
        );
      } else {
        end(undefined, fd);
      }
    }
  );
}

function padL(value, length) {
  var string = String(value);
  while (string.length < length) string = ' ' + string;
  return string;
}

function padR(value, length) {
  var string = String(value);
  while (string.length < length) string += ' ';
  return string;
}

function parseType(path) {
  if (process.platform === 'win32') {
    if (/^\\\\\.\\.+/.test(path)) return 0;
    return 1;
  } else {
    if (/^\/dev\/.+/i.test(path)) return 0;
    return 1;
  }
}

var bufferUnaligned = Buffer.alloc(1 + SIZE, 255).slice(1, 1 + SIZE);
var bufferAligned = binding.getAlignedBuffer(SIZE, 4096);
var length = bufferAligned.length;
while (length--) bufferAligned[length] = 255;

var queue = new Queue();
queue.onData = function(options, end) {
  var type = parseType(path);
  open(path, options,
    function(error, fd) {
      if (error) return end(error);
      if (options.flags & (ALIGNED | O_DIRECT)) {
        var buffer = bufferAligned;
      } else {
        var buffer = bufferUnaligned;
      }
      var position = 0;
      var previous = 0;
      var blocks = Math.ceil(SIZE / options.block);
      if (options.flags & (FDATASYNC | FSYNC | O_DSYNC | O_SYNC)) {
        blocks = Math.min(options.block / 32, blocks);
      }
      var now = Date.now();
      while (blocks--) {
        position += Node.fs.writeSync(
          fd,
          buffer,
          0,
          options.block,
          position
        );
        if (position - previous !== options.block) {
          throw new Error('position did not advance by a block');
        }
        previous = position;
        if (options.flags & FDATASYNC) Node.fs.fdatasyncSync(fd);
        if (options.flags & FSYNC) Node.fs.fsyncSync(fd);
      }
      if (options.flags & AMORTIZED_FDATASYNC) Node.fs.fdatasyncSync(fd);
      if (options.flags & AMORTIZED_FSYNC) Node.fs.fsyncSync(fd);
      var time = Date.now() - now;
      var throughput = ((position / (1024 * 1024)) / (time / 1000)).toFixed(2);
      Node.fs.fdatasyncSync(fd);
      Node.fs.closeSync(fd);
      var result = [];
      result.push(padL(options.block, 10));
      result.push(Node.path.basename(path) === 'file' ? 'file' : path);
      var type = [];
      if (options.flags & AMORTIZED_FDATASYNC) type.push('AMORTIZED_FDATASYNC');
      if (options.flags & AMORTIZED_FSYNC) type.push('AMORTIZED_FSYNC');
      if (options.flags & BUFFERED) type.push('BUFFERED');
      if (options.flags & FDATASYNC) type.push('FDATASYNC');
      if (options.flags & FSYNC) type.push('FSYNC');
      if (options.flags & O_DSYNC) type.push('O_DSYNC');
      if (options.flags & O_SYNC) type.push('O_SYNC');
      if (options.flags & O_DIRECT) type.push('O_DIRECT');
      if (options.flags & ALIGNED) type.push('ALIGNED');
      result.push(padR(type.join(' + '), 38));
      result.push(padL(throughput, 8) + ' MB/s');
      console.log(result.join(' | '));
      end();
    }
  );
};
queue.onEnd = function(error) {
  if (path === PATH_DEFAULT) {
    try {
      Node.fs.unlinkSync(path);
    } catch (unlinkError) {
      if (!error) throw unlinkError;
    }
  }
  if (error) throw error;
};
BLOCKS.forEach(
  function(block) {
    if (block < BLOCK_MIN) return;
    if (block > BLOCK_MAX) return;
    FLAGS.forEach(
      function(flags) {
        if (flags & O_DIRECT) {
          if (!binding.O_DIRECT && process.platform !== 'darwin') return;
        }
        if ((flags & O_DSYNC) && !binding.O_DSYNC) return;
        if ((flags & O_SYNC) && !binding.O_SYNC) return;
        queue.push({ block: block, path: path, flags: flags });
      }
    );
  }
);
queue.end();
