
paracat
=======

Usage: paracat NUMPROCS -- COMMAND ARG1 ARG2 ...


Spawn NUMPROCS instances of COMMAND, piping chunks of stdin into the
child processes.

The spawned processes will always receive chunks as full lines - a
line is never sent partially to more than one process.


Performance is copmarable to "cat", but with some time lost due to
rounding chunks to the nearest newline, and some time lost in spawning
processes and writing to their pipes.

A single spawned process is most performant, adding more will degrade
performance until about four processes are being used. At that point
extra processes do not significantly impact performance.

Despite not processing files as quickly as "cat", it is highly likely
that this will not be the bottleneck in a processing pipeline, so it
can be used to parallelize the processing of large amounts of input.


Proof of concept:

```sh
$ time ./paracat 2 -- /usr/bin/wc -w < /tmp/somefile
14499594
14500406

real0m3.141s
user0m5.328s
sys0m0.376s

$ time /usr/bin/wc -w /tmp/somefile
29000000 /tmp/somefile

real0m5.301s
user0m5.196s
sys0m0.088s
```
