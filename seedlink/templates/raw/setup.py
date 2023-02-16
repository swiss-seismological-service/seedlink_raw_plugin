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

    try: miniSeedEncoding = seedlink.param('sources.raw.miniSeedEncoding')
    except:
      miniSeedEncoding = "STEIM2"
      seedlink.setParam('sources.raw.miniSeedEncoding', miniSeedEncoding)

    channelProfiles = seedlink.param('sources.raw.channelProfiles')

    channelCodeMap = ""
    for profile in channelProfiles.split(","):

        try: locationCode = seedlink.param(f"sources.raw.channelProfile.{profile}.locationCode")
        except: locationCode = ""
        channelCodes = seedlink.param(f"sources.raw.channelProfile.{profile}.channelCodes")

        for kv in channelCodes.split(","):
            channelCode, serverChannel = kv.split(":")
            channelCodeMap += f"{locationCode}.{channelCode}:{serverChannel},"

    seedlink.setParam('sources.raw.channelCodeMap', channelCodeMap)

    stream = seedlink.net + '.' + seedlink.sta
    seedlink.setParam('sources.raw.stream', stream)

    return stream

  # Flush does nothing
  def flush(self, seedlink):
    pass
