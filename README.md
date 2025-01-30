# seedlink_raw_plugin
[Seedlink](<https://github.com/SeisComP/seedlink>) plugin for custom data streaming to [SeisComP](<https://github.com/SeisComP>)

This plugin can be included in the SeisComP Seedlink implementation and allows to stream
custom data to SeisComP. This `RAW` plugin, running within the seedlink server, connects to 
the provided `raw_server.py`, which has to run on the device that provides the data.

The `raw_server.py` provides the API to stream your data to the `RAW` seedlink plugin without
worring about the protocol details. If you cannot use the `raw_server.py` in your device, you
can create your own implementation in any language.

If you want to make you device a fully fledged seedlink server, then install SeiComP
(or just seedklink) in the same machine where the `raw_server` is running, then configure
the `RAW` seedlink plugin to connect to `localhost`. The locally running seedlink server
will then be able to serve your custom data.

## How it works

The RAW plugin and server implement a barebone minimalistic protocol for data exchange.
When the seedlink plugin connects to a raw server, it selects a channel (or more). A channel
is just a numeric id used to distinguish multiple streams available in a raw_server.
The server then specifies the data settings for every channel before streaming it. These
settings may be:
- int8, int16, int32, float32, float64
- little-endian, big-endian 
- sample rate (max 1MHz)

The data packets, once received by the seedlink plugin, are eventually converted to miniseeds
and passed to seedlink server, which make them available to any application connecting to it
e.g. SeisComP.

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
# Set the preferred log levels for the api and the server processes
# The spawned server(s) will log to the file raw_server.log
#
logging.getLogger("raw_api").setLevel(logging.INFO)
logging.getLogger("raw_server").setLevel(logging.INFO)

#
# Channel defines the properties of the data we want to stream
# id: channel identifier (range 0-65535)
# samprate: max 1Mhz
# samptype: int8, int16, int32, float32, float64
#
# In this exaple we stream 2 sensors, each one with 3 components, that we
# intend to map in seiscomp to a single station with 2 location codes:

# Station 1 (3 components)
c1 = rs.Channel(id=1, samprate=200, endianness=sys.byteorder, samptype="int32")
c2 = rs.Channel(id=2, samprate=200, endianness=sys.byteorder, samptype="int32")
c3 = rs.Channel(id=3, samprate=200, endianness=sys.byteorder, samptype="int32")

# Station 2 (3 components)
c4 = rs.Channel(id=4, samprate=100, endianness=sys.byteorder, samptype="int16")
c5 = rs.Channel(id=5, samprate=100, endianness=sys.byteorder, samptype="int16")
c6 = rs.Channel(id=6, samprate=100, endianness=sys.byteorder, samptype="int16")

#
# create a streaming server that will run as a separate process and wait for
# connection on `port=65535
#
streamer = rs.Streamer(channels=[c1,c2,c3,c4,c5,c6], host="127.0.0.1", port=65535)
streamer.start()

#
# Read data from hardware and pass it to the streamer that makes it available
# to connected clients (seedlink raw plugins)
#
while True:
  #
  # Here put your device logic: once you got the device data (samples and
  # sample time) pass it to the streamer via `streamer.feed_data()` method
  #
  st1 = device_driver_station1.get_data()
  st2 = device_driver_station2.get_data()

  # channel_id: what channel the data belongs to (range 0-65535)
  # samptime: start timestamp of the samples (type datetime.datetime) micro sec resolution
  # timing_quality: mapped to Miniseed Timing quality in Data Extension Blockette
  #                1001. Vendor specific value from 0 to 100 of maximum accuracy,
  #                taking into account both clock quality and data flags
  # samples: array like (numpy array, list) containing the data samples, which
  #           will be converted by the api to the Channel samptype and endianness
  #           before streaming it.
  #
  streamer.feed_data(channel_id=1, st1.samptime, st1.time_quality, st1.comp1_samples)
  streamer.feed_data(channel_id=2, st1.samptime, st1.time_quality, st1.comp2_samples)
  streamer.feed_data(channel_id=3, st1.samptime, st1.time_quality, st1.comp3_samples)

  streamer.feed_data(channel_id=4, st2.samptime, st2.time_quality, st2.comp1_samples)
  streamer.feed_data(channel_id=5, st2.samptime, st2.time_quality, st2.comp2_samples)
  streamer.feed_data(channel_id=6, st2.samptime, st2.time_quality, st2.comp3_samples)
```

## Installation

Simply drop the code in the `seiscomp/src/base/seedlink/` folder and edit 
`seiscomp/src/base/seedlink/plugins/CMakeLists.txt` to compile the raw_plugin too during
the compilation/installation process.

## Configuration

To make this plugin work within SeisComP you need to provides the seedlink bindings for your
station. In the bindings you need to select the raw plugin and configure it with the address
of the raw server, then select the channel ids you want to request and how to map these
channel ids to the location and channel codes of the station.

You can use `raw_server.py` for testing the bindings: when running `python raw_server.py` it
will stream sample data in various formats (see  the `__main__` function  for details).
Configure the bindings to connect to localhost and run `python raw_server.py` to see if
all works fine, at least with the sample data.

![Bindings options](/bindingsOptions.png?raw=true "Bindings options")



