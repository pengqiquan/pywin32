"""An ISAPI extension base class implemented using a thread-pool."""
# $Id$

import sys
from isapi import isapicon
import isapi.simple
from win32file import GetQueuedCompletionStatus, CreateIoCompletionPort, \
                      PostQueuedCompletionStatus, CloseHandle
from win32security import SetThreadToken
from win32event import INFINITE
from pywintypes import OVERLAPPED

# Python 2.3 and earlier insists on "C" locale - if it isn't, subtle things
# break, such as floating point constants loaded from .pyc files.
# The threading module uses such floating-points as an argument to sleep(),
# resulting in extremely long sleeps when tiny intervals are specified.
# We can work around this by resetting the C locale before the import.
if sys.hexversion < 0x02040000:
    import locale
    locale.setlocale(locale.LC_NUMERIC, "C")

import threading
import traceback

ISAPI_REQUEST = 1
ISAPI_SHUTDOWN = 2

class WorkerThread(threading.Thread):
    def __init__(self, extension, io_req_port):
        self.running = False
        self.io_req_port = io_req_port
        self.extension = extension
        threading.Thread.__init__(self)

    def run(self):
        self.running = True
        while self.running:
            errCode, bytes, key, overlapped = \
                GetQueuedCompletionStatus(self.io_req_port, INFINITE)
            if key == ISAPI_SHUTDOWN and overlapped is None:
                break

            # Let the parent extension handle the command.
            dispatcher = self.extension.dispatch_map.get(key)
            if dispatcher is None:
                raise RuntimeError, "Bad request '%s'" % (key,)
            
            dispatcher(errCode, bytes, key, overlapped)

    def call_handler(self, cblock):
        self.extension.Dispatch(cblock)

# A generic thread-pool based extension, using IO Completion Ports.
# Sub-classes can override one method to implement a simple extension, or
# may leverage the CompletionPort to queue their own requests, and implement a
# fully asynch extension.
class ThreadPoolExtension(isapi.simple.SimpleExtension):
    "Base class for an ISAPI extension based around a thread-pool"
    max_workers = 20
    worker_shutdown_wait = 15000 # 15 seconds for workers to quit. XXX - per thread!!! Fix me!
    def __init__(self):
        self.workers = []
        # extensible dispatch map, for sub-classes that need to post their
        # own requests to the completion port.
        # Each of these functions is called with the result of 
        # GetQueuedCompletionStatus for our port.
        self.dispatch_map = {
            ISAPI_REQUEST: self.DispatchConnection,
        }

    def GetExtensionVersion(self, vi):
        isapi.simple.SimpleExtension.GetExtensionVersion(self, vi)
        # As per Q192800, the CompletionPort should be created with the number
        # of processors, even if the number of worker threads is much larger.
        # Passing 0 means the system picks the number.
        self.io_req_port = CreateIoCompletionPort(-1, None, 0, 0)
        # start up the workers
        self.workers = []
        for i in range(self.max_workers):
            worker = WorkerThread(self, self.io_req_port)
            worker.start()
            self.workers.append(worker)

    def HttpExtensionProc(self, control_block):
        overlapped = OVERLAPPED()
        overlapped.object = control_block
        PostQueuedCompletionStatus(self.io_req_port, 0, ISAPI_REQUEST, overlapped)
        return isapicon.HSE_STATUS_PENDING

    def TerminateExtension(self, status):
        for worker in self.workers:
            worker.running = False
        for worker in self.workers:
            PostQueuedCompletionStatus(self.io_req_port, 0, ISAPI_SHUTDOWN, None)
        for worker in self.workers:
            worker.join(self.worker_shutdown_wait)
        self.dispatch_map = {} # break circles
        CloseHandle(self.io_req_port)

    # This is the one operation the base class supports - a simple
    # Connection request.  We setup the thread-token, and dispatch to the
    # sub-class's 'Dispatch' method.
    def DispatchConnection(self, errCode, bytes, key, overlapped):
        control_block = overlapped.object
        # setup the correct user for this request
        hRequestToken = control_block.GetImpersonationToken()
        SetThreadToken(None, hRequestToken)
        try:
            try:
                self.Dispatch(control_block)
            except:
                self.HandleDispatchError(control_block)
        finally:
            # reset the security context
            SetThreadToken(None, None)

    def Dispatch(self, ecb):
        """Overridden by the sub-class to handle connection requests.
        
        This class creates a thread-pool using a Windows completion port,
        and dispatches requests via this port.  Sub-classes can generally
        implementeach connection request using blocking reads and writes, and
        the thread-pool will still provide decent response to the end user.
        
        The sub-class can set a max_workers attribute (default is 20).  Note
        that this generally does *not* mean 20 threads will all be concurrently
        running, via the magic of Windows completion ports.
        
        There is no default implementation - sub-classes must implement this.
        """
        raise NotImplementedError, "sub-classes should override Dispatch"

    def HandleDispatchError(self, ecb):
        """Handles errors in the Dispatch method.
        
        When a Dispatch method call fails, this method is called to handle
        the exception.  The default implementation formats the traceback
        in the browser.
        """
        ecb.HttpStatusCode = isapicon.HSE_STATUS_ERROR
        #control_block.LogData = "we failed!"
        ecb.SendResponseHeaders("200 OK", "Content-type: text/html\r\n\r\n", 
                                False)
        exc_typ, exc_val, exc_tb = sys.exc_info()
        limit = None
        try:
            try:
                import cgi
                print >> ecb
                print >> ecb, "<H3>Traceback (most recent call last):</H3>"
                list = traceback.format_tb(exc_tb, limit) + \
                       traceback.format_exception_only(exc_typ, exc_val)
                print >> ecb, "<PRE>%s<B>%s</B></PRE>" % (
                    cgi.escape("".join(list[:-1])), cgi.escape(list[-1]),)
            except:
                print "FAILED to render the error message!"
                traceback.print_exc()
                print "ORIGINAL extension error:"
                traceback.print_exception(exc_typ, exc_val, exc_tb)
        finally:
            # holding tracebacks in a local of a frame that may itself be 
            # part of a traceback used to be evil and cause leaks!
            exc_tb = None
            ecb.DoneWithSession()