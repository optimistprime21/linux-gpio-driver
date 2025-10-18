#include <linux/ioctl.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/io.h>
#include <linux/ioctl.h>
#include <linux/printk.h>
#include <linux/types.h>
#include <linux/slab.h>
#include <linux/cdev.h>
#include <linux/device.h> 
#include <linux/interrupt.h> 
#include <linux/wait.h>   
#include <linux/uaccess.h>
#include "gpio_ioctrl.h"

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Seren Sila Uysal");
MODULE_DESCRIPTION("GPIO Driver");

#define DEVICE_NAME "gpio_drv"
#define GPIO_NUM_PINS 8
#define GPIO_BASE 0x28000000UL

/* Register offset per pin */
static const unsigned int gpio_offsets[8] = {
    0x00, 0x04, 0x08, 0x0c, 0x10, 0x14, 0x1c, 0x20
};

/* Register bits and their purposes */
#define DATA_BIT      (1u << 0) 
#define DIRECT_BIT    (1u << 1)
#define INT_STATUS    (1u << 8)
#define INT_ENABLE    (1u << 9)

// ioremap-ed base address
static void __iomem *gpio_base = NULL;
#define GPIO_MAP_SIZE 0x100

static dev_t gpio_dev_num;              // Device number allocated from the kernel
static struct cdev gpio_cdev;           // Character device structure
static struct class *gpio_class = NULL; // Device class structure

// Single IRQ number sent by the GPIO hardware to the CPU
unsigned int irq_number = 70;

// Wait queue to put user processes to sleep/wake up for interrupts
static DECLARE_WAIT_QUEUE_HEAD(gpio_wait_queue); 

// Flag to indicate which pin caused the interrupt
static volatile u8 interrupt_pending_pins = 0; 

static int gpio_open(struct inode *inode, struct file *file);
static int gpio_release(struct inode *inode, struct file *file);
static long gpio_ioctl(struct file *file, unsigned int cmd, unsigned long arg);

static const struct file_operations gpio_fops = {
    .owner = THIS_MODULE,
    .open = gpio_open,             
    .release = gpio_release,       
    .unlocked_ioctl = gpio_ioctl,  
};

/* Returns the hardware register address for each pin */
static inline void __iomem *pin_reg(unsigned int pin)
{
    if (pin >= GPIO_NUM_PINS) return NULL; // Invalid pin number
    return gpio_base + gpio_offsets[pin]; // Returns the virtual address of the requested pin
}

static inline u32 read_pin_reg(unsigned int pin)
{
    // Get the register address of the pin
    void __iomem *r = pin_reg(pin);
    if (!r) return 0;
    
    // Read a 32-bit (4-byte) value from the ioremap-ed address
    return readl(r);
}

static inline void write_pin_reg(unsigned int pin, u32 val)
{
    void __iomem *r = pin_reg(pin);
    if (!r) return;
    writel(val, r);
}

/* Called when the user opens the /dev/gpio_drv file */
static int gpio_open(struct inode *inode, struct file *file)
{
    pr_info("%s: device opened\n", DEVICE_NAME);
    return 0;
}

/* Called when the user calls close() */
static int gpio_release(struct inode *inode, struct file *file)
{
    pr_info("%s: device closed\n", DEVICE_NAME);
    return 0;
}

// Validate the PIN number for error checking
static int is_valid_pin(unsigned char pin)
{
    if (pin >= GPIO_NUM_PINS) 
    {
        printk(KERN_ERR "Invalid pin: %d\n", pin);
        return -EINVAL; 
    }
    return 0;
}

/* Set Direction */
// 0 (Input) or 1 (Output)
static int set_direction(unsigned char pin, unsigned char direction)
{
    u32 reg_val;
    if (is_valid_pin(pin)) return -EINVAL;
    
    reg_val = read_pin_reg(pin);
    
    if (direction == 1) 
    { // 1 means Output 
        reg_val |= DIRECT_BIT;
        pr_info("Pin %d set as OUTPUT.\n", pin);
    } 
    else 
    { // 0 means Input 
        reg_val &= ~DIRECT_BIT;
        printk(KERN_INFO "Pin %d set as INPUT.\n", pin);
    }
    
    write_pin_reg(pin, reg_val);
    return 0;
}

/* Set Output Value */
// value: 0 (LOW) or 1 (HIGH)
static int set_output_value(unsigned char pin, unsigned char value)
{
    u32 reg_val;
    if (is_valid_pin(pin)) return -EINVAL;
    
    reg_val = read_pin_reg(pin);
    
    if (value == 1) 
    {
        reg_val |= DATA_BIT;
    } 
    else 
    {
        reg_val &= ~DATA_BIT;
    }
    
    write_pin_reg(pin, reg_val);
    printk(KERN_INFO "Pin %d data value set to %d.\n", pin, value);
    return 0;
}

