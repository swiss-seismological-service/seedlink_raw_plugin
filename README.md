# seedlink_raw_plugin
Adaptable Seedlink plugin for custom data streaming to SeisComP

This plugin can be included in the SeisComP Seedlink implementation and allows to stream
custom data to SeisComP. The plugin connects to the provided raw_server.py that can be used
as a python module to stream your own data.

The RAW plugin and server implement a barebone minimalistic protocol that can be easily
modified to suit your own use case, if the current protocol doesn't fullfil your requirements.

