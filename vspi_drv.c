/*
 * vspi_drv.c
 *
 *  Created on: Jun 15, 2011
 *      Author: mbehr
 *  (c) M. Behr, 2011
 *
 *  Based on source code and documentation from the book
 *  "Linux Device Drivers" by Alessandro Rubini and Jonathan Corbet,
 *  published by O'Reilly & Associates.
 *  Thanks a lot to Rubini, Corbet and O'Reilly!
 *
 * todo add GPLv2 lic here?
 *
 */
#include <linux/init.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/kernel.h>
#include <linux/slab.h> // kmalloc
#include <linux/fs.h>
#include <linux/errno.h> // for e.g. -ENOMEM, ...
#include <linux/types.h>
#include <linux/fcntl.h>
#include <linux/cdev.h>
#include <linux/semaphore.h>
#include <linux/spi/spidev.h> // for spi specific ioctl
#include <asm/uaccess.h> // for access_ok
#include <linux/wait.h> // wait queues
#include "vspi_drv.h"

// module parameter:
int param_major = VSPI_MAJOR;
module_param(param_major, int, S_IRUGO);

int param_minor = 0;
module_param(param_minor, int, S_IRUGO);

int param_ber = 0; // module parameter bit error rate: 0 = none
module_param(param_ber, int, S_IRUGO|S_IWUSR); // readable by all users in sysfs (/sys/module/vspi_drv/parameters/), changeable only by root
MODULE_PARM_DESC(param_ber, "bit error rate for both directions");

static unsigned long param_speed_cps = 18000000/8; // module parameter speed in cps.
module_param(param_speed_cps, ulong, S_IRUGO); // only readable
MODULE_PARM_DESC(param_speed_cps, "speed in bytes per second");

static unsigned long param_max_bytes_per_ioreq = 4*1024; // todo use page_size constant
module_param(param_max_bytes_per_ioreq, ulong, S_IRUGO);
MODULE_PARM_DESC(param_max_bytes_per_ioreq, "data bytes in biggest supported SPI message");


struct vspi_dev *vspi_devices; // allocated in vspi_drv_init

struct file_operations vspi_fops = {
		.owner = THIS_MODULE,
		.read = vspi_read,
		.write = vspi_write,
		.unlocked_ioctl = vspi_ioctl,
		.open = vspi_open,
		.release = vspi_release
};

#define SPI_MODE_MASK        (SPI_CPHA | SPI_CPOL | SPI_CS_HIGH \
		| SPI_LSB_FIRST | SPI_3WIRE | SPI_LOOP \
		| SPI_NO_CS | SPI_READY)

// exit function on module unload:
// used from _init as well in case of errors!

static void vspi_exit(void)
{
	int i;
	dev_t devno = MKDEV(param_major, param_minor);
	printk( KERN_ALERT "vspi_exit\n");

	// get rid of our devices:
	if (vspi_devices){
		for(i=0; i<VSPI_NR_DEVS; i++){
			cdev_del(&vspi_devices[i].cdev);
			// we don't rely on release being called. So check to free buffers here again:
			if (vspi_devices[i].rp)
				kfree(vspi_devices[i].rp);
			if (vspi_devices[i].wp)
				kfree(vspi_devices[i].wp);
		}
		kfree(vspi_devices);
		vspi_devices=0;
	}

	unregister_chrdev_region( devno, VSPI_NR_DEVS);
}

// setup the char_dev structure for these devices:
static void vspi_setup_cdev(struct vspi_dev *dev, int index)
{
	int err, devno = MKDEV(param_major, param_minor + index);
	cdev_init(&dev->cdev, &vspi_fops);
	dev->cdev.owner = THIS_MODULE;
	dev->cdev.ops = &vspi_fops;
	err = cdev_add(&dev->cdev, devno, 1);
	if (err)
		printk(KERN_NOTICE "Error %d adding vspi_drv%d", err, index);
}

