/*
 * mydma.c - 最终版PCIe DMA驱动
 * 一个功能完备的、支持就地操作和中断的DMA回环驱动程序。
 */

#include <linux/init.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/kernel.h>
#include <linux/io.h>
#include <linux/dma-mapping.h>
#include <linux/slab.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/minmax.h>
#include <linux/jiffies.h>
#include <linux/uaccess.h>
#include <linux/compiler.h>
#include <linux/interrupt.h>
#include <linux/sched.h>

// --- 1. 宏定义 ---

#define DRIVER_NAME "mydma"
#define DEVICE_NAME "mydma"
#define MYDMA_VENDOR_ID 0x1234
#define MYDMA_DEVICE_ID 0x5678

// 寄存器偏移地址 (来自第三章的设计)
#define MYDMA_REG_DEV_RESET     0x00
#define MYDMA_REG_INT_ENABLE    0x08
#define MYDMA_REG_RING_ADDR_LO  0x10
#define MYDMA_REG_RING_ADDR_HI  0x18
#define MYDMA_REG_RING_SIZE     0x20
#define MYDMA_REG_QUEUE_HEAD    0x28
#define MYDMA_REG_QUEUE_TAIL    0x30

#define MAX_DMA_TRANSFER_SIZE PAGE_SIZE // 为简化起见，限制单次DMA传输最大为一页

// --- 2. 数据结构定义 ---

// 硬件DMA描述符格式 (根据用户硬件设计)
struct dma_descriptor {
    volatile u32 done;      // 完成标志位。0xFF00: 待处理, 0: 硬件处理完毕
    u32        in_len: 16,  // 输入数据长度 (字节)
               out_len: 16; // 输出缓冲区最大长度 (字节)
    u32        reserved1;   // 保留字段
    u32        reserved2;   // 保留字段
    dma_addr_t in_addr;     // 输入数据的DMA物理地址
    dma_addr_t out_addr;    // 输出缓冲区的DMA物理地址
};

// 驱动侧用于跟踪在途DMA操作的上下文
struct dma_context {
    dma_addr_t dma_addr;
    void *virt_addr;
    size_t size;
};

// 驱动的私有数据结构
struct mydma_dev {
    void __iomem *bar0_virt_addr;   // BAR0的内核虚拟地址
    u32            ring_size;       // 环形缓冲区深度
    struct pci_dev *pdev;           // 指向PCI设备的指针
    int            irq;             // 中断号

    // 字符设备成员
    struct cdev cdev;
    dev_t dev_num;
    struct class *dev_class;
    struct device *device;

    // DMA描述符环形缓冲区
    dma_addr_t ring_buffer_dma_addr;
    struct dma_descriptor *ring_buffer_virt_addr;
    size_t ring_buffer_size;

    // 软件上下文环形数组，用于跟踪DMA缓冲区
    struct dma_context *dma_ctx_ring;

    // 驱动内部维护的队列头尾指针
    u32 queue_head;
    u32 queue_tail;

    // 用于中断驱动IO的等待队列
    wait_queue_head_t dma_wait_queue;
};

// --- 3. 函数原型 (前置声明) ---

static int mydma_open(struct inode *inode, struct file *filp);
static int mydma_release(struct inode *inode, struct file *filp);
static ssize_t mydma_read(struct file *filp, char __user *buf, size_t count, loff_t *f_pos);
static ssize_t mydma_write(struct file *filp, const char __user *buf, size_t count, loff_t *f_pos);
static int mydma_probe(struct pci_dev *pdev, const struct pci_device_id *id);
static void mydma_remove(struct pci_dev *pdev);
static irqreturn_t mydma_irq_handler(int irq, void *dev);
static int mydma_chrdev_setup(struct mydma_dev *priv_dev);
static void mydma_chrdev_cleanup(struct mydma_dev *priv_dev);

// --- 4. 全局变量定义 ---

// PCI设备ID表
static const struct pci_device_id mydma_id_table[] = {
    { PCI_DEVICE(MYDMA_VENDOR_ID, MYDMA_DEVICE_ID) },
    { 0, }
};
MODULE_DEVICE_TABLE(pci, mydma_id_table);

