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

## Benchmark

### WSL2/Linux(AMD Ryzen 7 7735HS)

$ ab -k -c 100 -n 100000 http://127.0.0.1:7000/index.html (6KB document)

```
Server Software:
Server Hostname:        127.0.0.1
Server Port:            7000

Document Path:          /index.html
Document Length:        6026 bytes

Concurrency Level:      100
Time taken for tests:   1.212 seconds
Complete requests:      100000
Failed requests:        0
Keep-Alive requests:    100000
Total transferred:      611600000 bytes
HTML transferred:       602600000 bytes
Requests per second:    82519.69 [#/sec] (mean)
Time per request:       1.212 [ms] (mean)
Time per request:       0.012 [ms] (mean, across all concurrent requests)
Transfer rate:          492861.74 [Kbytes/sec] received

Connection Times (ms)
              min  mean[+/-sd] median   max
Connect:        0    0   0.2      0       9
Processing:     0    1   0.4      1       9
Waiting:        0    1   0.4      1       4
Total:          0    1   0.4      1      10

Percentage of the requests served within a certain time (ms)
  50%      1
  66%      1
  75%      1
  80%      1
  90%      2
  95%      2
  98%      2
  99%      3
 100%     10 (longest request)
```

## License

MIT

## Author

Yasuhiro Matsumoto (a.k.a mattn)
