

import sys
import time
import fcntl
import ctypes

_IOC_NRBITS = 8
_IOC_TYPEBITS = 8
_IOC_SIZEBITS = 14
_IOC_DIRBITS = 2

_IOC_NRSHIFT = 0
_IOC_TYPESHIFT = _IOC_NRSHIFT + _IOC_NRBITS
_IOC_SIZESHIFT = _IOC_TYPESHIFT + _IOC_TYPEBITS
_IOC_DIRSHIFT = _IOC_SIZESHIFT + _IOC_SIZEBITS

_IOC_NONE = 0
_IOC_WRITE = 1
_IOC_READ  = 2

def _IOC(dir_, type_, nr, size):
    return (
        ctypes.c_int32(dir_ << _IOC_DIRSHIFT).value |
        ctypes.c_int32(ord(type_) << _IOC_TYPESHIFT).value |
        ctypes.c_int32(nr << _IOC_NRSHIFT).value |
        ctypes.c_int32(size << _IOC_SIZESHIFT).value)

def _IOC_TYPECHECK(t):
    return ctypes.sizeof(t)

def _IO(type_, nr):
    return _IOC(_IOC_NONE, type_, nr, 0)

def _IOW(type_, nr, size):
    return _IOC(_IOC_WRITE, type_, nr, _IOC_TYPECHECK(size))

def _IOR(type_, nr, size):
    return _IOC(_IOC_READ, type_, nr, _IOC_TYPECHECK(size))

def _IOWR(type_, nr, size):
    return _IOC(_IOC_READ | _IOC_WRITE, type_, nr, _IOC_TYPECHECK(size))

enum = ctypes.c_uint
fe_status_t = enum
FE_HAS_SIGNAL = 0x01
FE_HAS_CARRIER = 0x02
FE_HAS_VITERBI = 0x04
FE_HAS_SYNC = 0x08
FE_HAS_LOCK = 0x10
FE_TIMEDOUT = 0x20
FE_REINIT = 0x40

FE_READ_STATUS = _IOR('o', 69, fe_status_t)
FE_READ_BER = _IOR('o', 70, ctypes.c_uint32)
FE_READ_SIGNAL_STRENGTH = _IOR('o', 71, ctypes.c_uint16)
FE_READ_SNR = _IOR('o', 72, ctypes.c_uint16)
FE_READ_UNCORRECTED_BLOCKS = _IOR('o', 73, ctypes.c_uint32)



class Frontend(object):
#    global values
#    values =""

    def __init__(self, fd):
        self._fd = fd

    def _ioctlGet(self, query, c_type):
        result = c_type()
        try:
                fcntl.ioctl(self._fd, query, result)
        except:
                return result
        return result

    def getStatus(self):
        return self._ioctlGet(FE_READ_STATUS, fe_status_t).value

    def getBitErrorRate(self):
        return self._ioctlGet(FE_READ_BER, ctypes.c_uint32).value

    def getSignalNoiseRatio(self):
        return self._ioctlGet(FE_READ_SNR, ctypes.c_uint16).value

    def getSignalStrength(self):
        return self._ioctlGet(FE_READ_SIGNAL_STRENGTH, ctypes.c_uint16).value

    def getUncorrectedBlockCount(self):
        return self._ioctlGet(FE_READ_UNCORRECTED_BLOCKS, ctypes.c_uint32).value


dev = "/dev/dvb/adapter0/frontend0"
fe = Frontend(open(dev, 'r'))
print ("BitError %d; Status %d; SNR %.2f; Strength %d\n" % (fe.getBitErrorRate(), fe.getStatus(), fe.getSignalNoiseRatio() / 100, fe.getSignalStrength()))

#sys.stdout.write("EPOCH; BER; STATUS; SNR; SIGNAL\n")
#for x in range(1):
#        sys.stdout.write("%d;%d;%d;%d;%d\n" % (time.time(), fe.getBitErrorRate(), fe.getStatus(), fe.getSignalNoiseRatio(), fe.getSignalStrength()))
#        sys.stdout.write("BitError %d; Status %d; SNR %.2f; Strength %d\n" % (fe.getBitErrorRate(), fe.getStatus(), fe.getSignalNoiseRatio() / 100, fe.getSignalStrength()))
#        time.sleep(.1)
#        global values
#        values = ("BitError %d; Status %d; SNR %.2f; Strength %d\n" % (fe.getBitErrorRate(), fe.getStatus(), fe.getSignalNoiseRatio() / 100, fe.getSignalStrength()))
#        print("%s" % (values))

class Signal:

    def signalValues(self):
        self.values = ("BitError %d; Status %d; SNR %.2f; Strength %d\n" % (fe.getBitErrorRate(), fe.getStatus(), fe.getSignalNoiseRatio() / 100, fe.getSignalStrength()))
        return ("BitError %d; Status %d; SNR %.2f; Strength %d\n" % (fe.getBitErrorRate(), fe.getStatus(), fe.getSignalNoiseRatio() / 100, fe.getSignalStrength()))
        print ("BitError %d; Status %d; SNR %.2f; Strength %d\n" % (fe.getBitErrorRate(), fe.getStatus(), fe.getSignalNoiseRatio() / 100, fe.getSignalStrength()))
