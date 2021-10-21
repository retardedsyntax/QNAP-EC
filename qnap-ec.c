/*
 * Copyright (C) 2021 Stonyx
 * https://www.stonyx.com/
 *
 * This driver is free software. You can redistribute it and/or modify it under the terms of the
 * GNU General Public License Version 3 (or at your option any later version) as published by The
 * Free Software Foundation.
 *
 * This driver is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 * without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See
 * the GNU General Public License for more details.
 *
 * If you did not received a copy of the GNU General Public License along with this script see
 * http://www.gnu.org/copyleft/gpl.html or write to The Free Software Foundation, 675 Mass Ave,
 * Cambridge, MA 02139, USA.
 */

#include <linux/fs.h>
#include <linux/hwmon.h>
#include <linux/io.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include "qnap-ec-ioctl.h"

// Define the printk prefix
#undef pr_fmt
#define pr_fmt(fmt) "%s @ %s: " fmt, "qnap-ec", __FUNCTION__

// Specify module details
MODULE_DESCRIPTION("QNAP EC Driver");
MODULE_VERSION("1.0.0");
MODULE_AUTHOR("Stonyx - https://www.stonyx.com/");
MODULE_LICENSE("GPL");

// Define the devices structure
// Note: in order to use the platform_device_alloc function (see note in the qnap_ec_init 
//       function) we need to make the plat_device member a pointer and in order to use the
//       container_of macro in the qnap_ec_misc_dev_open and qnap_ec_misc_dev_ioctl functions
//       we need to make the misc_device member not a pointer
struct qnap_ec_devices {
  struct platform_device* plat_device;
  struct mutex misc_device_mutex;
  uint8_t open_misc_device;
  struct miscdevice misc_device;
};

// Define the I/O control data structure
struct qnap_ec_data {
  struct mutex mutex;
  struct qnap_ec_devices* devices;
  struct qnap_ec_ioctl_command ioctl_command;
  uint64_t fan_or_pwm_channel_checked_field;
  uint64_t fan_or_pwm_channel_valid_field;
  uint64_t temp_channel_checked_field;
  uint64_t temp_channel_valid_field;
};

// Declare functions
static int __init qnap_ec_init(void);
static int __init qnap_ec_find(void);
static int qnap_ec_probe(struct platform_device* platform_dev);
static umode_t qnap_ec_hwmon_is_visible(const void* data, enum hwmon_sensor_types type,
                                        u32 attribute, int channel);
static int qnap_ec_hwmon_read(struct device* dev, enum hwmon_sensor_types type, u32 attribute,
                              int channel, long* value);
static int qnap_ec_hwmon_write(struct device* dev, enum hwmon_sensor_types type, u32 attribute,
                               int channel, long value);
static int qnap_ec_is_fan_or_pwm_channel_valid(struct qnap_ec_data* data, int channel);
static int qnap_ec_is_temp_channel_valid(struct qnap_ec_data* data, int channel);
static int qnap_ec_call_helper(uint8_t log_error);
static int qnap_ec_misc_device_open(struct inode* inode, struct file* file);
static long int qnap_ec_misc_device_ioctl(struct file* file, unsigned int command,
                                          unsigned long argument);
static int qnap_ec_misc_device_release(struct inode* inode, struct file* file);
static void __exit qnap_ec_exit(void);

// Declare and/or define needed variables for overriding the detected device ID
static unsigned short qnap_ec_force_id;
module_param(qnap_ec_force_id, ushort, 0);
MODULE_PARM_DESC(qnap_ec_force_id, "Override the detected device ID");

// Specifiy the initialization and exit functions
module_init(qnap_ec_init);
module_exit(qnap_ec_exit);

// Declare the platform driver structure pointer
static struct platform_driver* qnap_ec_plat_driver;

// Declare the devices structure pointer
static struct qnap_ec_devices* qnap_ec_devices;

// Function called to initialize the driver
static int __init qnap_ec_init(void)
{
  // Declare needed variables
  int error;

  // Define static constant data consisting of the miscellaneous device file operations structure
  static const struct file_operations misc_device_file_ops = {
    .owner = THIS_MODULE,
    .open = &qnap_ec_misc_device_open,
    .unlocked_ioctl = &qnap_ec_misc_device_ioctl,
    .release = &qnap_ec_misc_device_release
  };

  // Check if we can't find the chip
  error = qnap_ec_find();
  if (error)
  {
    return error;
  }

  // Allocate memory for the platform driver structure and populate various fields
  qnap_ec_plat_driver = kzalloc(sizeof(struct platform_driver), GFP_KERNEL);
  if (qnap_ec_plat_driver == NULL)
  {
    return -ENOMEM;
  }
  qnap_ec_plat_driver->driver.owner = THIS_MODULE;
  qnap_ec_plat_driver->driver.name = "qnap-ec";
  qnap_ec_plat_driver->probe = &qnap_ec_probe;

  // Register the driver
  error = platform_driver_register(qnap_ec_plat_driver);
  if (error)
  {
    // Free the platform driver structure memory
    kfree(qnap_ec_plat_driver);

    return error;
  }

  // Allocate memory for the devices structure
  qnap_ec_devices = kzalloc(sizeof(struct qnap_ec_devices), GFP_KERNEL);
  if (qnap_ec_devices == NULL)
  {
    // Unregister the driver
    platform_driver_unregister(qnap_ec_plat_driver);

    // Free the platform driver structure memory
    kfree(qnap_ec_plat_driver);

    return -ENOMEM;
  }

  // Allocate memory for the platform device structure and populate various fields
  // Note: we are using the platform_device_alloc function in combination with the
  //       platform_device_add function instead of the platform_device_register function because
  //       this approach is recommended for legacy type drivers that use hardware probing, all
  //       other hwmon drivers use this approach, and it provides a device release function for us
  qnap_ec_devices->plat_device = platform_device_alloc("qnap-ec", 0);
  if (qnap_ec_devices->plat_device == NULL)
  {
    // Free the devices structure memory
    kfree(qnap_ec_devices);

    // Unregister the driver
    platform_driver_unregister(qnap_ec_plat_driver);

    // Free the platform driver structure memory
    kfree(qnap_ec_plat_driver);

    return -ENOMEM;
  }
  qnap_ec_devices->plat_device->name = "qnap-ec";
  qnap_ec_devices->plat_device->id = PLATFORM_DEVID_NONE;

  // "Register" the platform device
  error = platform_device_add(qnap_ec_devices->plat_device);
  if (error)
  {
    // Free the platform device structure memory
    platform_device_put(qnap_ec_devices->plat_device);

    // Free the devices structure memory
    kfree(qnap_ec_devices);

    // Unregister the platform driver
    platform_driver_unregister(qnap_ec_plat_driver);

    // Free the platform driver structure memory
    kfree(qnap_ec_plat_driver);

    return error;
  }

  // Initialize the miscellaneous device mutex
  mutex_init(&qnap_ec_devices->misc_device_mutex);

  // Populate various miscellaneous device structure fields
  qnap_ec_devices->misc_device.name = "qnap-ec";
  qnap_ec_devices->misc_device.minor = MISC_DYNAMIC_MINOR;
  qnap_ec_devices->misc_device.fops = &misc_device_file_ops;

  // Register the miscellaneous device
  error = misc_register(&qnap_ec_devices->misc_device);
  if (error)
  {
    // "Unregister" the platform device and free the platform device structure memory
    platform_device_put(qnap_ec_devices->plat_device);

    // Free the devices structure memory
    kfree(qnap_ec_devices);

    // Unregister the platform driver
    platform_driver_unregister(qnap_ec_plat_driver);

    // Free the platform driver structure memory
    kfree(qnap_ec_plat_driver);

    return error;
  }

  return 0;
}

