# seedlink_raw_plugin
Seedlink plugin for custom data streaming to SeisComP

This plugin can be included in the SeisComP Seedlink implementation and allows to stream
custom data to SeisComP. This RAW plugin, running within the seedlink server, connects to 
the provided raw_server.py, which has to run on your device.

The raw_server.py provides the API to stream your data to the seedlink server, through the
RAW plugin, without worring about the protocol details. If you cannot use the raw_server
in our device, you can create your own implementation and use raw_server.py as a reference.

The RAW plugin and server implement a barebone minimalistic protocol and it can easily be
modified if that doesn't already fullfil your requirements. Plugin and server agree on the
data settings before streaming: 
- int8, int16, int32
- little-endian, big-endian 
- sample rate (max 1MHz) 
- the channel(s) to stream (a channel is just a numeric id used to distinguish multiple streams
  but the meaning is defined by the server, which decides what a channel number refers to).
The data packets, once received by the plugin, are eventually converted to miniseeds and passed
to seedlink, which make them available to any application connecting to it e.g. SeisComP.

The raw_server.py runs as a separate process and inter process communication is used to
pass the data between your code and the server. The server itself uses asyncio to achive high
performance. All these details are hiddend behind a nice API layer.

To install simply drop the code in the `seiscomp/src/base/seedlink/` folder and edit 
`seiscomp/src/base/seedlink/plugins/CMakeLists.txt` to compile the raw_plugin too during
the compilation/installation process.

Finally, to make this plugin work within SeisComP you need to provides the seedlink bindings
for your station(s). In the bindings you need to select the raw plugin and configure it with
the address of the server.

![Bindings options](/bindingsOptions.png?raw=true "Bindings options")