// 文件操作结构体
static const struct file_operations mydma_fops = {
    .owner   = THIS_MODULE,
    .open    = mydma_open,
    .release = mydma_release,
    .read    = mydma_read,
    .write   = mydma_write,
};

// PCI驱动结构体
static struct pci_driver mydma_driver = {
    .name     = DRIVER_NAME,
    .id_table = mydma_id_table,
    .probe    = mydma_probe,
    .remove   = mydma_remove,
};


// --- 5. 函数实现 ---

static int mydma_open(struct inode *inode, struct file *filp)
{
    struct mydma_dev *priv_dev = container_of(inode->i_cdev, struct mydma_dev, cdev);
    filp->private_data = priv_dev;
    pr_info("mydma: open() called\n");
    return 0;
}

static int mydma_release(struct inode *inode, struct file *filp)
{
    pr_info("mydma: release() called\n");
    return 0;
}

static ssize_t mydma_read(struct file *filp, char __user *buf, size_t count, loff_t *f_pos)
{
    struct mydma_dev *priv_dev = filp->private_data;
    u32 hw_head;
    long timeout;
    int ret;
    struct dma_context *ctx;
    struct dma_descriptor *desc;
    size_t bytes_to_copy;

    pr_info("mydma: read() called, count=%zu\n", count);

    hw_head = readl(priv_dev->bar0_virt_addr + MYDMA_REG_QUEUE_HEAD);

    // 如果驱动的头指针与硬件的头指针相同，说明没有已完成的任务，需要等待
    if (priv_dev->queue_head == hw_head) {
        // 在等待队列上休眠，直到被中断唤醒或超时
        timeout = wait_event_interruptible_timeout(
                      priv_dev->dma_wait_queue,
                      // 唤醒条件：硬件头指针前进了
                      readl(priv_dev->bar0_virt_addr + MYDMA_REG_QUEUE_HEAD) != priv_dev->queue_head,
                      msecs_to_jiffies(5000) // 5秒超时
                  );
        if (timeout == 0) { dev_err(&priv_dev->pdev->dev, "Read timeout!\n"); return -ETIMEDOUT; }
        if (timeout < 0) { dev_err(&priv_dev->pdev->dev, "Read interrupted!\n"); return timeout; }
        hw_head = readl(priv_dev->bar0_virt_addr + MYDMA_REG_QUEUE_HEAD);
    }

    // 获取已完成任务的软件上下文和硬件描述符
    ctx = &priv_dev->dma_ctx_ring[priv_dev->queue_head];
    desc = &priv_dev->ring_buffer_virt_addr[priv_dev->queue_head];

    // 安全检查：确保唤醒后描述符确实已完成
    if (desc->done != 0) {
        dev_err(&priv_dev->pdev->dev, "DMA descriptor %u still not done (0x%x) after wake-up!\n",
                priv_dev->queue_head, desc->done);
        return -EIO;
    }
    rmb(); // 读内存屏障，确保先读取done标志位，再访问DMA缓冲区内容

    bytes_to_copy = min(count, ctx->size);

    // 将DMA完成的数据从内核空间拷贝到用户空间
    ret = copy_to_user(buf, ctx->virt_addr, bytes_to_copy);
    if (ret) {
        dev_err(&priv_dev->pdev->dev, "read: copy_to_user failed (bytes not copied: %d)\n", ret);
    }

    pr_info("mydma: Read %zu bytes from completed DMA descriptor %u.\n", bytes_to_copy, priv_dev->queue_head);

    // 释放本次DMA操作使用的缓冲区
    dma_free_coherent(&priv_dev->pdev->dev, ctx->size, ctx->virt_addr, ctx->dma_addr);
    memset(ctx, 0, sizeof(*ctx)); // 清理上下文槽位

    // 驱动头指针前进
    priv_dev->queue_head = (priv_dev->queue_head + 1) % priv_dev->ring_size;

    return ret ? -EFAULT : bytes_to_copy; // 如果拷贝失败返回错误，否则返回拷贝的字节数
}

