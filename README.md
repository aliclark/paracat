
paracat
=======

Usage: paracat -n NUMPROCS -- COMMAND ARG1 ARG2 ...


Spawn NUMPROCS instances of COMMAND, piping chunks of stdin into the
child processes.

The spawned processes will always receive chunks as full lines - a
line is never sent partially to more than one process.

Unless the --no-recombine option is supplied, the output of the
spawned processes will be written to stdout as full lines only.


Proof of concept:

```sh
$ time ./paracat -n 2 -- /usr/bin/wc -w < /tmp/somefile
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