// Function called to find the QNAP embedded controller
static int __init qnap_ec_find(void)
{
  // Declare needed variables
  uint8_t byte1;
  uint8_t byte2;

  // Request access to the input (0x2E) and output (0x2F) ports
  if (request_muxed_region(0x2E, 2, "qnap-ec") == NULL)
  {
    return -EBUSY;
  }

  // Write 0x20 to the input port
  outb(0x20, 0x2E);

  // Read the first identification byte from the output port
  byte1 = inb(0x2F);

  // Write 0x21 to the input port
  outb(0x21, 0x2E);

  // Read the second identification byte from the output port
  byte2 = inb(0x2F);

  // Check if the identification bytes do not match the expected values
  if (byte1 != 0x85 || byte2 != 0x28)
  {
    release_region(0x2E, 2);
    return -ENODEV;
  }

  // Release access to the input and output ports
  release_region(0x2E, 2);

  return 0;
}

// Function called to probe this driver
static int qnap_ec_probe(struct platform_device* platform_dev)
{
  // Declare needed variables
  struct qnap_ec_data* data;
  struct device* device;

  // Define static constant data consisiting of mulitple configuration arrays, multiple hwmon
  //   channel info structures, the hwmon channel info structures array, and the hwmon chip
  //   information structure
  // Note: the number of entries in the configuration arrays need to match the number of supported
  //       channels in the fan_or_pwm_channel_checked/valid_field and
  //       temp_channel_checked/valid_field bitfield members in the qnap_ec_data structure which
  //       is based on the switch statements in the ec_sys_get_fan_status, ec_sys_get_fan_speed,
  //       ec_sys_get_fan_pwm, and ec_sys_get_temperature functions in the libuLinux_hal.so library
  //       as decompiled by IDA and rounded up to the nearest multiple of 32 to allow for future
  //       additions of channels in those functions
  static const u32 fan_config[] = { HWMON_F_INPUT, HWMON_F_INPUT, HWMON_F_INPUT, HWMON_F_INPUT,
    HWMON_F_INPUT, HWMON_F_INPUT, HWMON_F_INPUT, HWMON_F_INPUT, HWMON_F_INPUT, HWMON_F_INPUT,
    HWMON_F_INPUT, HWMON_F_INPUT, HWMON_F_INPUT, HWMON_F_INPUT, HWMON_F_INPUT, HWMON_F_INPUT,
    HWMON_F_INPUT, HWMON_F_INPUT, HWMON_F_INPUT, HWMON_F_INPUT, HWMON_F_INPUT, HWMON_F_INPUT,
    HWMON_F_INPUT, HWMON_F_INPUT, HWMON_F_INPUT, HWMON_F_INPUT, HWMON_F_INPUT, HWMON_F_INPUT,
    HWMON_F_INPUT, HWMON_F_INPUT, HWMON_F_INPUT, HWMON_F_INPUT, HWMON_F_INPUT, HWMON_F_INPUT,
    HWMON_F_INPUT, HWMON_F_INPUT, HWMON_F_INPUT, HWMON_F_INPUT, HWMON_F_INPUT, HWMON_F_INPUT,
    HWMON_F_INPUT, HWMON_F_INPUT, HWMON_F_INPUT, HWMON_F_INPUT, HWMON_F_INPUT, HWMON_F_INPUT,
    HWMON_F_INPUT, HWMON_F_INPUT, HWMON_F_INPUT, HWMON_F_INPUT, HWMON_F_INPUT, HWMON_F_INPUT,
    HWMON_F_INPUT, HWMON_F_INPUT, HWMON_F_INPUT, HWMON_F_INPUT, HWMON_F_INPUT, HWMON_F_INPUT,
    HWMON_F_INPUT, HWMON_F_INPUT, HWMON_F_INPUT, HWMON_F_INPUT, HWMON_F_INPUT, HWMON_F_INPUT, 0 };
  static const u32 pwm_config[] = { HWMON_PWM_INPUT, HWMON_PWM_INPUT, HWMON_PWM_INPUT,
    HWMON_PWM_INPUT, HWMON_PWM_INPUT, HWMON_PWM_INPUT, HWMON_PWM_INPUT, HWMON_PWM_INPUT, 
    HWMON_PWM_INPUT, HWMON_PWM_INPUT, HWMON_PWM_INPUT, HWMON_PWM_INPUT, HWMON_PWM_INPUT, 
    HWMON_PWM_INPUT, HWMON_PWM_INPUT, HWMON_PWM_INPUT, HWMON_PWM_INPUT, HWMON_PWM_INPUT, 
    HWMON_PWM_INPUT, HWMON_PWM_INPUT, HWMON_PWM_INPUT, HWMON_PWM_INPUT, HWMON_PWM_INPUT, 
    HWMON_PWM_INPUT, HWMON_PWM_INPUT, HWMON_PWM_INPUT, HWMON_PWM_INPUT, HWMON_PWM_INPUT, 
    HWMON_PWM_INPUT, HWMON_PWM_INPUT, HWMON_PWM_INPUT, HWMON_PWM_INPUT, HWMON_PWM_INPUT, 
    HWMON_PWM_INPUT, HWMON_PWM_INPUT, HWMON_PWM_INPUT, HWMON_PWM_INPUT, HWMON_PWM_INPUT, 
    HWMON_PWM_INPUT, HWMON_PWM_INPUT, HWMON_PWM_INPUT, HWMON_PWM_INPUT, HWMON_PWM_INPUT, 
    HWMON_PWM_INPUT, HWMON_PWM_INPUT, HWMON_PWM_INPUT, HWMON_PWM_INPUT, HWMON_PWM_INPUT, 
    HWMON_PWM_INPUT, HWMON_PWM_INPUT, HWMON_PWM_INPUT, HWMON_PWM_INPUT, HWMON_PWM_INPUT, 
    HWMON_PWM_INPUT, HWMON_PWM_INPUT, HWMON_PWM_INPUT, HWMON_PWM_INPUT, HWMON_PWM_INPUT, 
    HWMON_PWM_INPUT, HWMON_PWM_INPUT, HWMON_PWM_INPUT, HWMON_PWM_INPUT, HWMON_PWM_INPUT, 
    HWMON_PWM_INPUT, 0 };
  static const u32 temp_config[] = { HWMON_T_INPUT, HWMON_T_INPUT, HWMON_T_INPUT, HWMON_T_INPUT,
    HWMON_T_INPUT, HWMON_T_INPUT, HWMON_T_INPUT, HWMON_T_INPUT, HWMON_T_INPUT, HWMON_T_INPUT,
    HWMON_T_INPUT, HWMON_T_INPUT, HWMON_T_INPUT, HWMON_T_INPUT, HWMON_T_INPUT, HWMON_T_INPUT,
    HWMON_T_INPUT, HWMON_T_INPUT, HWMON_T_INPUT, HWMON_T_INPUT, HWMON_T_INPUT, HWMON_T_INPUT,
    HWMON_T_INPUT, HWMON_T_INPUT, HWMON_T_INPUT, HWMON_T_INPUT, HWMON_T_INPUT, HWMON_T_INPUT,
    HWMON_T_INPUT, HWMON_T_INPUT, HWMON_T_INPUT, HWMON_T_INPUT, HWMON_T_INPUT, HWMON_T_INPUT,
    HWMON_T_INPUT, HWMON_T_INPUT, HWMON_T_INPUT, HWMON_T_INPUT, HWMON_T_INPUT, HWMON_T_INPUT,
    HWMON_T_INPUT, HWMON_T_INPUT, HWMON_T_INPUT, HWMON_T_INPUT, HWMON_T_INPUT, HWMON_T_INPUT,
    HWMON_T_INPUT, HWMON_T_INPUT, HWMON_T_INPUT, HWMON_T_INPUT, HWMON_T_INPUT, HWMON_T_INPUT,
    HWMON_T_INPUT, HWMON_T_INPUT, HWMON_T_INPUT, HWMON_T_INPUT, HWMON_T_INPUT, HWMON_T_INPUT,
    HWMON_T_INPUT, HWMON_T_INPUT, HWMON_T_INPUT, HWMON_T_INPUT, HWMON_T_INPUT, HWMON_T_INPUT, 0 };
  static const struct hwmon_channel_info fan_channel_info = {
    .type = hwmon_fan,
    .config = fan_config
  };
  static const struct hwmon_channel_info pwm_channel_info = {
    .type = hwmon_pwm,
    .config = pwm_config
  };
  static const struct hwmon_channel_info temp_channel_info = {
    .type = hwmon_temp,
    .config = temp_config
  };
  static const struct hwmon_channel_info* hwmon_channel_info[] = { &fan_channel_info, 
    &pwm_channel_info, &temp_channel_info, NULL };
  static const struct hwmon_ops hwmon_ops = {
    .is_visible = &qnap_ec_hwmon_is_visible,
    .read = &qnap_ec_hwmon_read,
    .write = &qnap_ec_hwmon_write
  };
  static const struct hwmon_chip_info hwmon_chip_info = {
    .info = hwmon_channel_info,
    .ops = &hwmon_ops
  };

  // Allocate device managed memory for the data structure
  data = devm_kzalloc(&platform_dev->dev, sizeof(struct qnap_ec_data), GFP_KERNEL);
  if (data == NULL)
  {
    return -ENOMEM;
  }

  // Initialize the data mutex and set the devices pointer
  mutex_init(&data->mutex);
  data->devices = qnap_ec_devices;

  // Register the hwmon device and include the data structure
  // Note: hwmon device name cannot contain dashes
  device = devm_hwmon_device_register_with_info(&platform_dev->dev, "qnap_ec", data,
    &hwmon_chip_info, NULL);
  if (device == NULL)
  {
    return -ENOMEM;
  }

  // Set the custom device data to the data structure
  dev_set_drvdata(&platform_dev->dev, data);

  return 0;
}

