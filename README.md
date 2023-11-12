# cliupnp
A command-line tool for opening up UPnP ports on your router. Compiles on Linux, Mac, and Windows (using mingw32).
Requires: `miniupnpc` must be installed on the system to compile and run.

## Instructions

After compiling, do `./cliupnp --help` to see the options (there aren't many). 

```
Usage: cliupnp [--help] [--version] [--debug] port

Positional arguments:
  port           One or more ports to open up on the router [nargs: 1 or more] 

Optional arguments:
  -h, --help     shows help message and exits 
  -v, --version  prints version information and exits 
  -d, --debug    Enable extra debug logging
```

The program just accepts some port(s)s as 1 or more arg(s) and then contacts the router to keep them open and routed to your computer's IP.
Leave the program running to keep the ports open, interrupt the program (with `CTRL-C`) to close them. 
Note: Not all routers have UPnP or have it enabled, so you will get an error message and the program will exit if that is the case.

Enjoy!
