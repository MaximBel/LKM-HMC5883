#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>

#include <linux/i2c.h>
#include <linux/i2c-dev.h>

#include <linux/slab.h>
#include <linux/gfp.h>

#include <linux/kthread.h>
#include <linux/interrupt.h>
#include <linux/gpio.h>

#include <linux/delay.h>

#include <linux/circ_buf.h>
#include <asm-generic/barrier.h>

#include <linux/uaccess.h>

//------------------------------------------------------------------------------- ring buffer and its methods

#define OUT_BUF_SIZE 128 // ACHTUNG!!! Must the digit with all 1 in binary plus 1(example: 0b01111111 + 1 -> 0b10000000)
static struct circ_buf outputBuffer;

static inline void InsertDataToRing(char data) {

	unsigned long head = outputBuffer.head;
	unsigned long tail = ACCESS_ONCE(outputBuffer.tail);

	if (CIRC_SPACE(head, tail, OUT_BUF_SIZE) >= 1) {

		outputBuffer.buf[head] = data;

		outputBuffer.head = (head + 1) & (OUT_BUF_SIZE - 1);

	}

}

static inline char GetDataFromRing(void) {

	char outputData = 0;
	unsigned long head = ACCESS_ONCE(outputBuffer.head);
	unsigned long tail = outputBuffer.tail;

	if (CIRC_CNT(head, tail, OUT_BUF_SIZE) >= 1) {

		outputData = outputBuffer.buf[tail];

		outputBuffer.tail = (tail + 1) & (OUT_BUF_SIZE - 1);

	}

	return outputData;

}

static inline u8 GetSpaceInRing(void) {

	return CIRC_SPACE(outputBuffer.head, ACCESS_ONCE(outputBuffer.tail),
			OUT_BUF_SIZE);

}

static inline u8 GetDataCountInRing(void) {

	return CIRC_CNT(ACCESS_ONCE(outputBuffer.head),
			ACCESS_ONCE(outputBuffer.tail), OUT_BUF_SIZE);

}

//---------------------------------------------------------------------------------------

static const struct i2c_device_id hmc_i2c_idtable[] = { { "hmc5883", 0 }, { } };
MODULE_DEVICE_TABLE( i2c, hmc_i2c_idtable);

static struct i2c_client *hmc_client = NULL;

static const int drdyPin = 4; // data ready irq pin

static int hmc_i2c_probe(struct i2c_client *drv_client,
		const struct i2c_device_id *id) {

	printk(KERN_INFO "HMC5883: device probed with address 0x%X \r\n", drv_client->addr);

	hmc_client = drv_client;

	return 0;

}

static int hmc_i2c_remove(struct i2c_client *drv_client) {

	printk(KERN_INFO "HMC5883: device removed \r\n");

	hmc_client = NULL;

	return 0;
}

static struct i2c_driver hmc_i2c_driver = { .driver = { .name = "hmc5883", },

.probe = hmc_i2c_probe, .remove = hmc_i2c_remove, .id_table = hmc_i2c_idtable, };

//----------------------------------------------------------------------------------------

static struct task_struct * usThread = NULL;
static u8 dataBufferPointer[6] = { 0, 0, 0, 0, 0, 0 };

static int us_Task(void* Params);

static int us_Task(void* Params) {

	while (!kthread_should_stop()) {

		if (i2c_smbus_read_i2c_block_data(hmc_client, 0x03, 6,
				dataBufferPointer) != 6) {

			// nothing to do here
			// maybe need to signalize error to user

		}

		msleep(50);

	}

	kfree(dataBufferPointer);

	printk(KERN_INFO "HMC5883: thread has finalize. \r\n");

	return 0;

}

//----------------------------------------------------------------------------------------

#define DEVICE_NAME					"hmc5883"
#define DEVICE_CLASS 				"Magnitometer"

//-----------------
static struct class* hmc_Class = NULL;
static struct device* hmc_Device = NULL;
static int majorNumber;
static bool outputDataAvailable = false;

static int dev_open(struct inode *inodep, struct file *filep) {

	printk(KERN_INFO "HMC5883: Device opened\r\n");

	return 0;
}

static ssize_t dev_read(struct file *filep, char *buffer, size_t len, loff_t *offset) {

	int error_count = 1;
	int outputLength = 0;
	char out = 0;
	char tempData[50];

	if (outputDataAvailable == false) {

		sprintf(tempData, "X:%05d Y:%05d Z:%05d\r\n",
				((dataBufferPointer[0] << 8) + dataBufferPointer[1]),
				((dataBufferPointer[2] << 8) + dataBufferPointer[3]),
				((dataBufferPointer[4] << 8) + dataBufferPointer[5]));

		outputLength = strlen(tempData);

		if (GetSpaceInRing() >= outputLength) {

			int i;

			for (i = 0; i < outputLength; i++) {

				InsertDataToRing(tempData[i]);

			}

			outputDataAvailable = true;

		} else {

			printk(KERN_ERR "HMC5883: Not enough data space in ring buffer to put it in the world!\r\n");

			return -ENOMEM;

		}

	}

	if (GetDataCountInRing() > 0) {

		out = GetDataFromRing();

		error_count = copy_to_user(buffer, &out, 1);

		if (error_count == 0) {

			printk(KERN_INFO "HMC5883: Data was sended to user.\n");

			return 1;

		} else {

			printk(KERN_INFO "HMC5883 Fail to write data to user\r\n");
			return -EFAULT;

		}

	} else {

		outputDataAvailable = false;

		return 0;

	}

}

