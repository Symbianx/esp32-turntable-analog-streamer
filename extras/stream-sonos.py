#!/usr/bin/env python
from soco import SoCo
import sys

if __name__ == '__main__':

    sonos_ip = sys.argv[1]
    stream_url = sys.argv[2]

    sonos = SoCo(sonos_ip) # Pass in the IP of your Sonos speaker
    # You could use the discover function instead, if you don't know the IP

    # Pass in a URI to a media file to have it streamed through the Sonos
    # speaker
    sonos.play_uri(stream_url)
