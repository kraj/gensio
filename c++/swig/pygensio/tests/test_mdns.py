#
#  gensio - A library for abstracting stream I/O
#  Copyright (C) 2018  Corey Minyard <minyard@acm.org>
#
#  SPDX-License-Identifier: GPL-2.0-only
#

# Test mdns interfaces

import sys
import gensios_enabled
if not gensios_enabled.check_gensio_enabled("mdns"):
    sys.exit(77)

import pygensio
from testbase import *

class Free_Done(pygensio.MDNS_Free_Done):
    def __init__(self, waiter):
        pygensio.MDNS_Free_Done.__init__(self)
        self.waiter = waiter
        return

    def mdns_free_done(self):
        self.waiter.wake()
        return

class Watch_Free_Done(pygensio.MDNS_Watch_Free_Done):
    def __init__(self, waiter):
        pygensio.MDNS_Watch_Free_Done.__init__(self)
        self.waiter = waiter
        return

    def mdns_watch_free_done(self):
        self.waiter.wake()
        return

class Watch_EvHnd(pygensio.MDNS_Watch_Event):
    def __init__(self, waiter):
        pygensio.MDNS_Watch_Event.__init__(self)
        self.waiter = waiter
        self.watch_count = 0
        return

    def event(self, state, interfacenum, ipdomain,
              name, mtype, domain, host, addr, txt):
        if state == pygensio.GENSIO_MDNS_ALL_FOR_NOW:
            self.waiter.wake()
            return
        self.watch_count += 1
        return

waiter = pygensio.Waiter(o)
m = pygensio.MDNS(o)
s = pygensio.MDNS_Service(m, -1, pygensio.GENSIO_NETTYPE_UNSPEC, "gensio1",
                          "_gensio1._tcp", None, None, 5001, ("A", "B"))
e = Watch_EvHnd(waiter)
w = pygensio.MDNS_Watch(m, -1,  pygensio.GENSIO_NETTYPE_UNSPEC, "gensio1",
                None, None, None, e)

rv = waiter.wait(1, pygensio.gensio_time(5, 0))
if rv != 0:
    raise Exception("Error waiting for mdns watch: " +
                    pygensio.err_to_string(rv))

wfh = Watch_Free_Done(waiter)
w.free(wfh)
rv = waiter.wait(1, pygensio.gensio_time(1, 0))
if rv != 0:
    raise Exception("Error waiting for mdns watch free: " +
                    pygensio.err_to_string(rv))
del wfh

del s

mfh = Free_Done(waiter)
m.free(mfh)
rv = waiter.wait(1, pygensio.gensio_time(1, 0))
if rv != 0:
    raise Exception("Error waiting for mdns free: " +
                    pygensio.err_to_string(rv))
del mfh

del e
del waiter
del o

test_shutdown()

print("Pass")
sys.exit(0)