// init function on module load:
static int __init vspi_init(void)
{
	int retval = 0, i;
	dev_t dev=0;

	printk( KERN_ALERT "vspi_drv_init (c) M. Behr, 2011\n");

	if (param_major){
		dev = MKDEV(param_major, param_minor);
		retval = register_chrdev_region(dev, VSPI_NR_DEVS, "vspi_drv");
	}else{
		retval = alloc_chrdev_region(&dev, param_minor, VSPI_NR_DEVS, "vspi_drv");
		param_major = MAJOR(dev);
	}
	if (retval <0 ){
		printk(KERN_WARNING "vspi_drv: can't get major %d\n", param_major);
		return retval;
	}

	// allocate the devices. We could have them static as well, as we don't change the number at load time
	vspi_devices = kmalloc( VSPI_NR_DEVS * sizeof( struct vspi_dev), GFP_KERNEL);
	if (!vspi_devices){
		retval = -ENOMEM;
		goto fail;
	}
	memset(vspi_devices, 0, VSPI_NR_DEVS * sizeof(struct vspi_dev));

	// init each device:
	for (i=0; i< VSPI_NR_DEVS; i++){
		vspi_devices[i].isMaster = ((i==0) ? 1 : 0); // first one is master
		sema_init(&vspi_devices[i].sem, 1); // 1 = count
		vspi_setup_cdev(&vspi_devices[i], i);
		// rp and wp will be allocated in open
	}

	return 0; // 0 = success,
fail:
	vspi_exit();
	return retval;
}


module_init( vspi_init );
module_exit( vspi_exit );

// open:
int vspi_open(struct inode *inode, struct file *filep)
{
	struct vspi_dev *dev;
	dev = container_of(inode->i_cdev, struct vspi_dev, cdev);
	filep->private_data = dev;

	if (dev->isMaster)
		printk( KERN_NOTICE "vspi open master\n");
	else
		printk( KERN_NOTICE "vspi open slave\n");

	if (down_interruptible(&dev->sem))
		return -ERESTARTSYS;

	// allow max 1 user at the same time
	if (dev->isOpened){
		up(&dev->sem);
		return -EUSERS;
	}else
		dev->isOpened+=1;

	// now we're holding/blocking the semaphore.
	if (!dev->rp){
		dev->rp = kmalloc(param_max_bytes_per_ioreq, GFP_KERNEL);
		if (!dev->rp){
			up(&dev->sem);
			return -ENOMEM;
		}
	}
	if (!dev->wp){
		dev->wp = kmalloc(param_max_bytes_per_ioreq, GFP_KERNEL);
		if (!dev->wp){
			up(&dev->sem);
			return -ENOMEM;
		}
	}

	up(&dev->sem);
	// now the semaphore is free again.

	return 0; // success
}

// close/release:
int vspi_release(struct inode *inode, struct file *filep)
{
	struct vspi_dev *dev = filep->private_data;

	if(dev->isMaster)
		printk( KERN_ALERT "vspi_release master\n");
	else
		printk( KERN_ALERT "vspi_release slave\n");

	if (down_interruptible(&dev->sem))
		return -ERESTARTSYS;

	if (!dev->isOpened){
		printk( KERN_WARNING "vspi_release called with no one opened\n");
	} else{
		dev->isOpened-=1;
	}
	if (!dev->isOpened){
		// now free the buffers:
		if (dev->wp){
			kfree(dev->wp);
			dev->wp=0;
		}
		if (dev->rp){
			kfree(dev->rp);
			dev->rp = 0;
		}
	}

	up(&dev->sem);

	return 0; // success
}

static int vspi_message(struct vspi_dev *dev,
		struct spi_ioc_transfer *u_xfers, unsigned n_xfers)
{
	// todo nyi
	return 0;
}

// read:
ssize_t vspi_read(struct file *filep, char __user *buf, size_t count, loff_t *f_pos)
{
	ssize_t retval = -ENOMEM;
	struct vspi_dev *dev = filep->private_data;

	if (count > param_max_bytes_per_ioreq)
		return -EMSGSIZE;


	return retval;
}

// write:
ssize_t vspi_write(struct file *filep, const char __user *buf, size_t count, loff_t *f_pos)
{
	ssize_t retval = -ENOMEM;

	if (count > param_max_bytes_per_ioreq)
		return -EMSGSIZE;


	return retval;
}




