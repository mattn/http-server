# http-server

A fast http server written in C.

## Usage

```
$ ./http-server
```

## Requirements

* [libuv](https://github.com/joyent/libuv)

## Installation

```
$ make
```

# Benchmark

## Windows(Core i5/3Hz)

$ ab -k -c 10 -n 10000 http://127.0.0.1:7000/


# License

MIT

# Author

Yasuhiro Matsumoto (a.k.a mattn)