// Function called to check if a hwmon attribute is visible
static umode_t qnap_ec_hwmon_is_visible(const void* data, enum hwmon_sensor_types type,
                                        u32 attribute, int channel)
{
  // Switch based on the sensor type
  // Note: we are using a switch statement to simplify possible future expansion
  switch (type)
  {
    case hwmon_fan:
      // Switch based on the sensor attribute
      switch (attribute)
      {
        case hwmon_fan_input:
          // Check if this channel is valid
          if (qnap_ec_is_fan_or_pwm_channel_valid((struct qnap_ec_data*)data, channel) == 1)
          {
            // Make the fan input read only
            return S_IRUGO;
          }

          return 0;
        default:
          return 0;
      }
    case hwmon_pwm:
      // Switch based on the sensor attribute
      switch (attribute)
      {
        case hwmon_pwm_input:
          // Check if this channel is valid
          if (qnap_ec_is_fan_or_pwm_channel_valid((struct qnap_ec_data*)data, channel) == 1)
          {
            // Make the PWM input read/write
            return S_IRUGO | S_IWUSR;
          }

          return 0;
        default:
          return 0;
      }
    case hwmon_temp:
      // Switch based on the sensor attribute
      switch (attribute)
      {
        case hwmon_temp_input:
          // Check if this channel is valid
          if (qnap_ec_is_temp_channel_valid((struct qnap_ec_data*)data, channel) == 1)
          {
            // Make the temperature input read only
            return S_IRUGO;
          }

          return 0;
        default:
          return 0;
      }
    default:
      return 0;
  }
}

