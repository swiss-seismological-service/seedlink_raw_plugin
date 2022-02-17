# seedlink_raw_plugin
Adaptable Seedlink plugin for custom data streaming to SeisComP

This plugin can be included in the SeisComP Seedlink implementation and allows to stream
custom data to SeisComP. The raw plugin, running withiin seedlink server, connects to the
provided raw_server.py, running on your own device. The raw_server.py provides the API to 
stream your own data to the plugin without worring about the protocol details. The 
raw_server.py works as a separate process and the module API use inter process communication
to pass data between your code and the server. The server itself uses asyncio to achive high
performance. That said, there is no need to use raw_server, a different implementation would
work as well.

The RAW plugin and server implement a barebone minimalistic protocol that can be easily
modified in the code to suit your own use case, if that doesn't fullfil already your
requirements. Plugin and server agree on the data settings before streaming: int8, int16, int32,
little-endian, big-endian and sample rate (max 1MHz) and the channels to stream (a channel is
just a numeric id used to distinguish multiple streams coming from the same server). The data
packets, once received by the plugin, are eventually converted to miniseeds and passed to
seedlink, which make them available to any application connecting to it.

Finally, to make this plugin work within SeisComP you need to provides the seedlink bindings
for your station(s). In the bindings you need to select the raw plugin and configure it with
the address of the server.
