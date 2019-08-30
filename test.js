var assert = require('assert');
var binding = require('.');

var Node = {
  fs: require('fs'),
  process: process
};

[
  'O_DIRECT',
  'O_DSYNC',
  'O_EXCL',
  'O_EXLOCK',
  'O_SYNC'
].forEach(
  function(key) {
    var value = binding[key];
    assert(value !== undefined);
    assert(Number.isSafeInteger(value));
    assert(value >= 0);
    console.log('PASS: Constant: ' + key);
  }
);

if (Node.process.platform !== 'darwin') {
  // On Linux, prevent regressions: https://github.com/libuv/libuv/issues/2420
  // On Windows, we expect O_DIRECT to be mapped to FILE_FLAG_NO_BUFFERING.
  assert(binding.O_DIRECT > 0);
}
if (Node.process.platform === 'darwin' || Node.process.platform === 'win32') {
  assert(binding.O_EXLOCK > 0);
} else {
  assert(binding.O_EXCL > 0);
}
assert(binding.O_DSYNC > 0);
assert(binding.O_SYNC > 0);

[
  'getAlignedBuffer',
  'getBlockDevice',
  'setF_NOCACHE',
  'setFlock',
  'setFSCTL_LOCK_VOLUME'
].forEach(
  function(key) {
    var value = binding[key];
    assert(value !== undefined);
    assert(typeof value === 'function');
    console.log('PASS: Function: ' + key + '()');
  }
);

function exception(method, message, set) {
  set.forEach(
    function(args) {
      var nameArgs = args.map(
        function(arg) {
          if (arg === undefined) return 'undefined';
          if (typeof arg === 'function') return 'function';
          return JSON.stringify(arg);
        }
      );
      var name = method + '(' + nameArgs.join(', ') + '): ';
      try {
        binding[method](...args);
      } catch (error) {
        assert(error.message === message);
        console.log('PASS: ' + name + JSON.stringify(message));
        return;
      }
      throw new Error('FAIL: ' + name + 'Expected ' + JSON.stringify(message));
    }
  );
}

exception(
  'getAlignedBuffer',
  'bad arguments, expected: (size, alignment)',
  [
    [],
    [8],
    [undefined, 8],
    [undefined + 8, 8],
    [-1, 8],
    [8, -1],
    [8.1, 8],
    [8, 8.1],
    [8, 8, 8],
    [Math.pow(2, 31), 8]
  ]
);
exception('getAlignedBuffer', 'size must not be 0', [[0, 8]]);
exception('getAlignedBuffer', 'alignment must not be 0', [[8, 0]]);
exception(
  'getAlignedBuffer',
  'alignment must be a power of 2',
  [
    [8, 9], [8, 10], [8, 11], [8, 12], [8, 13], [8, 14], [8, 15], [8, 17]
  ]
);
exception(
  'getAlignedBuffer',
  'alignment must be at least 8 bytes',
  [
    [8, 1], [8, 2], [8, 4]
  ]
);
exception(
  'getAlignedBuffer',
  'alignment must be at most 4194304 bytes',
  [[8, 8 * 1024 * 1024]]
);

exception(
  'getBlockDevice',
  'bad arguments, expected: (fd, callback)',
  [
    [],
    [undefined, function() {}],
    [undefined + 1, function() {}],
    [-1, function() {}],
    [1.1, function() {}],
    [Math.pow(2, 31), function() {}],
    [1],
    [1, {}],
    [1, null],
    [1, undefined],
    [1, function() {}, function() {}]
  ]
);

if (Node.process.platform !== 'darwin') {
  exception('setF_NOCACHE', 'only supported on mac os', [[]]);
}
if (Node.process.platform === 'win32') {
  exception('setFlock', 'not supported on windows', [[]]);
}
if (Node.process.platform !== 'win32') {
  exception('setFSCTL_LOCK_VOLUME', 'only supported on windows', [[]]);
}