static ssize_t dev_write(struct file *filep, const char *buffer, size_t len, loff_t *offset) {

	printk(KERN_INFO "HMC5883: Nothing to write from user\r\n");

	return 0;
}

static int dev_release(struct inode *inodep, struct file *filep) {

	printk(KERN_INFO "HMC5883: Device released\r\n");

	return 0;
}

static struct file_operations fops = { .open = dev_open, .read = dev_read,
		.write = dev_write, .release = dev_release };

//----------------------------------------------------------------------------------

static int __init magni_init(void) {
	int returnState = -1;
	u8 ir_data[4];
	u8 configData[3];

	//-------------------------------------------------------------------------------------

	outputBuffer.buf = kmalloc(OUT_BUF_SIZE, GFP_KERNEL);

	if(outputBuffer.buf == NULL) {

		returnState = -ENOMEM;

		goto destroy_out_buf;

	}

	//-------------------------------------------------------------------------------------
	returnState = i2c_add_driver(&hmc_i2c_driver);

	if(returnState) {

		goto destroy_i2c;

	}

	if(hmc_client == NULL) {

		returnState = -ENODEV;

		goto destroy_i2c;

	}

	//---------------------------------------------------------------------------------------

	i2c_smbus_read_i2c_block_data(hmc_client, 0x0A, 3, ir_data);

	if(ir_data[0] != 72 || ir_data[1] != 52 || ir_data[2] != 51) {

		returnState = -ENODEV;

		goto destroy_i2c;

	}

	configData[0] = 0x10;	//0x0C; // 7.5Hz, normal meas., 1 sample

	configData[1] = 0x20;// default gain +- 1.3 Ga

	configData[2] = 0x00;

	if(i2c_smbus_write_byte_data(hmc_client, 0x00, configData[0]) != 0 ||
			i2c_smbus_write_byte_data(hmc_client, 0x01, configData[1]) != 0 ||
			i2c_smbus_write_byte_data(hmc_client, 0x02, configData[2]) != 0) {

		returnState = -EBADR;

		goto destroy_i2c;

	}

	printk(KERN_INFO "HMC5883: sensor initialised. \r\n");

	//--------------------------------------------------------------------------------------

	usThread = kthread_run(us_Task, NULL, "HMC5883 thread");

	if(!usThread || usThread == ERR_PTR(-ENOMEM)) {

		returnState = -((int)usThread);

		goto destroy_thread;

	}
	printk(KERN_INFO "HMC5883: thread was started.\r\n");

	//---------------------------------------------------------------------------------------

	majorNumber = register_chrdev(0, DEVICE_NAME, &fops);

	if (majorNumber<0) {
		printk(KERN_ERR "Failed to register a major number of US-015\n");
		return majorNumber;
	}

	hmc_Class = class_create(THIS_MODULE, DEVICE_CLASS);
	if (IS_ERR(hmc_Class)) {
		unregister_chrdev(majorNumber, DEVICE_NAME);
		printk(KERN_ERR "Failed to register device class\n");
		return PTR_ERR(hmc_Class);
	}

	hmc_Device = device_create(hmc_Class, NULL, MKDEV(majorNumber, 0), NULL, DEVICE_NAME);
	if (IS_ERR(hmc_Device)) {

		goto destroy_char_dev;

		returnState = PTR_ERR(hmc_Device);
	}

	//-------------------------------------------------------

	printk(KERN_INFO "HMC5883: module complitely loaded. \r\n");

	return 0;

	//----------------------ERRORS------------------------------------

	destroy_char_dev:

	class_unregister(hmc_Class);
	class_destroy(hmc_Class);
	unregister_chrdev(majorNumber, DEVICE_NAME);
	printk(KERN_INFO "HMC5883: device released.\r\n");

	//------------------------------------------------------

	destroy_thread:

	if(usThread) {
		kthread_stop(usThread);

		printk(KERN_INFO "US-015: thread removed.\r\n");
	}

	//-------------------------------------------------------

	destroy_i2c:

	i2c_del_driver(&hmc_i2c_driver);

	printk(KERN_INFO "HMC5883: i2c driver complitely removed. \r\n");

	//-------------------------------------------------------

	destroy_out_buf:

	return returnState;

}

static void __exit magni_exit(void) {
	int unregResult = -1;

	//----------------------
	device_destroy(hmc_Class, MKDEV(majorNumber, 0));
	class_unregister(hmc_Class);
	class_destroy(hmc_Class);
	unregister_chrdev(majorNumber, DEVICE_NAME);
	printk(KERN_INFO "HMC5883: device released.\r\n");

	//------------------------------------------------------

	if(usThread) {
		unregResult = kthread_stop(usThread);
		printk(KERN_INFO "US-015: thread removed with state: %d\r\n", unregResult);
	}

	//-------------------------------------------------------

	i2c_del_driver(&hmc_i2c_driver);

	printk(KERN_INFO "HMC5883: i2c driver complitely removed. \r\n");

	//-------------------------------------------------------

	kfree(outputBuffer.buf);

	//--------------------------------------------------------

	printk(KERN_INFO "HMC5883: module removed. \r\n");

}

module_init( magni_init);
module_exit( magni_exit);

MODULE_AUTHOR("Beley Maxim");
MODULE_DESCRIPTION("Simple driver for Honeywell magnitometer HMC5883");
MODULE_VERSION("1.0");
MODULE_LICENSE("GPL");
