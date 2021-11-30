import os

'''
Plugin handler for the raw plugin. The plugin handler needs
to support two methods: push and flush.

push   Pushes a Seedlink station binding. Can be used to either
       create configuration files immediately or to manage the
       information until flush is called.

flush  Flush the configuration and return a unique key for this
       station that is used by seedlink to group eg templates for
       plugins.ini.
'''
class SeedlinkPluginHandler:
  # Create defaults
  def __init__(self): pass

  def push(self, seedlink):
    try: host = seedlink.param('sources.raw.host')
    except:
      host = "localhost"
      seedlink.setParam('sources.raw.host', host)

    try: port = seedlink.param('sources.raw.port')
    except:
      port = "65535"
      seedlink.setParam('sources.raw.port', port)

    try: locationCode = seedlink.param('sources.raw.locationCode')
    except:
      locationCode = ""
      seedlink.setParam('sources.raw.locationCode', locationCode)

    try: channelCode = seedlink.param('sources.raw.channelCode')
    except:
      channelCode = ""
      seedlink.setParam('sources.raw.channelCode', channelCode)

    stream = seedlink.net + '.' + seedlink.sta + '.' + locationCode + '.' + channelCode
    seedlink.setParam('sources.raw.stream', stream)

    # this is a mandatory parameter and it is requested so that an exception is
    # raised whether a value has not been provided
    try: componentMap = seedlink.param('sources.raw.componentMap')
    except:
      componentMap = ""
      seedlink.setParam('sources.raw.componentMap', componentMap)

    # key is station (one instance per station) - no multiple sensors per station allowed
    return seedlink.net + '.' + seedlink.sta

  # Flush does nothing
  def flush(self, seedlink):
    pass
