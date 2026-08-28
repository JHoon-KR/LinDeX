#ifndef ADVC_AHB_TRANSPORT_H
#define ADVC_AHB_TRANSPORT_H

#include <android/hardware_buffer.h>

/*
 * AHB handles are transferred only after an ADVC_OP_TRANSFER_AHB reply with
 * ADVC_FLAG_AHB_FOLLOWS. The connection must have one in-flight request while
 * these functions run, preventing an AHB handle record from being mistaken for
 * a protocol record. Ownership follows AHardwareBuffer acquire/release rules.
 */
static inline int advc_send_ahardwarebuffer(int fd, const AHardwareBuffer *buffer) {
    return AHardwareBuffer_sendHandleToUnixSocket(buffer, fd);
}

static inline int advc_receive_ahardwarebuffer(int fd, AHardwareBuffer **buffer) {
    return AHardwareBuffer_recvHandleFromUnixSocket(fd, buffer);
}

static inline int advc_send_ahardwarebuffer_callback(int fd, void *buffer,
                                                     void *userdata) {
    (void)userdata;
    return buffer != NULL ?
        AHardwareBuffer_sendHandleToUnixSocket((AHardwareBuffer *)buffer, fd) : -1;
}

#endif
