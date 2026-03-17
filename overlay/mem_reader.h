#pragma once

#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>

#include "../include/hunt_shared.h"

/* Userspace wrapper for /dev/hunt_read ioctl interface */
class MemReader {
public:
    MemReader() : fd_(-1) {}
    ~MemReader() { close(); }

    bool open() {
        fd_ = ::open(HUNT_DEVICE_PATH, O_RDWR);
        return fd_ >= 0;
    }

    void close() {
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
    }

    bool set_pid(int32_t pid) {
        if (fd_ < 0) return false;
        return ioctl(fd_, HUNT_IOC_SET_PID, &pid) == 0;
    }

    bool read_mem(uint64_t address, void *buffer, uint64_t size) {
        if (fd_ < 0) return false;

        struct hunt_read_req req = {};
        req.address = address;
        req.buffer = (uint64_t)buffer;
        req.size = size;
        req.result = -1;

        if (ioctl(fd_, HUNT_IOC_READ_MEM, &req) != 0)
            return false;

        return req.result == 0;
    }

    bool get_module_base(const char *name, uint64_t &base, uint64_t &size) {
        if (fd_ < 0) return false;

        struct hunt_module_req req = {};
        strncpy(req.name, name, sizeof(req.name) - 1);
        req.result = -1;

        if (ioctl(fd_, HUNT_IOC_GET_MODULE, &req) != 0)
            return false;

        if (req.result != 0)
            return false;

        base = req.base;
        size = req.size;
        return true;
    }

    template<typename T>
    bool read(uint64_t address, T &out) {
        return read_mem(address, &out, sizeof(T));
    }

    bool read_ptr(uint64_t address, uint64_t &out) {
        return read<uint64_t>(address, out);
    }

private:
    int fd_;
};
