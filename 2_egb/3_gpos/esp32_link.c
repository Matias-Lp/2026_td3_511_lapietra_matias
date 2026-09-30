#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/types.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/of.h>
#include <linux/serdev.h>
#include <linux/kfifo.h>
#include <linux/wait.h>
#include <linux/uaccess.h>
#include <linux/err.h>
#include <linux/string.h>

#include "esp32_link.h"

#define DEVICE_NAME "esp32link"
#define CLASS_NAME  "td3"
#define MAX_LINE    64
#define FIFO_SIZE   256

MODULE_LICENSE("GPL");
MODULE_AUTHOR("TD3 -- UTN FRA");
MODULE_DESCRIPTION("Char device sobre serdev: protocolo set/get/exec con un ESP32-S3 por UART");

static dev_t           dev_num;
static struct cdev     esp32_cdev;
static struct class   *esp32_class;
static struct device  *esp32_device;
static struct serdev_device *esp32_serdev;

/* read() como productor-consumidor: receive_buf() (productor) empuja cada
 * linea completa acá; esp32_read() (consumidor) se bloquea si esta vacio. */
static struct kfifo rx_fifo;
static DECLARE_WAIT_QUEUE_HEAD(rx_wait);

/* Ensamblado de bytes entrantes hasta encontrar un '\n'. */
static char   linebuf[MAX_LINE];
static size_t linelen;

/* ===================== serdev: recepcion ===================== */

static size_t esp32_receive_buf(struct serdev_device *serdev,
                                 const unsigned char *data, size_t count)
{
    size_t i;

    for (i = 0; i < count; i++) {
        if (linelen < sizeof(linebuf) - 1)
            linebuf[linelen++] = data[i];

        if (data[i] == '\n') {
            kfifo_in(&rx_fifo, linebuf, linelen);
            wake_up_interruptible(&rx_wait);
            linelen = 0;
        }
    }

    return count; /* consumimos todo lo que nos dieron */
}

/* serdev_device_write() exige un write_wakeup registrado para aceptar un
 * timeout != 0 -- lo usa para saber cuando se libero espacio en el buffer
 * de transmision mientras espera. Sin el, devuelve -EINVAL de entrada. No
 * necesitamos hacer nada especial acá, alcanza con que exista. */
static void esp32_write_wakeup(struct serdev_device *serdev)
{
}

static const struct serdev_device_ops esp32_serdev_ops = {
    .receive_buf  = esp32_receive_buf,
    .write_wakeup = esp32_write_wakeup,
};

/* ===================== file_operations ===================== */

static int esp32_open(struct inode *inode, struct file *file)
{
    return 0;
}

static int esp32_release(struct inode *inode, struct file *file)
{
    return 0;
}

static ssize_t esp32_read(struct file *file, char __user *buf,
                           size_t len, loff_t *off)
{
    unsigned int copiados;
    int ret;

    if (kfifo_is_empty(&rx_fifo)) {
        if (file->f_flags & O_NONBLOCK)
            return -EAGAIN;
        if (wait_event_interruptible(rx_wait, !kfifo_is_empty(&rx_fifo)))
            return -ERESTARTSYS;
    }

    ret = kfifo_to_user(&rx_fifo, buf, len, &copiados);
    if (ret)
        return ret;

    return copiados;
}

static ssize_t esp32_write(struct file *file, const char __user *buf,
                            size_t len, loff_t *off)
{
    char   local[MAX_LINE];
    size_t n = len;
    int    ret;

    if (n >= sizeof(local))
        n = sizeof(local) - 1; /* nunca copiar mas de lo que entra en local[] */

    if (copy_from_user(local, buf, n))
        return -EFAULT;
    local[n] = '\0';

    /* Asegurar el delimitador de linea que espera el firmware, sin pisar
     * el limite del buffer local. */
    if ((n == 0 || local[n - 1] != '\n') && n < sizeof(local) - 1) {
        local[n++] = '\n';
        local[n] = '\0';
    }

    ret = serdev_device_write(esp32_serdev, local, n, msecs_to_jiffies(1000));
    printk(KERN_INFO "esp32_link: write() pidio %zu bytes, serdev_device_write devolvio %d\n",
           n, ret);
    if (ret < 0) {
        printk(KERN_ERR "esp32_link: serdev_device_write() fallo en write() (%d)\n", ret);
        return ret;
    }
    if (ret != n) {
        printk(KERN_WARNING "esp32_link: write parcial, se mandaron %d de %zu bytes\n",
               ret, n);
    }

    return len; /* se consumio todo el buffer original */
}