// Function called to read from a hwmon attribute
static int qnap_ec_hwmon_read(struct device* device, enum hwmon_sensor_types type, u32 attribute,
                              int channel, long* value)
{
  // Declare and/or define needed variables
  struct qnap_ec_data* data = dev_get_drvdata(device);

  // Switch based on the sensor type
  // Note: we are using a switch statement to simplify possible future expansion
  switch (type)
  {
    case hwmon_fan:
      // Switch based on the sensor attribute
      switch (attribute)
      {
        case hwmon_fan_input:
          // Check if this channel is invalid
          if (qnap_ec_is_fan_or_pwm_channel_valid(data, channel) != 1)
          {
            return -EOPNOTSUPP;
          }

          // Get the data mutex lock
          mutex_lock(&data->mutex);

          // Set the I/O control command structure fields for calling the ec_sys_get_fan_speed
          //   function in the libuLinux_hal library the helper program
          // Note: "sizeof(((struct qnap_ec_ioctl_command*)0)->function_name)" statement is based
          //       on the FIELD_SIZEOF macro which was removed from the kernel
          data->ioctl_command.function_type = int8_func_uint8_uint32pointer;
          strncpy(data->ioctl_command.function_name, "ec_sys_get_fan_speed",
            sizeof(((struct qnap_ec_ioctl_command*)0)->function_name) - 1);
          data->ioctl_command.argument1_uint8 = channel;
          data->ioctl_command.argument2_uint32 = 0;

          break;
        default:
          return -EOPNOTSUPP;
      }
      break;
    case hwmon_pwm:
      // Switch based on the sensor attribute
      switch (attribute)
      {
        case hwmon_pwm_input:
          // Check if this channel is invalid
          if (qnap_ec_is_fan_or_pwm_channel_valid(data, channel) != 1)
          {
            return -EOPNOTSUPP;
          }

          // Get the data mutex lock
          mutex_lock(&data->mutex);

          // Set the I/O control command structure fields for calling the ec_sys_get_fan_pwm
          //   function in the libuLinux_hal library the helper program
          // Note: "sizeof(((struct qnap_ec_ioctl_command*)0)->function_name)" statement is based
          //       on the FIELD_SIZEOF macro which was removed from the kernel
          data->ioctl_command.function_type = int8_func_uint8_uint32pointer;
          strncpy(data->ioctl_command.function_name, "ec_sys_get_fan_pwm",
            sizeof(((struct qnap_ec_ioctl_command*)0)->function_name) - 1);
          data->ioctl_command.argument1_uint8 = channel;
          data->ioctl_command.argument2_uint32 = 0;

          break;
        default:
          return -EOPNOTSUPP;
      }
      break;
    case hwmon_temp:
      // Switch based on the sensor attribute
      switch (attribute)
      {
        case hwmon_temp_input:
          // Check if this channel is invalid
          if (qnap_ec_is_temp_channel_valid(data, channel) != 1)
          {
            return -EOPNOTSUPP;
          }

          // Get the data mutex lock
          mutex_lock(&data->mutex);

          // Set the I/O control command structure fields for calling the ec_sys_get_temperature
          //   function in the libuLinux_hal library via the helper program
          // Note: "sizeof(((struct qnap_ec_ioctl_command*)0)->function_name)" statement is based
          //       on the FIELD_SIZEOF macro which was removed from the kernel
          // Note: we are using an int64 field in place of a double field since floating point
          //       math is not possible in kernel space
          data->ioctl_command.function_type = int8_func_uint8_doublepointer;
          strncpy(data->ioctl_command.function_name, "ec_sys_get_temperature",
            sizeof(((struct qnap_ec_ioctl_command*)0)->function_name) - 1);
          data->ioctl_command.argument1_uint8 = channel;
          data->ioctl_command.argument2_int64 = 0;

          break;
        default:
          return -EOPNOTSUPP;
      }
      break;
    default:
      return -EOPNOTSUPP;
  }

  // Set the open device flag to allow return communication by the helper program
  data->devices->open_misc_device = 1;

  // Call the helper program
  if (qnap_ec_call_helper(1) != 0)
  {
    // Clear the open device flag
    data->devices->open_misc_device = 0;

    // Release the data mutex lock
    mutex_unlock(&data->mutex);

    return -ENODATA;
  }

  // Clear the open device flag
  data->devices->open_misc_device = 0;

  // Check if the called function returned any errors
  if (data->ioctl_command.return_value_int8 != 0)
  {
    // Log the error
    printk(KERN_ERR "libuLinux_hal library %s function called by qnap-ec helper program returned "
      "a non zero value (%i)", data->ioctl_command.function_name,
      data->ioctl_command.return_value_int8);

    // Release the data mutex lock
    mutex_unlock(&data->mutex);

    return -ENODATA;
  }

  // Switch based on the sensor type
  // Note: we are using a switch statement to match the switch statement above with the exception
  //       of error cases
  switch (type)
  {
    case hwmon_fan:
      // Switch based on the sensor attribute
      switch (attribute)
      {
        case hwmon_fan_input:
          // Set the value to the returned fan speed value
          *value = data->ioctl_command.argument2_uint32;

          break;
      }
      break;
    case hwmon_pwm:
      // Switch based on the sensor attribute
      switch (attribute)
      {
        case hwmon_pwm_input:
          // Set the value to the returned fan PWM value
          *value = data->ioctl_command.argument2_uint32;

          break;
      }
      break;
    case hwmon_temp:
      // Switch based on the sensor attribute
      switch (attribute)
      {
        case hwmon_temp_input:
          // Set the value to the returned temperature value
          // Note: we are using an int64 field instead of a double field because floating point
          //       math is not possible in kernel space and because an int64 value can hold a 19
          //       digit integer while a double value can hold a 16 digit integer without loosing
          //       precision we can multiple the double value by 1000 to move three digits after
          //       the decimal point to before the decimal point and still fit the value in an
          //       int64 value and preserve three digits after the decimal point however because
          //       we need to return a millidegree value there is no need to divide by 1000
          *value = data->ioctl_command.argument2_int64;

          break;
      }
      break;
    // Dummy default cause to silence compiler warnings about not including all enums in switch
    //   statement
    default:
      break;
  }

  // Release the data mutex lock
  mutex_unlock(&data->mutex);

  return 0;
}

