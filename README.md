
paracat
=======

Usage: paracat [--no-shell] [--no-recombine] [-n numprocs] [--] [command [arg ...]]


Spawn numproces instances of a command, piping chunks of stdin into
the child processes.

The spawned processes will always receive chunks as full lines - a
line is never sent partially to more than one process.

If numprocs is not specified, a default of 2 is used.

If command is not specified, '/bin/cat' is used.

Unless the --no-recombine option is supplied, the output of the
spawned processes will be written to stdout as full lines only.

By default the command is passed as an argument to '/bin/sh -c' - use
--no-shell to execute the command directly.

The performance cost of both of these defaults is insignificant, so
it's easiest to just leave them as default.

stderr is not recombined at line breaks, so this may become
interleaved.

A double dash (--) may optionally be used as a separator between
options and the command, but it shouldn't be necessary.


Proof of concept:

```sh
$ time paracat wc -w <somefile
14499710
14500290

real    0m2.997s
user    0m5.256s
sys     0m0.408s

$ time wc -w somefile
29000000 /tmp/somefile

real0m5.301s
user0m5.196s
sys0m0.088s
```
