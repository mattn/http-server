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

$ ab -k -c 10 -n 10000 http://127.0.0.1:7000/

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

### Linux(Xeon 1.80GHz)

```
Server Software:
Server Hostname:        127.0.0.1
Server Port:            7000

Document Path:          /
Document Length:        12 bytes

Concurrency Level:      100
Time taken for tests:   3.981 seconds
Complete requests:      100000
Failed requests:        0
Write errors:           0
Keep-Alive requests:    100000
Total transferred:      10002540 bytes
HTML transferred:       1200252 bytes
Requests per second:    25121.37 [#/sec] (mean)
Time per request:       3.981 [ms] (mean)
Time per request:       0.040 [ms] (mean, across all concurrent requests)
Transfer rate:          2453.88 [Kbytes/sec] received

Connection Times (ms)
              min  mean[+/-sd] median   max
Connect:        0    0   0.2      0       7
Processing:     2    4   0.5      4       7
Waiting:        2    3   0.6      3       6
Total:          3    4   0.6      4      10

Percentage of the requests served within a certain time (ms)
  50%      4
  66%      4
  75%      4
  80%      4
  90%      4
  95%      6
  98%      6
  99%      6
 100%     10 (longest request)

```

### Windows(Intel Core i5/3Hz)

```
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

### Linux(Intel Core i5/3Hz)

```
Server Software:        
Server Hostname:        127.0.0.1
Server Port:            7000

Document Path:          /
Document Length:        12 bytes

Concurrency Level:      10
Time taken for tests:   0.310 seconds
Complete requests:      10000
Failed requests:        0
Keep-Alive requests:    10000
Total transferred:      1000000 bytes
HTML transferred:       120000 bytes
Requests per second:    32291.29 [#/sec] (mean)
Time per request:       0.310 [ms] (mean)
Time per request:       0.031 [ms] (mean, across all concurrent requests)
Transfer rate:          3153.45 [Kbytes/sec] received

Connection Times (ms)
              min  mean[+/-sd] median   max
Connect:        0    0   0.0      0       1
Processing:     0    0   0.2      0       5
Waiting:        0    0   0.1      0       5
Total:          0    0   0.2      0       5

Percentage of the requests served within a certain time (ms)
  50%      0
  66%      0
  75%      0
  80%      0
  90%      0
  95%      0
  98%      1
  99%      1
 100%      5 (longest request)
```

## License

MIT

## Author

Yasuhiro Matsumoto (a.k.a mattn)