// Function called to write to a hwmon atrribute
static int qnap_ec_hwmon_write(struct device* device, enum hwmon_sensor_types type, u32 attribute,
                               int channel, long value)
{
  // Declare and/or define needed variables
  struct qnap_ec_data* data = dev_get_drvdata(device);

  // Switch based on the sensor type
  // Note: we are using a switch statement to simplify possible future expansion
  switch (type)
  {
    case hwmon_pwm:
      // Switch based on the sensor attribute
      switch (attribute)
      {
        case hwmon_pwm_input:
          // Check if this channel is invalid
          if (qnap_ec_is_fan_or_pwm_channel_valid(data, channel) != 1)
          {
            return -EOPNOTSUPP;
          }

          // Get the data mutex lock
          mutex_lock(&data->mutex);

          // Set the I/O control command structure fields for calling the ec_sys_set_fan_speed
          //   function in the libuLinux_hal library via the helper program
          // Note: "sizeof(((struct qnap_ec_ioctl_command*)0)->function_name)" statement is based
          //       on the FIELD_SIZEOF macro which was removed from the kernel
          data->ioctl_command.function_type = int8_func_uint8_uint8;
          strncpy(data->ioctl_command.function_name, "ec_sys_set_fan_speed",
            sizeof(((struct qnap_ec_ioctl_command*)0)->function_name) - 1);
          data->ioctl_command.argument1_uint8 = channel;
          data->ioctl_command.argument2_uint8 = value;

          break;
        default:
          return -EOPNOTSUPP;
      }
      break;
    default:
      return -EOPNOTSUPP;
  }

  // Set the open device flag to allow return communication by the helper program
  data->devices->open_misc_device = 1;

  // Call the helper program
  if (qnap_ec_call_helper(1) != 0)
  {
    // Clear the open device flag
    data->devices->open_misc_device = 0;

    // Release the data mutex lock
    mutex_unlock(&data->mutex);

    // Note: the "bad exchange" error code seems to be the closest code to describe this error
    //       condition (ie: the exchange of information between the kernel module and the helper
    //       program went bad)
    return -EBADE;
  }

  // Clear the open device flag
  data->devices->open_misc_device = 0;

  // Check if the called function returned any errors
  if (data->ioctl_command.return_value_int8 != 0)
  {
    // Log the error
    printk(KERN_ERR "libuLinux_hal library %s function called by qnap-ec helper program returned "
      "a non zero value (%i)", data->ioctl_command.function_name,
      data->ioctl_command.return_value_int8);

    // Release the data mutex lock
    mutex_unlock(&data->mutex);

    // Note: the "bad exchange" error code seems to be the closest code to describe this error
    //       condition (ie: the exchange of information between the kernel module and the helper
    //       program went bad)
    return -EBADE;
  }

  // Release the data mutex lock
  mutex_unlock(&data->mutex);

  return 0;
}

