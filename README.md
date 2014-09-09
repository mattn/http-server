# http-server

A fast http server written in C.

## Usage

```
$ ./http-server
```

## Requirements

* [libuv](https://github.com/joyent/libuv)
* [cmake](http://www.cmake.org/)

## Installation

```
$ mkdir build && cd build && cmake .. && make
```

# Benchmark

## Windows(Core i5/3Hz)

```
$ ab -k -c 10 -n 10000 http://127.0.0.1:7000/
Server Software:
Server Hostname:        localhost
Server Port:            7000

Document Path:          /
Document Length:        12 bytes

Concurrency Level:      40
Time taken for tests:   5.130513 seconds
Complete requests:      100000
Failed requests:        0
Write errors:           0
Keep-Alive requests:    100000
Total transferred:      10001004 bytes
HTML transferred:       1200036 bytes
Requests per second:    19491.23 [#/sec] (mean)
Time per request:       2.052 [ms] (mean)
Time per request:       0.051 [ms] (mean, across all concurrent requests)
Transfer rate:          1903.51 [Kbytes/sec] received

Connection Times (ms)
              min  mean[+/-sd] median   max
Connect:        0    0   0.0      0       1
Processing:     1    2   0.3      2      11
Waiting:        0    1   0.7      1      11
Total:          1    2   0.3      2      12

Percentage of the requests served within a certain time (ms)
  50%      2
  66%      2
  75%      2
  80%      2
  90%      2
  95%      3
  98%      3
  99%      3
 100%     12 (longest request)
```

# License

MIT

# Author

Yasuhiro Matsumoto (a.k.a mattn)
