# multiplex
Input multiplexer device for Linux
==================================

This userspace daemon was written for this multi-function remote control:

```
ID 05a4:9881 Ortek Technology, Inc. IR receiver [VRC-1100 Vista MCE Remote Control]
```

By default, Linux will create 4 device nodes in `/dev/input/` for this remote
control unit. The `udev` rules provided in `70-vdr.rules` will assign these
nodes the following names:

- `/dev/input/dvb-ir-mouse` for the mouse functions
- `/dev/input/dvb-ir-key<N>` for the key functions

The `multiplex` daemon will copy events from these 4 nodes into a single new
`uinput` device which programs such as `vdr` can then use.
