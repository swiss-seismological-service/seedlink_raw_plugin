# seedlink_raw_plugin
[Seedlink](<https://github.com/SeisComP/seedlink>) plugin for custom data streaming to [SeisComP](<https://github.com/SeisComP>)

This plugin can be included in the SeisComP Seedlink implementation and allows to stream
custom data to SeisComP. This RAW plugin, running within the seedlink server, connects to 
the provided raw_server.py, which has to run on your device.

The raw_server.py provides the API to stream your data to the seedlink server, through the
RAW plugin, without worring about the protocol details. If you cannot use the raw_server
in your device, you can create your own implementation and use raw_server.py as a reference.

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

The `raw_server.py` runs as a separate process and inter process communication is used to
pass the data between your code and the server. The server itself uses asyncio to achive high
performance. All these details are hiddend behind a nice API layer. To use `raw_server.py` as a
module you can have a look at the `__main__` function as a reference, which contains a test
program that can be used to send sample data for testing. However the general idea is the
following:

```
import sys
import datetime
import numpy as np

import raw_server as rs

#
# Define the data we want to stream:
#  - 3 channels at 4000hz with ids 1, 2, 3
#  - data is 2 bytes integers, system endianness (could be 'little' or 'big')
#
c1 = Channel(id=1, samprate=4000, endianness=sys.byteorder, sampsize=2)
c2 = Channel(id=2, samprate=4000, endianness=sys.byteorder, sampsize=2)
c3 = Channel(id=3, samprate=4000, endianness=sys.byteorder, sampsize=2)

#
# create a streaming server that will run as a separate process
#
streamer = rs.Streamer(channels=[c1,c2,c3], host="127.0.0.1", port=65535)
streamer.start()

# Read data from hardware and pass it to the streamer
while True:
  #
  # Here put your device logic: you need to pass the data to the streamer that
  # takes care of sending it to seedlink if/when seedlink connects to `port=65535`
  # and asks for channels ids 1, 2 or 3. It is the `streamer.feed_data` method that 
  # decides what the seedlink server gets when asking for a specific channel id
  #
  # samptime: timestamp of the samples (type datetime)
  # samplesX: numpy arrays of type and endianness accordingly to Channel above
  #
  streamer.feed_data( channel_id=1, samptime, samples1 )
  streamer.feed_data( channel_id=2, samptime, samples2 )
  streamer.feed_data( channel_id=3, samptime, samples3 )

```


To install simply drop the code in the `seiscomp/src/base/seedlink/` folder and edit 
`seiscomp/src/base/seedlink/plugins/CMakeLists.txt` to compile the raw_plugin too during
the compilation/installation process.

Finally, to make this plugin work within SeisComP you need to provides the seedlink bindings
for your station(s). In the bindings you need to select the raw plugin and configure it with
the address of the server.

You can use `raw_server.py` for testing the bindings: when running `python raw_server.py` it
will stream sample data (see  the `__main__` function  for details). So you can configure 
your bindings to connect to the local raw_server and check that all works fine, at least with
the sample data.

![Bindings options](/bindingsOptions.png?raw=true "Bindings options")



