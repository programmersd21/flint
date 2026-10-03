# environment
  cpu        12th Gen Intel(R) Core(TM) i5-1235U
  kernel     Linux-7.2.8-2-cachyos-x86_64-with-glibc2.44
  machine    x86_64
  compiler   cc (GCC) 16.2.1 20260810
  python     3.14.7
  flint      Flint v0.7.0-5-gb340d5f

case              median      best   checksum
----------------------------------------------------
arith              1.59s     1.43s   400000000000000
arith_add        987.3ms   897.5ms   199999990000000
arith_div        255.9ms   248.5ms   16002164.035301253
arith_mod        165.5ms   156.3ms   8999994
arith_mul         92.1ms    86.9ms   7.389041319626742
call_direct      142.1ms   134.1ms   2000001000000
call_native      113.8ms   100.5ms   5888890
call_nested      179.4ms   168.4ms   812500
calls            178.2ms   168.9ms   3000000
closure_call     103.3ms    96.6ms   2000001
closures         124.2ms   117.0ms   2000000
coalesce         397.1ms   363.6ms   6249995000000
compare          303.7ms   292.9ms   0
expr              0.57ms    0.46ms   
fib               18.1ms    16.0ms   196418
float            149.0ms   142.4ms   15.491338344866602
hello             0.54ms    0.46ms   hello, world
json_parse_big    7.58ms    6.03ms   4000
json_parse_small    0.77ms    0.58ms   5
json_roundtrip    8.52ms    5.94ms   23994000
json_write_big    21.2ms    19.1ms   1174084
json_write_small    0.57ms    0.50ms   38
list_build        47.5ms    45.2ms   1000000
list_index        54.0ms    50.8ms   124999750000
list_iter         29.8ms    28.1ms   89999700000
lists            111.1ms   107.9ms   1499998500000
mem_alloc         64.1ms    60.5ms   124999750000
mem_retain       149.8ms   138.8ms   300000
range_loop       218.8ms   212.1ms   12499997500000
startup           0.53ms    0.44ms   
str_ascii         14.4ms    13.1ms   16000000
str_concat        17.4ms    16.0ms   20000
str_concat_small    50.4ms    45.2ms   200000
str_equal        129.0ms   116.0ms   2000000
str_find          30.3ms    27.5ms   300000
str_find_miss    117.6ms   111.2ms   0
str_fmt           40.0ms    36.0ms   2859685
str_ident         23.6ms    20.5ms   11000000
str_join          4.70ms    3.14ms   249999
str_long_equal    21.8ms    17.9ms   200000
str_prefix        68.3ms    62.6ms   1000000
str_replace       2.59ms    1.91ms   100000
str_split         3.90ms    2.75ms   20000
str_temp          35.9ms    30.7ms   1888890
str_unequal       93.1ms    84.5ms   0
str_utf8          50.1ms    42.6ms   24500000
strings          100.3ms    82.5ms   2088889
table_access     503.5ms   461.2ms   10000
table_build       28.1ms    24.3ms   200000
table_field      105.9ms    98.3ms   15000000
table_iter       486.5ms   449.7ms   199990000
tinyfn            0.58ms    0.53ms   1000