// Function called to check if the fan or PWM channel number is valid
// Note: the return value is 1 if the channel is valid, 0 if the channel is invalid, and a
//       negative value if an error occurred
static int qnap_ec_is_fan_or_pwm_channel_valid(struct qnap_ec_data* data, int channel)
{
  // Note: based on testing the logic for determining if a fan or PWM channel is valid is:
  //       - if channel is 10 or 11 then channel is invalid
  //       - call ec_sys_get_fan_status function in the libuLinux_hal library
  //       - if the function return value is non zero then the channel is invalid
  //       - if the returned fan status value is non zero then the channel is invalid
  //       - call ec_sys_get_fan_speed function in the libuLinux_hal library
  //       - if the function return value is non zero then the channel is invalid
  //       - if the returned fan speed value is 65535 then the channel is invalid
  //       - call ec_sys_get_fan_pwm function in the libuLinux_hal library
  //       - if the function return value is non zero then the channel is invalid
  //       - if the returned fan PWM value is 650 then the channel is invalid

  // Check if this channel has already been checked
  if (((data->fan_or_pwm_channel_checked_field >> channel) & 0x01) == 1)
  {
    return (data->fan_or_pwm_channel_valid_field >> channel) & 0x01;
  }

  // Get the data mutex lock
  mutex_lock(&data->mutex);

  // Check if the channel number is 10 or 11 which are only valid for units with redundant power
  //   supplies and cause the libuLinux_hal library functions to fail to execute when retrieving
  //   data for these channels
  if (channel == 10 || channel == 11)
  {
    // Mark this channel as checked and invalid
    data->fan_or_pwm_channel_checked_field |= ((uint64_t)0x01 << channel);
    data->fan_or_pwm_channel_valid_field &= ~((uint64_t)0x01 << channel);

    // Release the data mutex lock
    mutex_unlock(&data->mutex);

    return 0;
  }

  // Set the I/O control command structure fields for calling the ec_sys_get_fan_status function
  //   in the libuLinux_hal library via the helper program
  // Note: "sizeof(((struct qnap_ec_ioctl_command*)0)->function_name)" statement is based on the
  //       FIELD_SIZEOF macro which was removed from the kernel
  data->ioctl_command.function_type = int8_func_uint8_uint32pointer;
  strncpy(data->ioctl_command.function_name, "ec_sys_get_fan_status",
    sizeof(((struct qnap_ec_ioctl_command*)0)->function_name) - 1);
  data->ioctl_command.argument1_uint8 = channel;
  data->ioctl_command.argument2_uint32 = 0;

  // Set the open device flag to allow return communication by the helper program
  data->devices->open_misc_device = 1;

  // Call the helper program
  if (qnap_ec_call_helper(0) != 0)
  {
    // Clear the open device flag
    data->devices->open_misc_device = 0;

    // Mark this channel as checked and invalid
    data->fan_or_pwm_channel_checked_field |= ((uint64_t)0x01 << channel);
    data->fan_or_pwm_channel_valid_field &= ~((uint64_t)0x01 << channel);

    // Release the data mutex lock
    mutex_unlock(&data->mutex);

    return -ENODATA;
  }

  // Clear the open device flag
  data->devices->open_misc_device = 0;

  // Check if the called function return value is non zero
  if (data->ioctl_command.return_value_int8 != 0)
  {
    // Mark this channel as checked and invalid
    data->fan_or_pwm_channel_checked_field |= ((uint64_t)0x01 << channel);
    data->fan_or_pwm_channel_valid_field &= ~((uint64_t)0x01 << channel);

    // Release the data mutex lock
    mutex_unlock(&data->mutex);

    return 0;
  }

  // Check if the returned status value is non zero
  if (data->ioctl_command.argument2_uint32 != 0)
  {
    // Mark this channel as checked and invalid
    data->fan_or_pwm_channel_checked_field |= ((uint64_t)0x01 << channel);
    data->fan_or_pwm_channel_valid_field &= ~((uint64_t)0x01 << channel);

    // Release the data mutex lock
    mutex_unlock(&data->mutex);

    return 0;
  }

  // Set the I/O control command structure fields for calling the ec_sys_get_fan_speed function
  //   in the libuLinux_hal library via the helper program
  // Note: "sizeof(((struct qnap_ec_ioctl_command*)0)->function_name)" statement is based on the
  //       FIELD_SIZEOF macro which was removed from the kernel
  data->ioctl_command.function_type = int8_func_uint8_uint32pointer;
  strncpy(data->ioctl_command.function_name, "ec_sys_get_fan_speed",
    sizeof(((struct qnap_ec_ioctl_command*)0)->function_name) - 1);
  data->ioctl_command.argument1_uint8 = channel;
  data->ioctl_command.argument2_uint32 = 0;

  // Set the open device flag to allow return communication by the helper program
  data->devices->open_misc_device = 1;

  // Call the helper program
  if (qnap_ec_call_helper(0) != 0)
  {
    // Clear the open device flag
    data->devices->open_misc_device = 0;

    // Mark this channel as checked and invalid
    data->fan_or_pwm_channel_checked_field |= ((uint64_t)0x01 << channel);
    data->fan_or_pwm_channel_valid_field &= ~((uint64_t)0x01 << channel);

    // Release the data mutex lock
    mutex_unlock(&data->mutex);

    return -ENODATA;
  }

  // Clear the open device flag
  data->devices->open_misc_device = 0;

  // Check if the called function return value is non zero
  if (data->ioctl_command.return_value_int8 != 0)
  {
    // Mark this channel as checked and invalid
    data->fan_or_pwm_channel_checked_field |= ((uint64_t)0x01 << channel);
    data->fan_or_pwm_channel_valid_field &= ~((uint64_t)0x01 << channel);

    // Release the data mutex lock
    mutex_unlock(&data->mutex);

    return 0;
  }

  // Check if the returned fan speed value is 65535
  if (data->ioctl_command.argument2_uint32 == 65535)
  {
    // Mark this channel as checked and invalid
    data->fan_or_pwm_channel_checked_field |= ((uint64_t)0x01 << channel);
    data->fan_or_pwm_channel_valid_field &= ~((uint64_t)0x01 << channel);

    // Release the data mutex lock
    mutex_unlock(&data->mutex);

    return 0;
  }

  // Set the I/O control command structure fields for calling the ec_sys_get_fan_pwm function
  //   in the libuLinux_hal library via the helper program
  // Note: "sizeof(((struct qnap_ec_ioctl_command*)0)->function_name)" statement is based on the
  //       FIELD_SIZEOF macro which was removed from the kernel
  data->ioctl_command.function_type = int8_func_uint8_uint32pointer;
  strncpy(data->ioctl_command.function_name, "ec_sys_get_fan_pwm",
    sizeof(((struct qnap_ec_ioctl_command*)0)->function_name) - 1);
  data->ioctl_command.argument1_uint8 = channel;
  data->ioctl_command.argument2_uint32 = 0;

  // Set the open device flag to allow return communication by the helper program
  data->devices->open_misc_device = 1;

  // Call the helper program
  if (qnap_ec_call_helper(0) != 0)
  {
    // Clear the open device flag
    data->devices->open_misc_device = 0;

    // Mark this channel as checked and invalid
    data->fan_or_pwm_channel_checked_field |= ((uint64_t)0x01 << channel);
    data->fan_or_pwm_channel_valid_field &= ~((uint64_t)0x01 << channel);

    // Release the data mutex lock
    mutex_unlock(&data->mutex);

    return -ENODATA;
  }

  // Clear the open device flag
  data->devices->open_misc_device = 0;

  // Check if the called function return value is non zero
  if (data->ioctl_command.return_value_int8 != 0)
  {
    // Mark this channel as checked and invalid
    data->fan_or_pwm_channel_checked_field |= ((uint64_t)0x01 << channel);
    data->fan_or_pwm_channel_valid_field &= ~((uint64_t)0x01 << channel);

    // Release the data mutex lock
    mutex_unlock(&data->mutex);

    return 0;
  }

  // Check if the returned fan PWM value is 650
  if (data->ioctl_command.argument2_uint32 == 650)
  {
    // Mark this channel as checked and invalid
    data->fan_or_pwm_channel_checked_field |= ((uint64_t)0x01 << channel);
    data->fan_or_pwm_channel_valid_field &= ~((uint64_t)0x01 << channel);

    // Release the data mutex lock
    mutex_unlock(&data->mutex);

    return 0;
  }

  // Mark this channel as checked and valid
  data->fan_or_pwm_channel_checked_field |= ((uint64_t)0x01 << channel);
  data->fan_or_pwm_channel_valid_field |= ((uint64_t)0x01 << channel);

  // Release the data mutex lock
  mutex_unlock(&data->mutex);

  return 1;
}