static const struct file_operations esp32_fops = {
    .owner   = THIS_MODULE,
    .open    = esp32_open,
    .release = esp32_release,
    .read    = esp32_read,
    .write   = esp32_write,
};

/* ===================== serdev: probe/remove ===================== */

static int esp32_probe(struct serdev_device *serdev)
{
    u32 baudrate = 115200; /* valor por defecto si el DT no trae current-speed */
    int ret;

    esp32_serdev = serdev;
    serdev_device_set_client_ops(serdev, &esp32_serdev_ops);

    ret = serdev_device_open(serdev);
    if (ret)
        return ret;

    of_property_read_u32(serdev->dev.of_node, "current-speed", &baudrate);
    serdev_device_set_baudrate(serdev, baudrate);
    serdev_device_set_flow_control(serdev, false);

    ret = kfifo_alloc(&rx_fifo, FIFO_SIZE, GFP_KERNEL);
    if (ret)
        goto err_close;

    ret = alloc_chrdev_region(&dev_num, 0, 1, DEVICE_NAME);
    if (ret < 0) {
        printk(KERN_ERR "esp32_link: no se pudo reservar major/minor\n");
        goto err_fifo;
    }

    cdev_init(&esp32_cdev, &esp32_fops);
    esp32_cdev.owner = THIS_MODULE;
    ret = cdev_add(&esp32_cdev, dev_num, 1);
    if (ret < 0) {
        printk(KERN_ERR "esp32_link: no se pudo registrar el cdev\n");
        goto err_chrdev;
    }

    esp32_class = class_create(CLASS_NAME);
    if (IS_ERR(esp32_class)) {
        printk(KERN_ERR "esp32_link: no se pudo crear la clase\n");
        ret = PTR_ERR(esp32_class);
        goto err_cdev;
    }

    esp32_device = device_create(esp32_class, NULL, dev_num, NULL, DEVICE_NAME);
    if (IS_ERR(esp32_device)) {
        printk(KERN_ERR "esp32_link: no se pudo crear el dispositivo\n");
        ret = PTR_ERR(esp32_device);
        goto err_class;
    }

    dev_info(&serdev->dev, "esp32_link: listo, baudrate=%u, /dev/%s\n",
             baudrate, DEVICE_NAME);
    return 0;

err_class:
    class_destroy(esp32_class);
err_cdev:
    cdev_del(&esp32_cdev);
err_chrdev:
    unregister_chrdev_region(dev_num, 1);
err_fifo:
    kfifo_free(&rx_fifo);
err_close:
    serdev_device_close(serdev);
    return ret;
}

static void esp32_remove(struct serdev_device *serdev)
{
    device_destroy(esp32_class, dev_num);
    class_destroy(esp32_class);
    cdev_del(&esp32_cdev);
    unregister_chrdev_region(dev_num, 1);
    kfifo_free(&rx_fifo);
    serdev_device_close(serdev);
    dev_info(&serdev->dev, "esp32_link: modulo descargado\n");
}

static const struct of_device_id esp32_of_match[] = {
    { .compatible = "td3,esp32-link" },
    { }
};
MODULE_DEVICE_TABLE(of, esp32_of_match);

static struct serdev_device_driver esp32_driver = {
    .probe  = esp32_probe,
    .remove = esp32_remove,
    .driver = {
        .name           = "esp32_link",
        .of_match_table = esp32_of_match,
    },
};
module_serdev_device_driver(esp32_driver);
