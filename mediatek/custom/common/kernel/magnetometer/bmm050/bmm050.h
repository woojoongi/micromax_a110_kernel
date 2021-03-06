/* bmm050.c - bmm050 compass driver
 * 
 *
 * This software program is licensed subject to the GNU General Public License
 * (GPL).Version 2,June 1991, available at http://www.fsf.org/copyleft/gpl.html

 * (C) Copyright 2011 Bosch Sensortec GmbH
 * All Rights Reserved
 */

/*
 * Definitions for bmm050 magnetic sensor chip.
 */
#ifndef __BMM050_H__
#define __BMM050_H__

#include <linux/ioctl.h>

#define CALIBRATION_DATA_SIZE	12


#define BMM050_I2C_ADDR		0x24

// conversion of magnetic data (for bmm050) to uT units
// conversion of magnetic data to uT units
// 32768 = 1Guass = 100 uT
// 100 / 32768 = 25 / 8096
// 65536 = 360Degree
// 360 / 65536 = 45 / 8192


#define CONVERT_M			1
#define CONVERT_M_DIV		4			
#define CONVERT_O			1
#define CONVERT_O_DIV		71		//(C_PI_F32X * AXIS_RESOLUTION_FACTOR / 180)

#endif /* __BMM050_H__ */