// Function called to check if the temperature channel number is valid
// Note: the return value is 1 if the channel is valid, 0 if the channel is invalid, and a
//       negative value if an error occurred
static int qnap_ec_is_temp_channel_valid(struct qnap_ec_data* data, int channel)
{
  // Note: based on testing the logic for determining if a temperature channel is valid is:
  //       - if channel is 10 or 11 then channel is invalid
  //       - call ec_sys_get_temperature function in the libuLinux_hal library
  //       - if the function return value is non zero then the channel is invalid
  //       - if the returned temperature value is negative then the channel is invalid

  // Check if this channel has already been checked
  if (((data->temp_channel_checked_field >> channel) & 0x01) == 1)
  {
    return (data->temp_channel_valid_field >> channel) & 0x01;
  }

  // Get the data mutex lock
  mutex_lock(&data->mutex);

  // Check if the channel number is 10 or 11 which are only valid for units with redundant power
  //   supplies and cause the libuLinux_hal library functions to fail to execute when retrieving
  //   data for these channels
  if (channel == 10 || channel == 11)
  {
    // Mark this channel as checked and invalid
    data->temp_channel_checked_field |= ((uint64_t)0x01 << channel);
    data->temp_channel_valid_field &= ~((uint64_t)0x01 << channel);

    // Release the data mutex lock
    mutex_unlock(&data->mutex);

    return 0;
  }

  // Set the I/O control command structure fields for calling the ec_sys_get_temperature function
  //   in the libuLinux_hal library via the helper program
  // Note: "sizeof(((struct qnap_ec_ioctl_command*)0)->function_name)" statement is based on the
  //       FIELD_SIZEOF macro which was removed from the kernel
  // Note: we are using an int64 field in place of a double field since floating point math is not
  //       possible in kernel space
  data->ioctl_command.function_type = int8_func_uint8_doublepointer;
  strncpy(data->ioctl_command.function_name, "ec_sys_get_temperature",
    sizeof(((struct qnap_ec_ioctl_command*)0)->function_name) - 1);
  data->ioctl_command.argument1_uint8 = channel;
  data->ioctl_command.argument2_int64 = 0;

  // Set the open device flag to allow return communication by the helper program
  data->devices->open_misc_device = 1;

  // Call the helper program
  if (qnap_ec_call_helper(0) != 0)
  {
    // Clear the open device flag
    data->devices->open_misc_device = 0;

    // Mark this channel as checked and invalid
    data->temp_channel_checked_field |= ((uint64_t)0x01 << channel);
    data->temp_channel_valid_field &= ~((uint64_t)0x01 << channel);

    // Release the data mutex lock
    mutex_unlock(&data->mutex);

    return -ENODATA;
  }

  // Clear the open device flag
  data->devices->open_misc_device = 0;

  // Check if the called function return value is non zero
  if (data->ioctl_command.return_value_int8 != 0)
  {
    // Mark this channel as checked and invalid
    data->temp_channel_checked_field |= ((uint64_t)0x01 << channel);
    data->temp_channel_valid_field &= ~((uint64_t)0x01 << channel);

    // Release the data mutex lock
    mutex_unlock(&data->mutex);

    return 0;
  }

  // Check if the returned temperature value is negative
  if (data->ioctl_command.argument2_int64 < 0)
  {
    // Mark this channel as checked and invalid
    data->temp_channel_checked_field |= ((uint64_t)0x01 << channel);
    data->temp_channel_valid_field &= ~((uint64_t)0x01 << channel);

    // Release the data mutex lock
    mutex_unlock(&data->mutex);

    return 0;
  }

  // Mark this channel as checked and as valid
  data->temp_channel_checked_field |= ((uint64_t)0x01 << channel);
  data->temp_channel_valid_field |= ((uint64_t)0x01 << channel);

  // Release the data mutex lock
  mutex_unlock(&data->mutex);

  return 1;
}

