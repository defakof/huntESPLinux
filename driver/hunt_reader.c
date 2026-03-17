/*
 * hunt_reader.ko - Kernel module for cross-process memory reading
 * Exposes /dev/hunt_read with ioctl for PID targeting, memory reads, and module lookup.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/sched.h>
#include <linux/mm.h>
#include <linux/uaccess.h>
#include <linux/pid.h>
#include <linux/sched/mm.h>
#include <linux/highmem.h>
#include <linux/version.h>

#include "../include/hunt_shared.h"

MODULE_LICENSE("GPL");
MODULE_AUTHOR("hunt-esp");
MODULE_DESCRIPTION("Read-only memory reader for game process");

static DEFINE_MUTEX(hunt_mutex);
static struct pid *target_pid_struct;
static pid_t target_pid;

static int read_process_memory(struct task_struct *task, unsigned long addr,
                               void *buf, size_t len)
{
    int copied;

    copied = access_process_vm(task, addr, buf, len, 0);
    if (copied != (int)len)
        return -EFAULT;

    return 0;
}

static struct task_struct *get_target_task(void)
{
    struct task_struct *task;

    if (!target_pid_struct)
        return NULL;

    task = get_pid_task(target_pid_struct, PIDTYPE_PID);
    return task;
}

static int hunt_set_pid(int32_t pid)
{
    struct pid *new_pid;

    new_pid = find_get_pid(pid);
    if (!new_pid)
        return -ESRCH;

    {
        struct task_struct *task = get_pid_task(new_pid, PIDTYPE_PID);
        if (!task) {
            put_pid(new_pid);
            return -ESRCH;
        }
        put_task_struct(task);
    }

    if (target_pid_struct)
        put_pid(target_pid_struct);

    target_pid_struct = new_pid;
    target_pid = pid;

    pr_info("hunt_reader: target PID set to %d\n", pid);
    return 0;
}

static int hunt_read_mem(struct hunt_read_req __user *ureq)
{
    struct hunt_read_req req;
    struct task_struct *task;
    void *kbuf;
    int ret;

    if (copy_from_user(&req, ureq, sizeof(req)))
        return -EFAULT;

    if (req.size == 0 || req.size > (4 * 1024 * 1024)) {
        req.result = -EINVAL;
        goto out;
    }

    task = get_target_task();
    if (!task) {
        req.result = -ESRCH;
        goto out;
    }

    kbuf = kvmalloc(req.size, GFP_KERNEL);
    if (!kbuf) {
        put_task_struct(task);
        req.result = -ENOMEM;
        goto out;
    }

    ret = read_process_memory(task, req.address, kbuf, req.size);
    put_task_struct(task);

    if (ret == 0) {
        if (copy_to_user((void __user *)req.buffer, kbuf, req.size))
            ret = -EFAULT;
    }

    kvfree(kbuf);
    req.result = ret;

out:
    if (copy_to_user(&ureq->result, &req.result, sizeof(req.result)))
        return -EFAULT;

    return 0;
}

/*
 * Walk target process VMAs to find a module by filename.
 * Wine maps DLLs as multiple non-contiguous VMAs; we compute the full span.
 */
static int hunt_get_module(struct hunt_module_req __user *ureq)
{
    struct hunt_module_req req;
    struct task_struct *task;
    struct mm_struct *mm;
    struct vm_area_struct *vma;
    struct vma_iterator vmi;
    int ret = -ENOENT;

    if (copy_from_user(&req, ureq, sizeof(req)))
        return -EFAULT;

    req.name[sizeof(req.name) - 1] = '\0';
    req.base = 0;
    req.size = 0;
    req.result = -ENOENT;

    task = get_target_task();
    if (!task) {
        req.result = -ESRCH;
        goto out;
    }

    mm = get_task_mm(task);
    put_task_struct(task);

    if (!mm) {
        req.result = -ESRCH;
        goto out;
    }

    mmap_read_lock(mm);

    {
        struct dentry *match_dentry = NULL;
        uint64_t mod_start = U64_MAX;
        uint64_t mod_end = 0;

        vma_iter_init(&vmi, mm, 0);
        for_each_vma(vmi, vma) {
            if (!vma->vm_file)
                continue;

            if (!match_dentry) {
                const char *fname = vma->vm_file->f_path.dentry->d_name.name;
                if (fname && strstr(fname, req.name)) {
                    match_dentry = vma->vm_file->f_path.dentry;
                    mod_start = vma->vm_start;
                    mod_end = vma->vm_end;
                }
            } else {
                if (vma->vm_file->f_path.dentry == match_dentry) {
                    if (vma->vm_start < mod_start)
                        mod_start = vma->vm_start;
                    if (vma->vm_end > mod_end)
                        mod_end = vma->vm_end;
                }
            }
        }

        if (match_dentry) {
            req.base = mod_start;
            req.size = mod_end - mod_start;
            req.result = 0;
            ret = 0;
        }
    }

    mmap_read_unlock(mm);
    mmput(mm);

out:
    if (copy_to_user(ureq, &req, sizeof(req)))
        return -EFAULT;

    return 0; /* status returned in req.result */
}

static long hunt_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
    int ret;

    mutex_lock(&hunt_mutex);

    switch (cmd) {
    case HUNT_IOC_SET_PID: {
        int32_t pid;
        if (copy_from_user(&pid, (void __user *)arg, sizeof(pid))) {
            ret = -EFAULT;
            break;
        }
        ret = hunt_set_pid(pid);
        break;
    }
    case HUNT_IOC_READ_MEM:
        ret = hunt_read_mem((struct hunt_read_req __user *)arg);
        break;
    case HUNT_IOC_GET_MODULE:
        ret = hunt_get_module((struct hunt_module_req __user *)arg);
        break;
    default:
        ret = -ENOTTY;
        break;
    }

    mutex_unlock(&hunt_mutex);
    return ret;
}

static int hunt_open(struct inode *inode, struct file *file)
{
    return 0;
}

static int hunt_release(struct inode *inode, struct file *file)
{
    return 0;
}

static const struct file_operations hunt_fops = {
    .owner          = THIS_MODULE,
    .unlocked_ioctl = hunt_ioctl,
    .open           = hunt_open,
    .release        = hunt_release,
};

static struct miscdevice hunt_misc = {
    .minor = MISC_DYNAMIC_MINOR,
    .name  = HUNT_DEVICE_NAME,
    .fops  = &hunt_fops,
    .mode  = 0666,
};

static int __init hunt_reader_init(void)
{
    int ret;

    ret = misc_register(&hunt_misc);
    if (ret) {
        pr_err("hunt_reader: failed to register misc device: %d\n", ret);
        return ret;
    }

    pr_info("hunt_reader: loaded, device at /dev/%s\n", HUNT_DEVICE_NAME);
    return 0;
}

static void __exit hunt_reader_exit(void)
{
    if (target_pid_struct)
        put_pid(target_pid_struct);

    misc_deregister(&hunt_misc);
    pr_info("hunt_reader: unloaded\n");
}

module_init(hunt_reader_init);
module_exit(hunt_reader_exit);