static ssize_t mydma_write(struct file *filp, const char __user *buf, size_t count, loff_t *f_pos)
{
    struct mydma_dev *priv_dev = filp->private_data;
    u32 next_tail;
    u32 hw_head;
    struct dma_context *ctx;
    struct dma_descriptor *desc;
    int ret;

    if (count == 0) return 0;
    if (count > MAX_DMA_TRANSFER_SIZE) {
        dev_warn(&priv_dev->pdev->dev, "Write size %zu exceeds max %lu\n", count, MAX_DMA_TRANSFER_SIZE);
        return -EINVAL;
    }

    // 检查环形缓冲区是否已满
    next_tail = (priv_dev->queue_tail + 1) % priv_dev->ring_size;
    hw_head = readl(priv_dev->bar0_virt_addr + MYDMA_REG_QUEUE_HEAD);
    if (next_tail == hw_head) {
        dev_warn(&priv_dev->pdev->dev, "DMA queue is full\n");
        return -EBUSY;
    }

    ctx = &priv_dev->dma_ctx_ring[priv_dev->queue_tail];

    // 为本次“就地”DMA操作分配一个缓冲区
    ctx->size = count;
    ctx->virt_addr = dma_alloc_coherent(&priv_dev->pdev->dev, count, &ctx->dma_addr, GFP_KERNEL);
    if (!ctx->virt_addr) {
        dev_err(&priv_dev->pdev->dev, "Failed to allocate DMA buffer\n");
        return -ENOMEM;
    }

    // 从用户空间拷贝数据到DMA缓冲区
    ret = copy_from_user(ctx->virt_addr, buf, count);
    if (ret) {
        dev_err(&priv_dev->pdev->dev, "write: copy_from_user failed\n");
        dma_free_coherent(&priv_dev->pdev->dev, ctx->size, ctx->virt_addr, ctx->dma_addr);
        memset(ctx, 0, sizeof(*ctx));
        return -EFAULT;
    }

    // 填充硬件描述符
    desc = &priv_dev->ring_buffer_virt_addr[priv_dev->queue_tail];
    desc->in_addr = ctx->dma_addr;
    desc->out_addr = ctx->dma_addr; // 就地操作：输入和输出地址相同
    desc->in_len = ctx->size;
    desc->out_len = ctx->size;
    desc->done = 0xFF00; // 设置为待处理状态

    wmb(); // 写内存屏障，确保描述符内容在更新尾指针前已写入内存

    // 更新硬件的尾指针，正式提交任务
    priv_dev->queue_tail = next_tail;
    writel(priv_dev->queue_tail, priv_dev->bar0_virt_addr + MYDMA_REG_QUEUE_TAIL);

    pr_info("mydma: Submitted in-place DMA req %u, dma_addr=0x%pad, len=%zu\n",
            (next_tail - 1 + priv_dev->ring_size) % priv_dev->ring_size, &ctx->dma_addr, count);

    return count;
}

static int mydma_chrdev_setup(struct mydma_dev *priv_dev)
{
    int ret;
    struct pci_dev *pdev = priv_dev->pdev;

    ret = alloc_chrdev_region(&priv_dev->dev_num, 0, 1, DEVICE_NAME);
    if (ret) { dev_err(&pdev->dev, "Failed to allocate chrdev region\n"); return ret; }

    priv_dev->dev_class = class_create(THIS_MODULE, DEVICE_NAME);
    if (IS_ERR(priv_dev->dev_class)) {
        ret = PTR_ERR(priv_dev->dev_class);
        dev_err(&pdev->dev, "Failed to create device class\n");
        goto err_unregister_chrdev;
    }

    cdev_init(&priv_dev->cdev, &mydma_fops);
    priv_dev->cdev.owner = THIS_MODULE;

    ret = cdev_add(&priv_dev->cdev, priv_dev->dev_num, 1);
    if (ret) {
        dev_err(&pdev->dev, "Failed to add cdev\n");
        goto err_class_destroy;
    }

    priv_dev->device = device_create(priv_dev->dev_class, &pdev->dev, priv_dev->dev_num, NULL, DEVICE_NAME "0");
    if (IS_ERR(priv_dev->device)) {
        ret = PTR_ERR(priv_dev->device);
        dev_err(&pdev->dev, "Failed to create device node\n");
        goto err_cdev_del;
    }
    pr_info("mydma: Character device created at /dev/%s0\n", DEVICE_NAME);
    return 0;

err_cdev_del: cdev_del(&priv_dev->cdev);
err_class_destroy: class_destroy(priv_dev->dev_class);
err_unregister_chrdev: unregister_chrdev_region(priv_dev->dev_num, 1);
    return ret;
}