/* Read Input Value */
static int get_input_value(unsigned char pin)
{
    u32 reg_val;
    if (is_valid_pin(pin)) return -EINVAL;
    
    reg_val = read_pin_reg(pin);
    
    // Check the direction bit (should only read if input) 
    if (reg_val & DIRECT_BIT) 
    { // Direction bit 1 means Output
        printk(KERN_WARNING "Pin %d is set as Output, input data cannot be read.\n", pin);
    }
    
    // Read and return the Data bit
    return (reg_val & DATA_BIT) ? 1 : 0;
}

/* Enable/Disable Interrupt */
// enable = 1 , disable = 0
static int set_interrupt_enable(unsigned char pin, unsigned char enable)
{
    u32 reg_val;
    if (is_valid_pin(pin)) return -EINVAL;
    
    reg_val = read_pin_reg(pin);
    
    if (enable == 1) 
    {
        reg_val |= INT_ENABLE;
        printk(KERN_INFO "Pin %d Interrupt ENABLED.\n", pin);
    } 
    else 
    {
        reg_val &= ~INT_ENABLE;
        printk(KERN_INFO "Pin %d Interrupt DISABLED.\n", pin);
    }
    
    write_pin_reg(pin, reg_val);
    return 0;
}

/* Read Interrupt Status */
static int read_interrupt_status(unsigned char pin)
{
    u32 reg_val;
    if (is_valid_pin(pin)) return -EINVAL;

    reg_val = read_pin_reg(pin);
    
    // If the Status bit is 1, an interrupt occurred
    return (reg_val & INT_STATUS) ? 1 : 0;
}

/* Clear Interrupt Status */
static int clear_interrupt_status(unsigned char pin)
{
    u32 reg_val;
    if (is_valid_pin(pin)) return -EINVAL;

    reg_val = read_pin_reg(pin);

    // Set the INT_STATUS bit to 1
    reg_val |= INT_STATUS;

    // Save the updated value
    write_pin_reg(pin, reg_val);

    pr_info("Pin %d Interrupt Status cleared.\n", pin);
    return 0;
}

/* Detect which of the 8 GPIO pins caused the interrupt and handle it */
static irqreturn_t gpio_isr(int irq, void *dev_id)
{
    int pin;
    u32 reg_val;
    u8 irq_detected = 0;

    // Check all pins to find which one caused the interrupt
    for (pin = 0; pin < GPIO_NUM_PINS; pin++)
    {
        reg_val = read_pin_reg(pin);

        if (reg_val & INT_STATUS) 
        {
            irq_detected = 1;

            // Mark the interrupt status globally
            interrupt_pending_pins |= (1 << pin);

            // Clear the interrupt status
            clear_interrupt_status(pin);

            printk(KERN_INFO "Interrupt detected on Pin %d. Status cleared.\n", pin);
        }
    }

    // Wake up user processes waiting in the queue
    if (irq_detected) 
    {
        wake_up_interruptible(&gpio_wait_queue);
        return IRQ_HANDLED;
    }
    
    return IRQ_NONE; 
}

/* Main handler for commands from the user */
static long gpio_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
    struct gpio_data data;
    long answer = 0;
    int value;

    if (_IOC_TYPE(cmd) != GPIO_IOC_MAGIC) return -ENOTTY;
    if (_IOC_NR(cmd) > GPIO_IOC_MAXNR) return -ENOTTY;

    // For Set and Clear commands, we need to get struct data from the user
    if ((cmd == SET_DIRECTION) || (cmd == WRITE_DATA) || 
        (cmd == SET_INT_ENABLE) || (cmd == CLEAR_INT_STATUS) || (cmd == WAIT_FOR_INTERRUPT)) 
    {
        if (copy_from_user(&data, (void __user *)arg, sizeof(data))) 
        {
            printk(KERN_ERR "%s: copy_from_user failed\n", DEVICE_NAME);
            return -EFAULT;
        }
    }

    switch (cmd) 
    {
        case SET_DIRECTION:
            answer = set_direction(data.pin, data.value);
            break;

        case WRITE_DATA:
            answer = set_output_value(data.pin, data.value);
            break;

        case READ_DATA:
            if (copy_from_user(&data, (void __user *)arg, sizeof(data))) return -EFAULT;
            value = get_input_value(data.pin);

            // Write the read value back to the user space
            if (copy_to_user((void __user *)arg, &value, sizeof(value))) 
            {
                 return -EFAULT;
            }
            break;

        case SET_INT_ENABLE:
            answer = set_interrupt_enable(data.pin, data.value);
            break;

        case READ_INT_STATUS:
            if (copy_from_user(&data, (void __user *)arg, sizeof(data))) return -EFAULT;
            value = read_interrupt_status(data.pin);

            // Write the read value back to the user space
            if (copy_to_user((void __user *)arg, &value, sizeof(value))) 
            {
                 return -EFAULT;
            }
            break;

        case CLEAR_INT_STATUS:
            answer = clear_interrupt_status(data.pin);
            break;
            
        case WAIT_FOR_INTERRUPT:
        {
            unsigned char pin_to_wait_for = data.pin;

            if (is_valid_pin(pin_to_wait_for)) return -EINVAL;

            printk(KERN_INFO "Waiting for interrupt on Pin %d...\n", pin_to_wait_for);

            // Put the process to sleep until the relevant bit in interrupt_pending_pins is set to 1
            // or a signal is received
            answer = wait_event_interruptible(gpio_wait_queue, (interrupt_pending_pins & (1 << pin_to_wait_for)));

            if (answer == -ERESTARTSYS) 
            { 
                printk(KERN_INFO "Waiting for Pin %d interrupted by a signal.\n", pin_to_wait_for);
                return -ERESTARTSYS;
            }

            // If the interrupt occurred, clear the flag
            interrupt_pending_pins &= ~(1 << pin_to_wait_for);

            printk(KERN_INFO "Interrupt occurred and handled for Pin %d.\n", pin_to_wait_for);
            answer = 0;
            break;
        }

        default:
            return -ENOTTY;
    }
    
    return answer;
}

