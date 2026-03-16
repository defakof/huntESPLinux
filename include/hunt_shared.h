#ifndef HUNT_SHARED_H
#define HUNT_SHARED_H

#ifdef __KERNEL__
#include <linux/types.h>
#include <linux/ioctl.h>
#else
#include <stdint.h>
#include <sys/ioctl.h>
#endif

#define HUNT_DEVICE_NAME "hunt_read"
#define HUNT_DEVICE_PATH "/dev/" HUNT_DEVICE_NAME

/* ioctl commands */
#define HUNT_IOC_MAGIC 'H'

/* Set target process PID */
#define HUNT_IOC_SET_PID    _IOW(HUNT_IOC_MAGIC, 1, int32_t)

/* Read memory from target process */
#define HUNT_IOC_READ_MEM   _IOWR(HUNT_IOC_MAGIC, 2, struct hunt_read_req)

/* Get module base address */
#define HUNT_IOC_GET_MODULE _IOWR(HUNT_IOC_MAGIC, 3, struct hunt_module_req)

struct hunt_read_req {
    uint64_t address;   /* source address in target process */
    uint64_t buffer;    /* userspace destination buffer pointer */
    uint64_t size;      /* bytes to read */
    int32_t  result;    /* 0 on success, negative on error */
    int32_t  _pad;
};

struct hunt_module_req {
    char     name[128]; /* module name to find (e.g., "GameHunt.dll") */
    uint64_t base;      /* returned base address */
    uint64_t size;      /* returned module size */
    int32_t  result;    /* 0 on success */
    int32_t  _pad;
};

#endif /* HUNT_SHARED_H */
