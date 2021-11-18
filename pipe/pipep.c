#include <linux/module.h>
#include <linux/init.h>
#include <linux/moduleparam.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/errno.h>
#include <linux/cdev.h>
#include <linux/fs.h>
#include <linux/semaphore.h>

int scull_major;


struct scull_pipe {
    wait_queue_head_t inq, outq;
    char *buffer, *end;
    int buffersize;
    int nreaders, nwriters;
    char *rp, *wp;
    struct semaphore sem;
    struct cdev cdev;
};

int scull_p_buffer = 100;
struct scull_pipe *scull_p_device;


static int scull_p_open(struct inode *inode, struct file *filp) 
{
    struct scull_pipe *dev;
    dev = container_of(inode->i_cdev, struct scull_pipe, cdev);
    printk(KERN_INFO "open");
    filp->private_data = dev;

    if (down_interruptible(&dev->sem))
        return -ERESTARTSYS;
    printk(KERN_INFO "open");
    /*防止多次打开，分配多个buffer*/
    if (!dev->buffer) 
    {
        dev->buffer = kmalloc(scull_p_buffer, GFP_KERNEL);
        if (!dev->buffer) {
            up(&dev->sem);
            return -ENOMEM;
        }
    }

    dev->buffersize = scull_p_buffer;
    
    dev->end = dev->buffer + dev->buffersize;
     
    dev->rp = dev->wp = dev->buffer;

    /*记录按读写打开的次数，方便关闭时的内存释放*/
    if (filp->f_mode & FMODE_READ) 
        dev->nreaders++;

    if (filp->f_mode & FMODE_WRITE) 
        dev->nwriters++;

    up(&dev->sem);

    /*nonseekable_open 通知内核禁止使用llseek()*/
    return nonseekable_open(inode, filp);
}





static int scull_p_release(struct inode *inode, struct file *filp)
{
    struct scull_pipe *dev = filp->private_data;
    
    /*没有使用可中断的方式加锁，释放的时候是不希望被打断的？*/
    down(&dev->sem);
    if (filp->f_mode == FMODE_READ) 
        dev->nreaders--;
    if (filp->f_mode == FMODE_WRITE)
        dev->nwriters--;
    /*当所有打开的文件都关闭的时候就释放内存*/
    if (dev->nreaders + dev->nwriters == 0) 
    {
        kfree(dev->buffer);
        dev->buffer = NULL;
    }
    up(&dev->sem);
    return 0;
}


static ssize_t scull_p_read(struct file *filp, char __user *buf, size_t count, loff_t *f_ops)
{
    struct scull_pipe *dev = filp->private_data;
        
    if (down_interruptible(&dev->sem))    
        return -ERESTARTSYS;
    
    /*读指针和写指针相等buffer内无数据，在这里需要处理阻塞处理和非阻塞的情况*/
    while(dev->rp == dev->wp)
    {
        /*1. 先释放锁*/
        up(&dev->sem);

        /*2. 判断读取方式，非阻塞直接返回*/
        if (filp->f_flags & O_NONBLOCK)
            return -EAGAIN;
        /*3.每次唤醒时判断buffer内有无数据，无数据继续休眠*/
        if (wait_event_interruptible(dev->inq, (dev->rp != dev->wp)))
            return -ERESTARTSYS;
        /*4. 唤醒成功，上锁，开始读取数据*/
        if (down_interruptible(&dev->sem)) 
            return -ERESTARTSYS;
    }
    
    /*检查count*/
    if (dev->wp > dev->rp) 
        count = min(count, (size_t)(dev->wp - dev->rp));
    else 
        count = min(count, (size_t)(dev->end - dev->rp));
    
    /*复制到用户空间*/
    if (copy_to_user(buf, dev->rp, count))
    {
        up(&dev->sem);
        return -EFAULT;
    }

    dev->rp += count;

    if (dev->rp == dev->end) 
        dev->rp = dev->buffer;

    up(&dev->sem);

    wake_up_interruptible(&dev->outq);
    return count;
}