(function() {
  var methods = [];
  if (Node.process.platform === 'darwin') methods.push('setF_NOCACHE');
  if (Node.process.platform !== 'win32') methods.push('setFlock');
  if (Node.process.platform === 'win32') methods.push('setFSCTL_LOCK_VOLUME');
  methods.forEach(
    function(method) {
      exception(
        method,
        'bad arguments, expected: (fd, value=0/1, callback)',
        [
          [],
          [1],
          
          [undefined, 0, function() {}],
          [undefined + 1, 0, function() {}],
          [-1, 0, function() {}],
          [1.1, 0, function() {}],
          [Math.pow(2, 31), 0, function() {}],

          [1, undefined, function() {}],
          [1, undefined + 1, function() {}],
          [1, -1, function() {}],
          [1, 1.1, function() {}],
          [1, 2, function() {}],
          [1, Math.pow(2, 31), function() {}],

          [1, 0],
          [1, 0, {}],
          [1, 0, null],
          [1, 0, undefined],
          [1, 0, function() {}, function() {}]
        ]
      );
    }
  );
})();

function getAlignedBuffer(size, alignment) {
  var buffer = binding.getAlignedBuffer(size, alignment);
  assert(Buffer.isBuffer(buffer));
  assert(buffer.length === size);
  var size = 0;
  var zero = 0;
  for (var index = 0, length = buffer.length; index < length; index++) {
    size++;
    zero += buffer[index];
  }
  assert(size === buffer.length);
  assert(zero === 0);
  // Force physical page allocation if only allocated virtually:
  for (var page = 0, length = buffer.length; page < length; page += 4096) {
    buffer[page] = 255;
  }
  console.log('PASS: getAlignedBuffer(' + size + ', ' + alignment + ')');
}

[1, 2, 3, 4, 5, 6, 7, 8, 4095, 4096, 4097, 1048576, 4194304].forEach(
  function(size) {
    [
      8,
      16,
      32,
      64,
      128,
      256,
      512,
      1024,
      2048,
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
      4194304
    ].forEach(
      function(alignment) {
        getAlignedBuffer(size, alignment);
      }
    );
  }
);

(function() {
  if (Node.process.platform === 'win32') return;
  var fd1 = Node.fs.openSync(module.filename, 'r');
  binding.getBlockDevice(fd1,
    function(error) {
      assert(error !== undefined);
      assert(error.message === 'fd is not a block or character device');
      console.log(
        'PASS: getBlockDevice(fd1): ' + JSON.stringify(error.message)
      );
    }
  );
})();

(function() {
  if (Node.process.platform !== 'darwin') return;
  var fd1 = Node.fs.openSync(module.filename, 'r');
  binding.setF_NOCACHE(fd1, 1,
    function(error) {
      assert(error === undefined);
      console.log('PASS: setF_NOCACHE(fd1, 1)');
      binding.setF_NOCACHE(fd1, 0,
        function(error) {
          assert(error === undefined);
          console.log('PASS: setF_NOCACHE(fd1, 0)');
        }
      );
    }
  );
})();

(function() {
  if (Node.process.platform === 'win32') return;
  var fd1 = Node.fs.openSync(module.filename, 'r');
  var fd2 = Node.fs.openSync(module.filename, 'r');
  binding.setFlock(fd1, 0,
    function(error) {
      assert(error === undefined);
      console.log('PASS: setFlock(fd1, 0)');
      binding.setFlock(fd1, 1,
        function(error) {
          assert(error === undefined);
          console.log('PASS: setFlock(fd1, 1)');
          binding.setFlock(fd2, 1,
            function(error) {
              assert(error !== undefined);
              assert(
                error.message === 'EWOULDBLOCK, the file is already locked'
              );
              console.log(
                'PASS: setFlock(fd2, 1): ' + JSON.stringify(error.message)
              );
              binding.setFlock(fd1, 0,
                function(error) {
                  assert(error === undefined);
                  console.log('PASS: setFlock(fd1, 0)');
                  binding.setFlock(fd2, 1,
                    function(error) {
                      assert(error === undefined);
                      console.log('PASS: setFlock(fd2, 1)');
                      binding.setFlock(fd2, 0,
                        function(error) {
                          assert(error === undefined);
                          console.log('PASS: setFlock(fd2, 0)');
                        }
                      );
                    }
                  );
                }
              );
            }
          );
        }
      );
    }
  );
})();