// the ioctl implementation. This is our main function. read/write will just use this.
// this is used as .unlocked_ioctl!
long vspi_ioctl(struct file *filep,
		unsigned int cmd, unsigned long arg)
{
	int err=0;
	long retval = 0;
	u32 tmp;
	unsigned n_ioc;
	struct spi_ioc_transfer *ioc;

	struct vspi_dev *dev = filep->private_data;

	if (_IOC_TYPE(cmd) != SPI_IOC_MAGIC)
		return -ENOTTY;

	/* check access direction one here:
	 * IOC_DIR is from the user perspective, while access_ok is from the kernel
	 * perspective
	 */

	if (_IOC_DIR(cmd) & _IOC_READ)
		err = !access_ok(VERIFY_WRITE,
				(void __user *)arg, _IOC_SIZE(cmd));
	if (err == 0 && _IOC_DIR(cmd) & _IOC_WRITE)
		err = !access_ok(VERIFY_READ,
				(void __user *)arg, _IOC_SIZE(cmd));
	if (err)
		return -EFAULT;

	// todo guard against device removal while we do ioctl?
	// do we have to use block the semaphore here?? (will take a long time...)

	switch (cmd){
	/* read requests */
	case SPI_IOC_RD_MODE:
		retval = __put_user(dev->mode & SPI_MODE_MASK,
				(__u8 __user *)arg);
		break;
	case SPI_IOC_RD_LSB_FIRST:
		retval = __put_user((dev->mode & SPI_LSB_FIRST) ? 1 : 0,
				(__u8 __user *)arg);
		break;
	case SPI_IOC_RD_BITS_PER_WORD:
		retval = __put_user(dev->bits_per_word, (__u8 __user *)arg);
		break;
	case SPI_IOC_RD_MAX_SPEED_HZ:
		retval = __put_user(dev->max_speed_hz, (__u32 __user *)arg);
		break;

	/* write requests */
	case SPI_IOC_WR_MODE:
		retval = __get_user(tmp, (__u8 __user *)arg);
		if (retval == 0){
			if (tmp & ~SPI_MODE_MASK){
				retval = -EINVAL;
				break;
			}
			tmp |= dev->mode & ~SPI_MODE_MASK;
			dev->mode = (u8)tmp;
		}
		break;
	case SPI_IOC_WR_LSB_FIRST:
		retval = __get_user(tmp, (__u8 __user *)arg);
		if (0 == retval){
			if (tmp)
				dev->mode |= SPI_LSB_FIRST;
			else
				dev->mode &= ~SPI_LSB_FIRST;
		}
		break;
	case SPI_IOC_WR_BITS_PER_WORD:
		retval = __get_user(tmp, (__u8 __user *)arg);
		if (0 == retval){
			dev->bits_per_word = tmp;
		}
		break;
	case SPI_IOC_WR_MAX_SPEED_HZ:
		retval = __get_user(tmp, (__u32 __user *)arg);
		if (0 == retval){
			dev->max_speed_hz = tmp;
		}
		break;
	default:
		/* segmented and/or full-duplex I/O request */
		if (_IOC_NR(cmd) != _IOC_NR(SPI_IOC_MESSAGE(0))
				|| _IOC_DIR(cmd) != _IOC_WRITE){
			// todo bug: we should allow _IOC_READ without _IOC_WRITE
			// as well. But currently spidev behaves the same way!
			retval = -ENOTTY;
			break;
		}
		tmp = _IOC_SIZE(cmd);
		if ((tmp % sizeof(struct spi_ioc_transfer)) != 0){
			retval = -EINVAL;
			break;
		}
		n_ioc = tmp / sizeof(struct spi_ioc_transfer);
		if (0 == n_ioc)
			break;

		/* copy requests: */
		ioc = kmalloc(tmp, GFP_KERNEL);
		if (!ioc){
			retval = -ENOMEM;
			break;
		}
		if (__copy_from_user(ioc, (void __user *)arg, tmp)){
			kfree(ioc);
			retval = -EFAULT;
			break;
		}
		retval = vspi_message(dev, ioc, n_ioc);
		kfree(ioc);
		break;
	}

	return retval;
}




// todo p3 define license MODULE_LICENSE("proprietary");
MODULE_LICENSE("Dual BSD/GPL");
MODULE_AUTHOR("Matthias Behr");
MODULE_DESCRIPTION("Virtual SPI driver with unrealiability features (patents pending)\n");