static int spacefree(struct scull_pipe *dev)
{
    if (dev->rp == dev->wp) 
        return dev->buffersize -1;
    return ((dev->rp + dev->buffersize - dev->wp) % dev->buffersize) - 1;
}



static ssize_t scull_p_write(struct file *filp, const char __user *buf, size_t count,
                             loff_t *f_pos) 
{
    struct  scull_pipe *dev = filp->private_data;
     
    /*如果一个进程已经获取锁，那么另一个进程该如何处理？*/
    if (down_interruptible(&dev->sem))
        return -ERESTARTSYS;
    


    /*以下的作用就是手动完成一个等待队列，如果buffer有没有剩余空间，则一直等待*/
    while(spacefree(dev) == 0){
        
        /*声明一个等待队列元素*/
        DEFINE_WAIT(wait);

        up(&dev->sem);
        if (filp->f_flags & O_NONBLOCK)
            return -EAGAIN;
        
        /*将元素加入到等待队列头*/
        prepare_to_wait(&dev->outq, &wait, TASK_INTERRUPTIBLE);
         
        /*进行条件判断，并且通过schedule()让出CPU*/
        if (spacefree(dev) == 0) 
            schedule();
        /*将进程设置为TASK_RUNNING,并移出等待队列*/
        finish_wait(&dev->outq, &wait);
         
        /*检查当前进程是否有信号处理，返回不为0表示有信号需要处理*/
        if (signal_pending(current))
            return -ERESTARTSYS;
        if (down_interruptible(&dev->sem))
            return -ERESTARTSYS;
        
    }

    /*计算可写入的数量*/
    count = min(count, (size_t)spacefree(dev));
    if (dev->wp >= dev->rp)
        count = min(count, (size_t)(dev->end - dev->wp));
    else 
        count = min(count, (size_t)(dev->rp - dev->wp - 1));

    if (copy_from_user(dev->wp, buf, count)){
        up(&dev->sem);
        return -EFAULT;
    }

    dev->wp += count;
    if (dev->wp == dev->end)
        dev->wp = dev->buffer;
    up(&dev->sem);

    
    wake_up_interruptible(&dev->inq);

    return count;
}

struct file_operations scull_pipe_fops = {
    .open = scull_p_open,
    .release = scull_p_release,
    .read = scull_p_read,
    .write = scull_p_write,
};



static int __init scull_p_init(void)
{
    int ret;
    dev_t dev = 0;

    if (scull_major) {
        dev = MKDEV(scull_major, 0);
        ret = register_chrdev_region(dev, 1, "scullp");
    } else {
        ret = alloc_chrdev_region(&dev, 0, 1, "scullp");
        scull_major = MAJOR(dev);
    }
    if (ret < 0) {
        printk(KERN_WARNING "scullp: can't get major %d", scull_major);
        return ret;
    }
    printk(KERN_INFO "scull_major = %d", scull_major);
    scull_p_device = kmalloc(sizeof(struct scull_pipe), GFP_KERNEL);
    if (scull_p_device == NULL) {
        unregister_chrdev_region(dev, 1);
        /*内存分配失败因该返回什么错误代码？*/
        
        return 0;
    }
    memset(scull_p_device, 0, sizeof(struct scull_pipe));
     
    init_waitqueue_head(&scull_p_device->inq);
    init_waitqueue_head(&scull_p_device->outq);
    sema_init(&scull_p_device->sem, 1);

    cdev_init(&scull_p_device->cdev, &scull_pipe_fops);
    scull_p_device->cdev.owner = THIS_MODULE;
    
    ret = cdev_add(&scull_p_device->cdev,dev,1);

    if (ret) {
        printk(KERN_WARNING "Error adding scullpipe%d", ret);
    }
    printk(KERN_INFO "init cpl");
    return 0;
}

static void scull_p_exit(void) 
{
    dev_t dev = 0;
    dev = MKDEV(scull_major, 0);
    cdev_del(&scull_p_device->cdev);
    kfree(scull_p_device->buffer);
    kfree(scull_p_device);
    unregister_chrdev_region(dev, 1);
}

module_init(scull_p_init);
module_exit(scull_p_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("ZHOUKAI");
