#ifndef GPIO_IOCTRL_H
#define GPIO_IOCTRL_H

/*
 * IOCTL Commands
 * _IO(type, nr)         -> Command only
 * _IOR(type, nr, size)  -> Read from the kernel
 * _IOW(type, nr, size)  -> Write to the kernel
 */
 
struct gpio_data 
{
    unsigned char pin;     // GPIO pin number
    unsigned char value;   // Value to set or read
};

/* Magic Number */
#define GPIO_IOC_MAGIC 'a'

// Set Direction (Input: 0, Output: 1)
#define SET_DIRECTION _IOW(GPIO_IOC_MAGIC, 1, struct gpio_data) 

// Read Input Value: Reads the data bit of the pin
#define READ_DATA _IOR(GPIO_IOC_MAGIC, 2, struct gpio_data)

// Set Output Value: Sets the data bit of the pin
#define WRITE_DATA _IOW(GPIO_IOC_MAGIC, 3, struct gpio_data)

// Configure Interrupt: Enables/disables the interrupt (Enable: 1, Disable: 0)
#define SET_INT_ENABLE _IOW(GPIO_IOC_MAGIC, 4, struct gpio_data)

// Read Interrupt Status: Reads the interrupt status bit
#define READ_INT_STATUS _IOR(GPIO_IOC_MAGIC, 5, struct gpio_data)

// Clear Interrupt Status: Clears the interrupt status bit
#define CLEAR_INT_STATUS _IOW(GPIO_IOC_MAGIC, 6, struct gpio_data)

// Wait for Interrupt
#define WAIT_FOR_INTERRUPT _IOW(GPIO_IOC_MAGIC, 7, struct gpio_data)

// Maximum command number
#define GPIO_IOC_MAXNR 7

#endif