/* Module initialization function */
static int __init ModuleInit(void)
{
    int err = 0; 
    printk(KERN_INFO "%s: init called\n", DEVICE_NAME);

    /* Map GPIO registers to CPU address space */
    gpio_base = ioremap(GPIO_BASE, GPIO_MAP_SIZE);
    
    if (!gpio_base) 
    {
        printk(KERN_ERR "Error: ioremap failed.\n");
        return -ENOMEM;
    }
    
    /* Register the interrupt */
    err = request_irq(irq_number, gpio_isr, IRQF_SHARED, DEVICE_NAME, (void *)gpio_isr);
    if (err) 
    {
        pr_err("%s: request_irq (IRQ %d) failed. Error: %d\n", DEVICE_NAME, irq_number, err);
        goto fail_request_irq;
    }
    printk(KERN_INFO "IRQ %d successfully registered.\n", irq_number);
    
    /* Allocate device number */
    err = alloc_chrdev_region(&gpio_dev_num, 0, 1, DEVICE_NAME);

    if (err < 0) 
    {
        pr_err("%s: alloc_chrdev_region failed\n", DEVICE_NAME);
        goto fail_dev_num;
    }

    printk(KERN_INFO "%s: major=%d, minor=%d\n", DEVICE_NAME, MAJOR(gpio_dev_num), MINOR(gpio_dev_num));

    /* Initialize cdev and bind file_operations */
    cdev_init(&gpio_cdev, &gpio_fops);
    gpio_cdev.owner = THIS_MODULE;

    /* Add cdev to the kernel */
    err = cdev_add(&gpio_cdev, gpio_dev_num, 1);
    if (err < 0) 
    {
        printk(KERN_ERR "%s: cdev_add failed\n", DEVICE_NAME);
        goto fail_cdev_add;
    }

    /* Create device class */
    gpio_class = class_create(THIS_MODULE, DEVICE_NAME);
    if (IS_ERR(gpio_class)) 
    {
        printk(KERN_ERR "%s: class_create failed\n", DEVICE_NAME);
        err = PTR_ERR(gpio_class);
        goto fail_class_create;
    }

    /* Create device node (/dev/gpio_drv) */
    if (IS_ERR(device_create(gpio_class, NULL, gpio_dev_num, NULL, DEVICE_NAME)))
    {
        printk(KERN_ERR "%s: device_create failed\n", DEVICE_NAME);
        err = -EFAULT;
        goto fail_device_create;
    }
    
    return 0; // Success

    /* Cleanup in case of failure */
    fail_device_create:
        class_destroy(gpio_class);

    fail_class_create:
        cdev_del(&gpio_cdev);

    fail_cdev_add:
        unregister_chrdev_region(gpio_dev_num, 1);
        
    fail_dev_num:
        free_irq(irq_number, (void *)gpio_isr); 

    fail_request_irq:
        if (gpio_base) 
        {
            iounmap(gpio_base);
        }
        return err;
}

/* Module cleanup function */
static void __exit ModuleExit(void)
{
    printk(KERN_INFO "%s: exit called\n", DEVICE_NAME);

    /* Free the interrupt handler */
    free_irq(irq_number, (void *)gpio_isr);
    printk(KERN_INFO "IRQ %d released.\n", irq_number);

    /* Remove the device node */
    if (gpio_class) 
    {
        device_destroy(gpio_class, gpio_dev_num);
        class_destroy(gpio_class);
    }
    
    /* Delete cdev */
    cdev_del(&gpio_cdev);

    /* Release the device number */
    unregister_chrdev_region(gpio_dev_num, 1);

    if (gpio_base) 
    {
        iounmap(gpio_base); 
        gpio_base = NULL;
    }
}

module_init(ModuleInit);
module_exit(ModuleExit);