static void mydma_chrdev_cleanup(struct mydma_dev *priv_dev)
{
    if (!priv_dev) return;
    if (priv_dev->device) device_destroy(priv_dev->dev_class, priv_dev->dev_num);
    if (priv_dev->cdev.owner) cdev_del(&priv_dev->cdev);
    if (priv_dev->dev_class) class_destroy(priv_dev->dev_class);
    if (priv_dev->dev_num) unregister_chrdev_region(priv_dev->dev_num, 1);
    pr_info("mydma: Character device cleaned up\n");
}

static irqreturn_t mydma_irq_handler(int irq, void *dev)
{
    struct mydma_dev *priv_dev = (struct mydma_dev *)dev;
    pr_info("mydma: Interrupt received!\n");
    wake_up_interruptible(&priv_dev->dma_wait_queue);
    return IRQ_HANDLED;
}

static int mydma_probe(struct pci_dev *pdev, const struct pci_device_id *id)
{
    int ret;
    struct mydma_dev *priv_dev;

    priv_dev = devm_kzalloc(&pdev->dev, sizeof(struct mydma_dev), GFP_KERNEL);
    if (!priv_dev) { return -ENOMEM; }
    pci_set_drvdata(pdev, priv_dev);
    priv_dev->pdev = pdev;

    ret = pci_enable_device(pdev);
    if (ret) { dev_err(&pdev->dev, "pci_enable_device failed\n"); return ret; }

    ret = pci_request_regions(pdev, DRIVER_NAME);
    if (ret) { dev_err(&pdev->dev, "pci_request_regions failed\n"); goto err_disable_device; }

    priv_dev->bar0_virt_addr = pci_iomap(pdev, 0, 0);
    if (!priv_dev->bar0_virt_addr) { ret = -EIO; dev_err(&pdev->dev, "pci_iomap failed\n"); goto err_release_regions; }

    writel(0x80000000, priv_dev->bar0_virt_addr + MYDMA_REG_DEV_RESET);

    ret = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(64));
    if (ret) { ret = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(32)); }
    if (ret) { dev_err(&pdev->dev, "DMA configuration failed\n"); goto err_iounmap; }

    u32 ring_size = 128;
    writel(ring_size, priv_dev->bar0_virt_addr + MYDMA_REG_RING_SIZE);
    priv_dev->ring_size = readl(priv_dev->bar0_virt_addr + MYDMA_REG_RING_SIZE);
    if (priv_dev->ring_size != ring_size) { ret = -EIO; dev_err(&pdev->dev, "Ring size mismatch\n"); goto err_iounmap; }
    pr_info("mydma: Set ring size to %u\n", priv_dev->ring_size);

    priv_dev->ring_buffer_size = priv_dev->ring_size * sizeof(struct dma_descriptor);
    priv_dev->ring_buffer_virt_addr = dma_alloc_coherent(&pdev->dev, priv_dev->ring_buffer_size, &priv_dev->ring_buffer_dma_addr, GFP_KERNEL);
    if (!priv_dev->ring_buffer_virt_addr) { ret = -ENOMEM; dev_err(&pdev->dev, "ring buffer alloc failed\n"); goto err_iounmap; }
    pr_info("mydma: Allocated ring buffer, dma_addr=0x%pad\n", &priv_dev->ring_buffer_dma_addr);

    priv_dev->dma_ctx_ring = devm_kcalloc(&pdev->dev, priv_dev->ring_size, sizeof(struct dma_context), GFP_KERNEL);
    if (!priv_dev->dma_ctx_ring) { ret = -ENOMEM; goto err_free_ring; }
    pr_info("mydma: Allocated software context ring\n");

    writel(upper_32_bits(priv_dev->ring_buffer_dma_addr), priv_dev->bar0_virt_addr + MYDMA_REG_RING_ADDR_HI);
    writel(lower_32_bits(priv_dev->ring_buffer_dma_addr), priv_dev->bar0_virt_addr + MYDMA_REG_RING_ADDR_LO);

    ret = pci_alloc_irq_vectors(pdev, 1, 1, PCI_IRQ_MSI);
    if (ret < 0) { dev_err(&pdev->dev, "pci_alloc_irq_vectors failed\n"); goto err_free_ctx; }

    priv_dev->irq = pci_irq_vector(pdev, 0);
    ret = request_irq(priv_dev->irq, mydma_irq_handler, 0, DRIVER_NAME, priv_dev);
    if (ret) { dev_err(&pdev->dev, "request_irq failed\n"); goto err_free_irq_vectors; }
    pr_info("mydma: Requested IRQ %d\n", priv_dev->irq);

    init_waitqueue_head(&priv_dev->dma_wait_queue);
    writel(1, priv_dev->bar0_virt_addr + MYDMA_REG_INT_ENABLE);
    pr_info("mydma: Hardware interrupts enabled\n");

    priv_dev->queue_head = 0;
    priv_dev->queue_tail = 0;

    ret = mydma_chrdev_setup(priv_dev);
    if (ret) { goto err_free_irq; }

    pr_info("mydma: probe successful\n");
    return 0;