// Function called to call the user space helper program
// Note: the return value is the call_usermodehelper function's error code if an error code was
//       returned or if successful the return value is the user space helper program's error code
//       if an error code was returned or if successful the return value is zero
static int qnap_ec_call_helper(uint8_t log_error)
{
  // Declare and/or define needed variables
  int return_value;
  char* arguments[] = {
    "/usr/local/sbin/qnap-ec",
    NULL 
  };
  char* environment[] = {
    "PATH=/usr/local/sbin;/usr/local/bin;/usr/sbin;/usr/bin;/sbin;/bin",
    NULL
  };

  // Call the user space helper program and check if the first 8 bits of the return value
  //   contain any error codes
  return_value = call_usermodehelper(arguments[0], arguments, environment, UMH_WAIT_PROC);
  if ((return_value & 0xFF) != 0)
  {
    // Try calling the user space helper program in the fall back locations
    arguments[0] = "/usr/local/bin/qnap-ec";
    return_value = call_usermodehelper(arguments[0], arguments, environment, UMH_WAIT_PROC);
    if ((return_value & 0xFF) != 0)
    {
      arguments[0] = "/usr/sbin/qnap-ec";
      return_value = call_usermodehelper(arguments[0], arguments, environment, UMH_WAIT_PROC);
      if ((return_value & 0xFF) != 0)
      {
        arguments[0] = "/usr/bin/qnap-ec";
        return_value = call_usermodehelper(arguments[0], arguments, environment, UMH_WAIT_PROC);
        if ((return_value & 0xFF) != 0)
        {
          arguments[0] = "/sbin/qnap-ec";
          return_value = call_usermodehelper(arguments[0], arguments, environment, UMH_WAIT_PROC);
          if ((return_value & 0xFF) != 0)
          {
            arguments[0] = "/bin/qnap-ec";
            return_value = call_usermodehelper(arguments[0], arguments, environment, UMH_WAIT_PROC);
            if ((return_value & 0xFF) != 0)
            {
              // Log the error
              printk(KERN_ERR "qnap-ec helper program not found in the expected path "
                "(/usr/local/sbin) or any of the fall back paths (/usr/local/bin;/usr/sbin;"
                "/usr/bin;/sbin;/bin)");

              // Return the call_usermodehelper function's error code
              return return_value & 0xFF;
            }    
          }    
        }    
      }
    }    
  }

  // Check if the user space helper program's return value stored in the second 8 bits of the
  //   return value contain any error codes
  if (((return_value >> 8) & 0xFF) != 0)
  {
    // Log the error
    if (log_error)
    {
      printk(KERN_ERR "qnap-ec helper program exited with a non zero exit code (%i)",
        ((return_value >> 8) & 0xFF));
    }

    // Return the user space helper program's error code
    return (return_value >> 8) & 0xFF;
  }
  
  return 0;
}

// Function called when the miscellaneous device is openeded
static int qnap_ec_misc_device_open(struct inode* inode, struct file* file)
{
  // Declare and/or define needed variables
  struct qnap_ec_devices* devices = container_of(file->private_data, struct qnap_ec_devices,
    misc_device);

  // Check if the open device flag is not set which means we are not expecting any communications
  if (devices->open_misc_device == 0)
  {
    return -EBUSY;
  }

  // Try to lock the miscellaneous device mutex if it's currently unlocked
  // Note: if the mutex is currently locked it means we are already communicating and this is an
  //       unexpected communication
  if (mutex_trylock(&devices->misc_device_mutex) == 0)
  {
    return -EBUSY;
  }

  return 0;
}

// Function called when the miscellaneous device receives a I/O control command
static long int qnap_ec_misc_device_ioctl(struct file* file, unsigned int command,
                                          unsigned long argument)
{
  // Declare and/or define needed variables
  // Note: the following statement is a combination of the following two statements
  //       struct qnap_ec_devices* devices = container_of(file->private_data,
  //         struct qnap_ec_devices, misc_device);
  //       struct qnap_ec_data* data = dev_get_drvdata(&devices->plat_device->dev);
  struct qnap_ec_data* data = dev_get_drvdata(&container_of(file->private_data,
    struct qnap_ec_devices, misc_device)->plat_device->dev);

  // Swtich based on the command
  switch (command)
  {
    case QNAP_EC_IOCTL_CALL:
      // Make sure we can write the data to user space
      if (access_ok(argument, sizeof(struct qnap_ec_ioctl_command)) == 0)
      {
        return -EFAULT;
      }

      // Copy the I/O control command data from the data structure to the user space
      if (copy_to_user((void*)argument, &data->ioctl_command,
        sizeof(struct qnap_ec_ioctl_command)) != 0)
      {
        return -EFAULT;
      }

      return 0;
    case QNAP_EC_IOCTL_RETURN:
      // Make sure we can read the data from user space
      if (access_ok(argument, sizeof(struct qnap_ec_ioctl_command)) == 0)
      {
        return -EFAULT;
      }

      // Copy the I/O control command data from the user space to the data structure
      if (copy_from_user(&data->ioctl_command, (void*)argument,
        sizeof(struct qnap_ec_ioctl_command)) != 0)
      {
        return -EFAULT;
      }

      return 0;
    default:
      return -EINVAL;
  }

  return 0;
}

// Function called when the miscellaneous device is released
static int qnap_ec_misc_device_release(struct inode* inode, struct file* file)
{
  // Declare and/or define needed variables
  struct qnap_ec_devices* devices = container_of(file->private_data, struct qnap_ec_devices,
    misc_device);

  // Release the miscellaneous device mutex lock
  mutex_unlock(&devices->misc_device_mutex);

  return 0;
}

// Function called to exit the driver
static void __exit qnap_ec_exit(void)
{
  // Unregister the miscellaneous device
  misc_deregister(&qnap_ec_devices->misc_device);

  // Unregister the platform device and free the platform device structure memory
  // Note: we are using the platform_device_unregister function instead of the platform_device_put
  //       function used in the qnap_ec_init function because all other hwmon drivers take this
  //       approach
  platform_device_unregister(qnap_ec_devices->plat_device);

  // Free the devices structure memory
  kfree(qnap_ec_devices);

  // Unregister the platform driver
  platform_driver_unregister(qnap_ec_plat_driver);

  // Free the platform driver structure memory
  kfree(qnap_ec_plat_driver);
}