err_free_irq:
    writel(0, priv_dev->bar0_virt_addr + MYDMA_REG_INT_ENABLE);
    free_irq(priv_dev->irq, priv_dev);
err_free_irq_vectors:
    pci_free_irq_vectors(pdev);
err_free_ctx:
    /* devm_kcalloc for dma_ctx_ring is auto-freed */
err_free_ring:
    dma_free_coherent(&pdev->dev, priv_dev->ring_buffer_size, priv_dev->ring_buffer_virt_addr, priv_dev->ring_buffer_dma_addr);
err_iounmap:
    pci_iounmap(pdev, priv_dev->bar0_virt_addr);
err_release_regions:
    pci_release_regions(pdev);
err_disable_device:
    pci_disable_device(pdev);
    pci_set_drvdata(pdev, NULL);
    return ret;
}

static void mydma_remove(struct pci_dev *pdev)
{
    struct mydma_dev *priv_dev = pci_get_drvdata(pdev);
    if (!priv_dev) return;

    pr_info("mydma: remove function called\n");

    mydma_chrdev_cleanup(priv_dev);

    writel(0, priv_dev->bar0_virt_addr + MYDMA_REG_INT_ENABLE);
    free_irq(priv_dev->irq, priv_dev);
    pci_free_irq_vectors(pdev);
#if 0 // remove drv then powerof vm machine bug in host kernel
    writel(0, priv_dev->bar0_virt_addr + MYDMA_REG_RING_ADDR_HI);
    writel(0, priv_dev->bar0_virt_addr + MYDMA_REG_RING_ADDR_LO);
    writel(0x80000000, priv_dev->bar0_virt_addr + MYDMA_REG_DEV_RESET);
#endif

    if (priv_dev->ring_buffer_virt_addr) {
        dma_free_coherent(&pdev->dev, priv_dev->ring_buffer_size, priv_dev->ring_buffer_virt_addr, priv_dev->ring_buffer_dma_addr);
    }

    if (priv_dev->bar0_virt_addr) {
        pci_iounmap(pdev, priv_dev->bar0_virt_addr);
    }

    pci_release_regions(pdev);
    pci_disable_device(pdev);
    pr_info("mydma: device removed successfully\n");
}


// --- 6. Module Init/Exit ---
static int __init mydma_init(void)
{
    pr_info("mydma: driver loading\n");
    return pci_register_driver(&mydma_driver);
}

static void __exit mydma_exit(void)
{
    pr_info("mydma: driver unloading\n");
    pci_unregister_driver(&mydma_driver);
}

module_init(mydma_init);
module_exit(mydma_exit);

// --- 7. Module Metadata ---
MODULE_LICENSE("GPL");
MODULE_AUTHOR("mr.linux@foxmail.com");
MODULE_DESCRIPTION("Final PCIe DMA loopback driver with in-place DMA and MSI interrupts.");
