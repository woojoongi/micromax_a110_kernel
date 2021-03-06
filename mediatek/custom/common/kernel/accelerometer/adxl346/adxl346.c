/* ADXL346 motion sensor driver
 *
 *
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#include <linux/interrupt.h>
#include <linux/i2c.h>
#include <linux/slab.h>
#include <linux/irq.h>
#include <linux/miscdevice.h>
#include <asm/uaccess.h>
#include <linux/delay.h>
#include <linux/input.h>
#include <linux/workqueue.h>
#include <linux/kobject.h>
#include <linux/earlysuspend.h>
#include <linux/platform_device.h>
#include <asm/atomic.h>

#include <cust_acc.h>
#include <linux/hwmsensor.h>
#include <linux/hwmsen_dev.h>
#include <linux/sensors_io.h>
#include "adxl346.h"
#include <linux/hwmsen_helper.h>

#ifdef MT6516
#include <mach/mt6516_devs.h>
#include <mach/mt6516_typedefs.h>
#include <mach/mt6516_gpio.h>
#include <mach/mt6516_pll.h>
#endif

#ifdef MT6573
#include <mach/mt6573_devs.h>
#include <mach/mt6573_typedefs.h>
#include <mach/mt6573_gpio.h>
#include <mach/mt6573_pll.h>
#endif

#ifdef MT6575
#include <mach/mt6575_devs.h>
#include <mach/mt6575_typedefs.h>
#include <mach/mt6575_gpio.h>
#include <mach/mt6575_pm_ldo.h>
#endif

#ifdef MT6577
#include <mach/mt6577_devs.h>
#include <mach/mt6577_typedefs.h>
#include <mach/mt6577_gpio.h>
#include <mach/mt6577_pm_ldo.h>
#endif


/*-------------------------MT6516&MT6573 define-------------------------------*/
#ifdef MT6516
#define POWER_NONE_MACRO MT6516_POWER_NONE
#endif

#ifdef MT6573
#define POWER_NONE_MACRO MT65XX_POWER_NONE
#endif

#ifdef MT6575
#define POWER_NONE_MACRO MT65XX_POWER_NONE
#endif

#ifdef MT6577
#define POWER_NONE_MACRO MT65XX_POWER_NONE
#endif


/*----------------------------------------------------------------------------*/
#define I2C_DRIVERID_ADXL346 346
/*----------------------------------------------------------------------------*/
#define DEBUG 1
/*----------------------------------------------------------------------------*/
#define CONFIG_ADXL346_LOWPASS   /*apply low pass filter on output*/       
/*----------------------------------------------------------------------------*/
#define ADXL346_AXIS_X          0
#define ADXL346_AXIS_Y          1
#define ADXL346_AXIS_Z          2
#define ADXL346_AXES_NUM        3
#define ADXL346_DATA_LEN        6
#define ADXL346_DEV_NAME        "ADXL346"
/*----------------------------------------------------------------------------*/
static const struct i2c_device_id adxl346_i2c_id[] = {{ADXL346_DEV_NAME,0},{}};
static struct i2c_board_info __initdata i2c_adxl346={ I2C_BOARD_INFO("ADXL346", 0x53)};
/*the adapter id will be available in customization*/
//static unsigned short adxl346_force[] = {0x00, ADXL346_I2C_SLAVE_ADDR, I2C_CLIENT_END, I2C_CLIENT_END};
//static const unsigned short *const adxl346_forces[] = { adxl346_force, NULL };
//static struct i2c_client_address_data adxl346_addr_data = { .forces = adxl346_forces,};

/*----------------------------------------------------------------------------*/
static int adxl346_i2c_probe(struct i2c_client *client, const struct i2c_device_id *id); 
static int adxl346_i2c_remove(struct i2c_client *client);
//static int adxl346_i2c_detect(struct i2c_client *client, int kind, struct i2c_board_info *info);
static int adxl346_suspend(struct i2c_client *client, pm_message_t msg) ;
static int adxl346_resume(struct i2c_client *client);

/*----------------------------------------------------------------------------*/
typedef enum {
    ADX_TRC_FILTER  = 0x01,
    ADX_TRC_RAWDATA = 0x02,
    ADX_TRC_IOCTL   = 0x04,
    ADX_TRC_CALI	= 0X08,
    ADX_TRC_INFO	= 0X10,
} ADX_TRC;
/*----------------------------------------------------------------------------*/
struct scale_factor{
    u8  whole;
    u8  fraction;
};
/*----------------------------------------------------------------------------*/
struct data_resolution {
    struct scale_factor scalefactor;
    int                 sensitivity;
};
/*----------------------------------------------------------------------------*/
#define C_MAX_FIR_LENGTH (32)
/*----------------------------------------------------------------------------*/
struct data_filter {
    s16 raw[C_MAX_FIR_LENGTH][ADXL346_AXES_NUM];
    int sum[ADXL346_AXES_NUM];
    int num;
    int idx;
};
/*----------------------------------------------------------------------------*/
struct adxl346_i2c_data {
    struct i2c_client *client;
    struct acc_hw *hw;
    struct hwmsen_convert   cvt;
    
    /*misc*/
    struct data_resolution *reso;
    atomic_t                trace;
    atomic_t                suspend;
    atomic_t                selftest;
	atomic_t				filter;
    s16                     cali_sw[ADXL346_AXES_NUM+1];

    /*data*/
    s8                      offset[ADXL346_AXES_NUM+1];  /*+1: for 4-byte alignment*/
    s16                     data[ADXL346_AXES_NUM+1];

#if defined(CONFIG_ADXL346_LOWPASS)
    atomic_t                firlen;
    atomic_t                fir_en;
    struct data_filter      fir;
#endif 
    /*early suspend*/
#if defined(CONFIG_HAS_EARLYSUSPEND)
    struct early_suspend    early_drv;
#endif     
};
/*----------------------------------------------------------------------------*/
static struct i2c_driver adxl346_i2c_driver = {
    .driver = {
//        .owner          = THIS_MODULE,
        .name           = ADXL346_DEV_NAME,
    },
	.probe      		= adxl346_i2c_probe,
	.remove    			= adxl346_i2c_remove,
//	.detect				= adxl346_i2c_detect,
//#if !defined(CONFIG_HAS_EARLYSUSPEND)    
    .suspend            = adxl346_suspend,
    .resume             = adxl346_resume,
//#endif
	.id_table = adxl346_i2c_id,
//	.address_data = &adxl346_addr_data,
};

/*----------------------------------------------------------------------------*/
static struct i2c_client *adxl346_i2c_client = NULL;
static struct platform_driver adxl346_gsensor_driver;
static struct adxl346_i2c_data *obj_i2c_data = NULL;
static bool sensor_power = false;
static GSENSOR_VECTOR3D gsensor_gain;
//static char selftestRes[8]= {0}; 


/*----------------------------------------------------------------------------*/
#define GSE_TAG                  "[Gsensor] "
#define GSE_FUN(f)               printk(KERN_INFO GSE_TAG"%s\n", __FUNCTION__)
#define GSE_ERR(fmt, args...)    printk(KERN_ERR GSE_TAG"%s %d : "fmt, __FUNCTION__, __LINE__, ##args)
#define GSE_LOG(fmt, args...)    printk(KERN_INFO GSE_TAG fmt, ##args)
/*----------------------------------------------------------------------------*/
static struct data_resolution adxl346_data_resolution[] = {
 /*8 combination by {FULL_RES,RANGE}*/
    {{ 3, 9}, 256},   /*+/-2g  in 10-bit resolution:  3.9 mg/LSB*/
    {{ 7, 8}, 128},   /*+/-4g  in 10-bit resolution:  7.8 mg/LSB*/
    {{15, 6},  64},   /*+/-8g  in 10-bit resolution: 15.6 mg/LSB*/
    {{31, 2},  32},   /*+/-16g in 10-bit resolution: 31.2 mg/LSB*/
    {{ 3, 9}, 256},   /*+/-2g  in 10-bit resolution:  3.9 mg/LSB (full-resolution)*/
    {{ 3, 9}, 256},   /*+/-4g  in 11-bit resolution:  3.9 mg/LSB (full-resolution)*/
    {{ 3, 9}, 256},   /*+/-8g  in 12-bit resolution:  3.9 mg/LSB (full-resolution)*/
    {{ 3, 9}, 256},   /*+/-16g in 13-bit resolution:  3.9 mg/LSB (full-resolution)*/            
};
/*----------------------------------------------------------------------------*/
static struct data_resolution adxl346_offset_resolution = {{15, 6}, 64};

/*--------------------ADXL power control function----------------------------------*/
static void ADXL346_power(struct acc_hw *hw, unsigned int on) 
{
	static unsigned int power_on = 0;

	if(hw->power_id != POWER_NONE_MACRO)		// have externel LDO
	{        
		GSE_LOG("power %s\n", on ? "on" : "off");
		if(power_on == on)	// power status not change
		{
			GSE_LOG("ignore power control: %d\n", on);
		}
		else if(on)	// power on
		{
			if(!hwPowerOn(hw->power_id, hw->power_vol, "ADXL346"))
			{
				GSE_ERR("power on fails!!\n");
			}
		}
		else	// power off
		{
			if (!hwPowerDown(hw->power_id, "ADXL346"))
			{
				GSE_ERR("power off fail!!\n");
			}			  
		}
	}
	power_on = on;    
}
/*----------------------------------------------------------------------------*/
static int ADXL346_SetDataResolution(struct adxl346_i2c_data *obj)
{
	int err;
	u8  dat, reso;

	if((err = hwmsen_read_byte(obj->client, ADXL346_REG_DATA_FORMAT, &dat)))
	{
		GSE_ERR("write data format fail!!\n");
		return err;
	}

	/*the data_reso is combined by 3 bits: {FULL_RES, DATA_RANGE}*/
	reso  = (dat & ADXL346_FULL_RES) ? (0x04) : (0x00);
	reso |= (dat & ADXL346_RANGE_16G); 

	if(reso < sizeof(adxl346_data_resolution)/sizeof(adxl346_data_resolution[0]))
	{        
		obj->reso = &adxl346_data_resolution[reso];
		return 0;
	}
	else
	{
		return -EINVAL;
	}
}
/*----------------------------------------------------------------------------*/
static int ADXL346_ReadData(struct i2c_client *client, s16 data[ADXL346_AXES_NUM])
{
	struct adxl346_i2c_data *priv = i2c_get_clientdata(client);        
	u8 addr = ADXL346_REG_DATAX0;
	u8 buf[ADXL346_DATA_LEN] = {0};
	int err = 0;

	if(NULL == client)
	{
		err = -EINVAL;
	}
	else if((err = hwmsen_read_block(client, addr, buf, 0x06)))
	{
		GSE_ERR("error: %d\n", err);
	}
	else
	{
		data[ADXL346_AXIS_X] = (s16)((buf[ADXL346_AXIS_X*2]) |
		         (buf[ADXL346_AXIS_X*2+1] << 8));
		data[ADXL346_AXIS_Y] = (s16)((buf[ADXL346_AXIS_Y*2]) |
		         (buf[ADXL346_AXIS_Y*2+1] << 8));
		data[ADXL346_AXIS_Z] = (s16)((buf[ADXL346_AXIS_Z*2]) |
		         (buf[ADXL346_AXIS_Z*2+1] << 8));
		data[ADXL346_AXIS_X] += priv->cali_sw[ADXL346_AXIS_X];
		data[ADXL346_AXIS_Y] += priv->cali_sw[ADXL346_AXIS_Y];
		data[ADXL346_AXIS_Z] += priv->cali_sw[ADXL346_AXIS_Z];

		if(atomic_read(&priv->trace) & ADX_TRC_RAWDATA)
		{
			GSE_LOG("[%08X %08X %08X] => [%5d %5d %5d]\n", data[ADXL346_AXIS_X], data[ADXL346_AXIS_Y], data[ADXL346_AXIS_Z],
		                               data[ADXL346_AXIS_X], data[ADXL346_AXIS_Y], data[ADXL346_AXIS_Z]);
		}
#ifdef CONFIG_ADXL346_LOWPASS
		if(atomic_read(&priv->filter))
		{
			if(atomic_read(&priv->fir_en) && !atomic_read(&priv->suspend))
			{
				int idx, firlen = atomic_read(&priv->firlen);   
				if(priv->fir.num < firlen)
				{                
					priv->fir.raw[priv->fir.num][ADXL346_AXIS_X] = data[ADXL346_AXIS_X];
					priv->fir.raw[priv->fir.num][ADXL346_AXIS_Y] = data[ADXL346_AXIS_Y];
					priv->fir.raw[priv->fir.num][ADXL346_AXIS_Z] = data[ADXL346_AXIS_Z];
					priv->fir.sum[ADXL346_AXIS_X] += data[ADXL346_AXIS_X];
					priv->fir.sum[ADXL346_AXIS_Y] += data[ADXL346_AXIS_Y];
					priv->fir.sum[ADXL346_AXIS_Z] += data[ADXL346_AXIS_Z];
					if(atomic_read(&priv->trace) & ADX_TRC_FILTER)
					{
						GSE_LOG("add [%2d] [%5d %5d %5d] => [%5d %5d %5d]\n", priv->fir.num,
							priv->fir.raw[priv->fir.num][ADXL346_AXIS_X], priv->fir.raw[priv->fir.num][ADXL346_AXIS_Y], priv->fir.raw[priv->fir.num][ADXL346_AXIS_Z],
							priv->fir.sum[ADXL346_AXIS_X], priv->fir.sum[ADXL346_AXIS_Y], priv->fir.sum[ADXL346_AXIS_Z]);
					}
					priv->fir.num++;
					priv->fir.idx++;
				}
				else
				{
					idx = priv->fir.idx % firlen;
					priv->fir.sum[ADXL346_AXIS_X] -= priv->fir.raw[idx][ADXL346_AXIS_X];
					priv->fir.sum[ADXL346_AXIS_Y] -= priv->fir.raw[idx][ADXL346_AXIS_Y];
					priv->fir.sum[ADXL346_AXIS_Z] -= priv->fir.raw[idx][ADXL346_AXIS_Z];
					priv->fir.raw[idx][ADXL346_AXIS_X] = data[ADXL346_AXIS_X];
					priv->fir.raw[idx][ADXL346_AXIS_Y] = data[ADXL346_AXIS_Y];
					priv->fir.raw[idx][ADXL346_AXIS_Z] = data[ADXL346_AXIS_Z];
					priv->fir.sum[ADXL346_AXIS_X] += data[ADXL346_AXIS_X];
					priv->fir.sum[ADXL346_AXIS_Y] += data[ADXL346_AXIS_Y];
					priv->fir.sum[ADXL346_AXIS_Z] += data[ADXL346_AXIS_Z];
					priv->fir.idx++;
					data[ADXL346_AXIS_X] = priv->fir.sum[ADXL346_AXIS_X]/firlen;
					data[ADXL346_AXIS_Y] = priv->fir.sum[ADXL346_AXIS_Y]/firlen;
					data[ADXL346_AXIS_Z] = priv->fir.sum[ADXL346_AXIS_Z]/firlen;
					if(atomic_read(&priv->trace) & ADX_TRC_FILTER)
					{
						GSE_LOG("add [%2d] [%5d %5d %5d] => [%5d %5d %5d] : [%5d %5d %5d]\n", idx,
						priv->fir.raw[idx][ADXL346_AXIS_X], priv->fir.raw[idx][ADXL346_AXIS_Y], priv->fir.raw[idx][ADXL346_AXIS_Z],
						priv->fir.sum[ADXL346_AXIS_X], priv->fir.sum[ADXL346_AXIS_Y], priv->fir.sum[ADXL346_AXIS_Z],
						data[ADXL346_AXIS_X], data[ADXL346_AXIS_Y], data[ADXL346_AXIS_Z]);
					}
				}
			}
		}	
#endif         
	}
	return err;
}
/*----------------------------------------------------------------------------*/
static int ADXL346_ReadOffset(struct i2c_client *client, s8 ofs[ADXL346_AXES_NUM])
{    
	int err;

	if((err = hwmsen_read_block(client, ADXL346_REG_OFSX, ofs, ADXL346_AXES_NUM)))
	{
		GSE_ERR("error: %d\n", err);
	}
	
	return err;    
}
/*----------------------------------------------------------------------------*/
static int ADXL346_ResetCalibration(struct i2c_client *client)
{
	struct adxl346_i2c_data *obj = i2c_get_clientdata(client);
	s8 ofs[ADXL346_AXES_NUM] = {0x00, 0x00, 0x00};
	int err;

	if((err = hwmsen_write_block(client, ADXL346_REG_OFSX, ofs, ADXL346_AXES_NUM)))
	{
		GSE_ERR("error: %d\n", err);
	}

	memset(obj->cali_sw, 0x00, sizeof(obj->cali_sw));
	return err;    
}
/*----------------------------------------------------------------------------*/
static int ADXL346_ReadCalibration(struct i2c_client *client, int dat[ADXL346_AXES_NUM])
{
    struct adxl346_i2c_data *obj = i2c_get_clientdata(client);
    int err;
    int mul;

    if ((err = ADXL346_ReadOffset(client, obj->offset))) {
        GSE_ERR("read offset fail, %d\n", err);
        return err;
    }    
    
    mul = obj->reso->sensitivity/adxl346_offset_resolution.sensitivity;

    dat[obj->cvt.map[ADXL346_AXIS_X]] = obj->cvt.sign[ADXL346_AXIS_X]*(obj->offset[ADXL346_AXIS_X]*mul + obj->cali_sw[ADXL346_AXIS_X]);
    dat[obj->cvt.map[ADXL346_AXIS_Y]] = obj->cvt.sign[ADXL346_AXIS_Y]*(obj->offset[ADXL346_AXIS_Y]*mul + obj->cali_sw[ADXL346_AXIS_Y]);
    dat[obj->cvt.map[ADXL346_AXIS_Z]] = obj->cvt.sign[ADXL346_AXIS_Z]*(obj->offset[ADXL346_AXIS_Z]*mul + obj->cali_sw[ADXL346_AXIS_Z]);                        
                                       
    return 0;
}
/*----------------------------------------------------------------------------*/
static int ADXL346_ReadCalibrationEx(struct i2c_client *client, int act[ADXL346_AXES_NUM], int raw[ADXL346_AXES_NUM])
{  
	/*raw: the raw calibration data; act: the actual calibration data*/
	struct adxl346_i2c_data *obj = i2c_get_clientdata(client);
	int err;
	int mul;

	if((err = ADXL346_ReadOffset(client, obj->offset)))
	{
		GSE_ERR("read offset fail, %d\n", err);
		return err;
	}    

	mul = obj->reso->sensitivity/adxl346_offset_resolution.sensitivity;
	raw[ADXL346_AXIS_X] = obj->offset[ADXL346_AXIS_X]*mul + obj->cali_sw[ADXL346_AXIS_X];
	raw[ADXL346_AXIS_Y] = obj->offset[ADXL346_AXIS_Y]*mul + obj->cali_sw[ADXL346_AXIS_Y];
	raw[ADXL346_AXIS_Z] = obj->offset[ADXL346_AXIS_Z]*mul + obj->cali_sw[ADXL346_AXIS_Z];

	act[obj->cvt.map[ADXL346_AXIS_X]] = obj->cvt.sign[ADXL346_AXIS_X]*raw[ADXL346_AXIS_X];
	act[obj->cvt.map[ADXL346_AXIS_Y]] = obj->cvt.sign[ADXL346_AXIS_Y]*raw[ADXL346_AXIS_Y];
	act[obj->cvt.map[ADXL346_AXIS_Z]] = obj->cvt.sign[ADXL346_AXIS_Z]*raw[ADXL346_AXIS_Z];                        
	                       
	return 0;
}
/*----------------------------------------------------------------------------*/
static int ADXL346_WriteCalibration(struct i2c_client *client, int dat[ADXL346_AXES_NUM])
{
	struct adxl346_i2c_data *obj = i2c_get_clientdata(client);
	int err;
	int cali[ADXL346_AXES_NUM], raw[ADXL346_AXES_NUM];
	int lsb = adxl346_offset_resolution.sensitivity;
	int divisor = obj->reso->sensitivity/lsb;

	if((err = ADXL346_ReadCalibrationEx(client, cali, raw)))	/*offset will be updated in obj->offset*/
	{ 
		GSE_ERR("read offset fail, %d\n", err);
		return err;
	}

	GSE_LOG("OLDOFF: (%+3d %+3d %+3d): (%+3d %+3d %+3d) / (%+3d %+3d %+3d)\n", 
		raw[ADXL346_AXIS_X], raw[ADXL346_AXIS_Y], raw[ADXL346_AXIS_Z],
		obj->offset[ADXL346_AXIS_X], obj->offset[ADXL346_AXIS_Y], obj->offset[ADXL346_AXIS_Z],
		obj->cali_sw[ADXL346_AXIS_X], obj->cali_sw[ADXL346_AXIS_Y], obj->cali_sw[ADXL346_AXIS_Z]);

	/*calculate the real offset expected by caller*/
	cali[ADXL346_AXIS_X] += dat[ADXL346_AXIS_X];
	cali[ADXL346_AXIS_Y] += dat[ADXL346_AXIS_Y];
	cali[ADXL346_AXIS_Z] += dat[ADXL346_AXIS_Z];

	GSE_LOG("UPDATE: (%+3d %+3d %+3d)\n", 
		dat[ADXL346_AXIS_X], dat[ADXL346_AXIS_Y], dat[ADXL346_AXIS_Z]);

	obj->offset[ADXL346_AXIS_X] = (s8)(obj->cvt.sign[ADXL346_AXIS_X]*(cali[obj->cvt.map[ADXL346_AXIS_X]])/(divisor));
	obj->offset[ADXL346_AXIS_Y] = (s8)(obj->cvt.sign[ADXL346_AXIS_Y]*(cali[obj->cvt.map[ADXL346_AXIS_Y]])/(divisor));
	obj->offset[ADXL346_AXIS_Z] = (s8)(obj->cvt.sign[ADXL346_AXIS_Z]*(cali[obj->cvt.map[ADXL346_AXIS_Z]])/(divisor));

	/*convert software calibration using standard calibration*/
	obj->cali_sw[ADXL346_AXIS_X] = obj->cvt.sign[ADXL346_AXIS_X]*(cali[obj->cvt.map[ADXL346_AXIS_X]])%(divisor);
	obj->cali_sw[ADXL346_AXIS_Y] = obj->cvt.sign[ADXL346_AXIS_Y]*(cali[obj->cvt.map[ADXL346_AXIS_Y]])%(divisor);
	obj->cali_sw[ADXL346_AXIS_Z] = obj->cvt.sign[ADXL346_AXIS_Z]*(cali[obj->cvt.map[ADXL346_AXIS_Z]])%(divisor);

	GSE_LOG("NEWOFF: (%+3d %+3d %+3d): (%+3d %+3d %+3d) / (%+3d %+3d %+3d)\n", 
		obj->offset[ADXL346_AXIS_X]*divisor + obj->cali_sw[ADXL346_AXIS_X], 
		obj->offset[ADXL346_AXIS_Y]*divisor + obj->cali_sw[ADXL346_AXIS_Y], 
		obj->offset[ADXL346_AXIS_Z]*divisor + obj->cali_sw[ADXL346_AXIS_Z], 
		obj->offset[ADXL346_AXIS_X], obj->offset[ADXL346_AXIS_Y], obj->offset[ADXL346_AXIS_Z],
		obj->cali_sw[ADXL346_AXIS_X], obj->cali_sw[ADXL346_AXIS_Y], obj->cali_sw[ADXL346_AXIS_Z]);

	if((err = hwmsen_write_block(obj->client, ADXL346_REG_OFSX, obj->offset, ADXL346_AXES_NUM)))
	{
		GSE_ERR("write offset fail: %d\n", err);
		return err;
	}

	return err;
}
/*----------------------------------------------------------------------------*/
static int ADXL346_CheckDeviceID(struct i2c_client *client)
{
	u8 databuf[10];    
	int res = 0;

	memset(databuf, 0, sizeof(u8)*10);    
	databuf[0] = ADXL346_REG_DEVID;    

	res = i2c_master_send(client, databuf, 0x1);
	if(res <= 0)
	{
		goto exit_ADXL346_CheckDeviceID;
	}
	
	udelay(500);

	databuf[0] = 0x0;        
	res = i2c_master_recv(client, databuf, 0x01);
	if(res <= 0)
	{
		goto exit_ADXL346_CheckDeviceID;
	}
	

	if(databuf[0]!=ADXL346_FIXED_DEVID)
	{
		return ADXL346_ERR_IDENTIFICATION;
	}

	exit_ADXL346_CheckDeviceID:
	if (res <= 0)
	{
		return ADXL346_ERR_I2C;
	}
	
	return ADXL346_SUCCESS;
}
/*----------------------------------------------------------------------------*/
static int ADXL346_SetPowerMode(struct i2c_client *client, bool enable)
{
	u8 databuf[2];    
	int res = 0;
	u8 addr = ADXL346_REG_POWER_CTL;
	struct adxl346_i2c_data *obj = i2c_get_clientdata(client);
	
	
	if(enable == sensor_power)
	{
		GSE_LOG("Sensor power status is newest!\n");
		return ADXL346_SUCCESS;
	}

	if(hwmsen_read_block(client, addr, databuf, 0x01))
	{
		GSE_ERR("read power ctl register err!\n");
		return ADXL346_ERR_I2C;
	}

	databuf[0] &= ~ADXL346_MEASURE_MODE;
	
	if(enable == TRUE)
	{
		databuf[0] |= ADXL346_MEASURE_MODE;
	}
	else
	{
		// do nothing
	}
	databuf[1] = databuf[0];
	databuf[0] = ADXL346_REG_POWER_CTL;
	

	res = i2c_master_send(client, databuf, 0x2);

	if(res <= 0)
	{
		GSE_LOG("set power mode failed!\n");
		return ADXL346_ERR_I2C;
	}
	else if(atomic_read(&obj->trace) & ADX_TRC_INFO)
	{
		GSE_LOG("set power mode ok %d!\n", databuf[1]);
	}

	sensor_power = enable;
	
	return ADXL346_SUCCESS;    
}
/*----------------------------------------------------------------------------*/
static int ADXL346_SetDataFormat(struct i2c_client *client, u8 dataformat)
{
	struct adxl346_i2c_data *obj = i2c_get_clientdata(client);
	u8 databuf[10];    
	int res = 0;

	memset(databuf, 0, sizeof(u8)*10);    
	databuf[0] = ADXL346_REG_DATA_FORMAT;    
	databuf[1] = dataformat;

	res = i2c_master_send(client, databuf, 0x2);

	if(res <= 0)
	{
		return ADXL346_ERR_I2C;
	}
	

	return ADXL346_SetDataResolution(obj);    
}
/*----------------------------------------------------------------------------*/
static int ADXL346_SetBWRate(struct i2c_client *client, u8 bwrate)
{
	u8 databuf[10];    
	int res = 0;

	memset(databuf, 0, sizeof(u8)*10);    
	databuf[0] = ADXL346_REG_BW_RATE;    
	databuf[1] = bwrate;

	res = i2c_master_send(client, databuf, 0x2);

	if(res <= 0)
	{
		return ADXL346_ERR_I2C;
	}
	
	return ADXL346_SUCCESS;    
}
/*----------------------------------------------------------------------------*/
static int ADXL346_SetIntEnable(struct i2c_client *client, u8 intenable)
{
	u8 databuf[10];    
	int res = 0;

	memset(databuf, 0, sizeof(u8)*10);    
	databuf[0] = ADXL346_REG_INT_ENABLE;    
	databuf[1] = intenable;

	res = i2c_master_send(client, databuf, 0x2);

	if(res <= 0)
	{
		return ADXL346_ERR_I2C;
	}
	
	return ADXL346_SUCCESS;    
}
/*----------------------------------------------------------------------------*/
//Remove it because it is not used now
/*
static int adxl346_gpio_config(void)
{
   //because we donot use EINT to support low power
   // config to GPIO input mode + PD 
    
    //set to GPIO_GSE_1_EINT_PIN
    mt_set_gpio_mode(GPIO_GSE_1_EINT_PIN, GPIO_GSE_1_EINT_PIN_M_GPIO);
    mt_set_gpio_dir(GPIO_GSE_1_EINT_PIN, GPIO_DIR_IN);
	mt_set_gpio_pull_enable(GPIO_GSE_1_EINT_PIN, GPIO_PULL_ENABLE);
	mt_set_gpio_pull_select(GPIO_GSE_1_EINT_PIN, GPIO_PULL_DOWN);
    //set to GPIO_GSE_2_EINT_PIN
	mt_set_gpio_mode(GPIO_GSE_2_EINT_PIN, GPIO_GSE_2_EINT_PIN_M_GPIO);
    mt_set_gpio_dir(GPIO_GSE_2_EINT_PIN, GPIO_DIR_IN);
	mt_set_gpio_pull_enable(GPIO_GSE_2_EINT_PIN, GPIO_PULL_ENABLE);
	mt_set_gpio_pull_select(GPIO_GSE_2_EINT_PIN, GPIO_PULL_DOWN);
	return 0;
}
*/

static int adxl346_init_client(struct i2c_client *client, int reset_cali)
{
	struct adxl346_i2c_data *obj = i2c_get_clientdata(client);
	int res = 0;

       //adxl346_gpio_config();
	
	res = ADXL346_CheckDeviceID(client); 
	if(res != ADXL346_SUCCESS)
	{
	    GSE_ERR("Check ID error\n");
		return res;
	}	

	res = ADXL346_SetPowerMode(client, false);
	if(res != ADXL346_SUCCESS)
	{
	    GSE_ERR("set power error\n");
		return res;
	}
	

	res = ADXL346_SetBWRate(client, ADXL346_BW_100HZ);
	if(res != ADXL346_SUCCESS ) //0x2C->BW=100Hz
	{
	    GSE_ERR("set power error\n");
		return res;
	}

	res = ADXL346_SetDataFormat(client, ADXL346_FULL_RES|ADXL346_RANGE_2G);
	if(res != ADXL346_SUCCESS) //0x2C->BW=100Hz
	{
	    GSE_ERR("set data format error\n");
		return res;
	}

	gsensor_gain.x = gsensor_gain.y = gsensor_gain.z = obj->reso->sensitivity;

	res = ADXL346_SetIntEnable(client, 0x00);//disable INT        
	if(res != ADXL346_SUCCESS) 
	{
	    GSE_ERR("ADXL346_SetIntEnable error\n");
		return res;
	}

	if(0 != reset_cali)
	{ 
		/*reset calibration only in power on*/
		res = ADXL346_ResetCalibration(client);
		if(res != ADXL346_SUCCESS)
		{
			return res;
		}
	}

#ifdef CONFIG_ADXL346_LOWPASS
	memset(&obj->fir, 0x00, sizeof(obj->fir));  
#endif

	return ADXL346_SUCCESS;
}
/*----------------------------------------------------------------------------*/
static int ADXL346_ReadChipInfo(struct i2c_client *client, char *buf, int bufsize)
{
	u8 databuf[10];    

	memset(databuf, 0, sizeof(u8)*10);

	if((NULL == buf)||(bufsize<=30))
	{
		return -1;
	}
	
	if(NULL == client)
	{
		*buf = 0;
		return -2;
	}

	sprintf(buf, "ADXL346 Chip");
	return 0;
}
/*----------------------------------------------------------------------------*/
static int ADXL346_ReadSensorData(struct i2c_client *client, char *buf, int bufsize)
{
	int acc[ADXL346_AXES_NUM];
	int res = 0;
	struct adxl346_i2c_data *obj = obj_i2c_data; //(struct adxl346_i2c_data*)i2c_get_clientdata(client);
	client = obj->client;
	//u8 databuf[20];

	//memset(databuf, 0, sizeof(u8)*10);

	if(NULL == buf)
	{
		return -1;
	}
	if(NULL == client)
	{
		*buf = 0;
		return -2;
	}

	if(sensor_power == FALSE)
	{
		res = ADXL346_SetPowerMode(client, true);
		if(res)
		{
			GSE_ERR("Power on adxl346 error %d!\n", res);
		}
		msleep(20);
	}

	if((res = ADXL346_ReadData(client, obj->data)))
	{        
		GSE_ERR("I2C error: ret value=%d", res);
		return -3;
	}
	else
	{
		obj->data[ADXL346_AXIS_X] += obj->cali_sw[ADXL346_AXIS_X];
		obj->data[ADXL346_AXIS_Y] += obj->cali_sw[ADXL346_AXIS_Y];
		obj->data[ADXL346_AXIS_Z] += obj->cali_sw[ADXL346_AXIS_Z];
		
		/*remap coordinate*/
		acc[obj->cvt.map[ADXL346_AXIS_X]] = obj->cvt.sign[ADXL346_AXIS_X]*obj->data[ADXL346_AXIS_X];
		acc[obj->cvt.map[ADXL346_AXIS_Y]] = obj->cvt.sign[ADXL346_AXIS_Y]*obj->data[ADXL346_AXIS_Y];
		acc[obj->cvt.map[ADXL346_AXIS_Z]] = obj->cvt.sign[ADXL346_AXIS_Z]*obj->data[ADXL346_AXIS_Z];

		//GSE_LOG("Mapped gsensor data: %d, %d, %d!\n", acc[ADXL346_AXIS_X], acc[ADXL346_AXIS_Y], acc[ADXL346_AXIS_Z]);

		//Out put the mg
		acc[ADXL346_AXIS_X] = acc[ADXL346_AXIS_X] * GRAVITY_EARTH_1000 / obj->reso->sensitivity;
		acc[ADXL346_AXIS_Y] = acc[ADXL346_AXIS_Y] * GRAVITY_EARTH_1000 / obj->reso->sensitivity;
		acc[ADXL346_AXIS_Z] = acc[ADXL346_AXIS_Z] * GRAVITY_EARTH_1000 / obj->reso->sensitivity;		
		

		sprintf(buf, "%04x %04x %04x", acc[ADXL346_AXIS_X], acc[ADXL346_AXIS_Y], acc[ADXL346_AXIS_Z]);
		if(atomic_read(&obj->trace) & ADX_TRC_IOCTL)
		{
			GSE_LOG("gsensor data: %s!\n", buf);
		}
		
	}
	
	return 0;
}
/*----------------------------------------------------------------------------*/
static int ADXL346_ReadRawData(struct i2c_client *client, char *buf)
{
	struct adxl346_i2c_data *obj = (struct adxl346_i2c_data*)i2c_get_clientdata(client);
	int res = 0;

	if (!buf || !client)
	{
		return EINVAL;
	}
	
	if((res = ADXL346_ReadData(client, obj->data)))
	{        
		GSE_ERR("I2C error: ret value=%d", res);
		return EIO;
	}
	else
	{
		sprintf(buf, "%04x %04x %04x", obj->data[ADXL346_AXIS_X], 
			obj->data[ADXL346_AXIS_Y], obj->data[ADXL346_AXIS_Z]);
	
	}
	
	return 0;
}
/*----------------------------------------------------------------------------*/
/*
static int ADXL346_InitSelfTest(struct i2c_client *client)
{
	int res = 0;
	u8  data;

	res = ADXL346_SetBWRate(client, ADXL346_BW_100HZ);
	if(res != ADXL346_SUCCESS ) //0x2C->BW=100Hz
	{
		return res;
	}
	
	res = hwmsen_read_byte(client, ADXL346_REG_DATA_FORMAT, &data);
	if(res != ADXL346_SUCCESS)
	{
		return res;
	}

	res = ADXL346_SetDataFormat(client, ADXL346_SELF_TEST|data);
	if(res != ADXL346_SUCCESS) //0x2C->BW=100Hz
	{
		return res;
	}
	
	return ADXL346_SUCCESS;
}
*/
/*----------------------------------------------------------------------------*/
/*
static int ADXL346_JudgeTestResult(struct i2c_client *client, s32 prv[ADXL346_AXES_NUM], s32 nxt[ADXL346_AXES_NUM])
{
    struct criteria {
        int min;
        int max;
    };
	
    struct criteria self[4][3] = {
        {{50, 540}, {-540, -50}, {75, 875}},
        {{25, 270}, {-270, -25}, {38, 438}},
        {{12, 135}, {-135, -12}, {19, 219}},            
        {{ 6,  67}, {-67,  -6},  {10, 110}},            
    };
    struct criteria (*ptr)[3] = NULL;
    u8 format;
    int res;
    if((res = hwmsen_read_byte(client, ADXL346_REG_DATA_FORMAT, &format)))
        return res;
    if(format & ADXL346_FULL_RES)
        ptr = &self[0];
    else if ((format & ADXL346_RANGE_4G))
        ptr = &self[1];
    else if ((format & ADXL346_RANGE_8G))
        ptr = &self[2];
    else if ((format & ADXL346_RANGE_16G))
        ptr = &self[3];

    if (!ptr) {
        GSE_ERR("null pointer\n");
        return -EINVAL;
    }
	GSE_LOG("format=%x\n",format);

    if (((nxt[ADXL346_AXIS_X] - prv[ADXL346_AXIS_X]) > (*ptr)[ADXL346_AXIS_X].max) ||
        ((nxt[ADXL346_AXIS_X] - prv[ADXL346_AXIS_X]) < (*ptr)[ADXL346_AXIS_X].min)) {
        GSE_ERR("X is over range\n");
        res = -EINVAL;
    }
    if (((nxt[ADXL346_AXIS_Y] - prv[ADXL346_AXIS_Y]) > (*ptr)[ADXL346_AXIS_Y].max) ||
        ((nxt[ADXL346_AXIS_Y] - prv[ADXL346_AXIS_Y]) < (*ptr)[ADXL346_AXIS_Y].min)) {
        GSE_ERR("Y is over range\n");
        res = -EINVAL;
    }
    if (((nxt[ADXL346_AXIS_Z] - prv[ADXL346_AXIS_Z]) > (*ptr)[ADXL346_AXIS_Z].max) ||
        ((nxt[ADXL346_AXIS_Z] - prv[ADXL346_AXIS_Z]) < (*ptr)[ADXL346_AXIS_Z].min)) {
        GSE_ERR("Z is over range\n");
        res = -EINVAL;
    }
    return res;
}
*/
/*----------------------------------------------------------------------------*/
static ssize_t show_chipinfo_value(struct device_driver *ddri, char *buf)
{
	struct i2c_client *client = adxl346_i2c_client;
	char strbuf[ADXL346_BUFSIZE];
	if(NULL == client)
	{
		GSE_ERR("i2c client is null!!\n");
		return 0;
	}
	
	ADXL346_ReadChipInfo(client, strbuf, ADXL346_BUFSIZE);
	return snprintf(buf, PAGE_SIZE, "%s\n", strbuf);        
}
/*----------------------------------------------------------------------------*/
static ssize_t show_sensordata_value(struct device_driver *ddri, char *buf)
{
	struct i2c_client *client = adxl346_i2c_client;
	char strbuf[ADXL346_BUFSIZE];
	
	if(NULL == client)
	{
		GSE_ERR("i2c client is null!!\n");
		return 0;
	}
	ADXL346_ReadSensorData(client, strbuf, ADXL346_BUFSIZE);
	return snprintf(buf, PAGE_SIZE, "%s\n", strbuf);            
}
/*----------------------------------------------------------------------------*/
static ssize_t show_cali_value(struct device_driver *ddri, char *buf)
{
	struct i2c_client *client = adxl346_i2c_client;
	struct adxl346_i2c_data *obj;
	int err, len = 0, mul;
	int tmp[ADXL346_AXES_NUM];

	if(NULL == client)
	{
		GSE_ERR("i2c client is null!!\n");
		return 0;
	}

	obj = i2c_get_clientdata(client);



	if((err = ADXL346_ReadOffset(client, obj->offset)))
	{
		return -EINVAL;
	}
	else if((err = ADXL346_ReadCalibration(client, tmp)))
	{
		return -EINVAL;
	}
	else
	{    
		mul = obj->reso->sensitivity/adxl346_offset_resolution.sensitivity;
		len += snprintf(buf+len, PAGE_SIZE-len, "[HW ][%d] (%+3d, %+3d, %+3d) : (0x%02X, 0x%02X, 0x%02X)\n", mul,                        
			obj->offset[ADXL346_AXIS_X], obj->offset[ADXL346_AXIS_Y], obj->offset[ADXL346_AXIS_Z],
			obj->offset[ADXL346_AXIS_X], obj->offset[ADXL346_AXIS_Y], obj->offset[ADXL346_AXIS_Z]);
		len += snprintf(buf+len, PAGE_SIZE-len, "[SW ][%d] (%+3d, %+3d, %+3d)\n", 1, 
			obj->cali_sw[ADXL346_AXIS_X], obj->cali_sw[ADXL346_AXIS_Y], obj->cali_sw[ADXL346_AXIS_Z]);

		len += snprintf(buf+len, PAGE_SIZE-len, "[ALL]    (%+3d, %+3d, %+3d) : (%+3d, %+3d, %+3d)\n", 
			obj->offset[ADXL346_AXIS_X]*mul + obj->cali_sw[ADXL346_AXIS_X],
			obj->offset[ADXL346_AXIS_Y]*mul + obj->cali_sw[ADXL346_AXIS_Y],
			obj->offset[ADXL346_AXIS_Z]*mul + obj->cali_sw[ADXL346_AXIS_Z],
			tmp[ADXL346_AXIS_X], tmp[ADXL346_AXIS_Y], tmp[ADXL346_AXIS_Z]);
		
		return len;
    }
}
/*----------------------------------------------------------------------------*/
static ssize_t store_cali_value(struct device_driver *ddri, const char *buf, size_t count)
{
	struct i2c_client *client = adxl346_i2c_client;  
	int err, x, y, z;
	int dat[ADXL346_AXES_NUM];

	if(!strncmp(buf, "rst", 3))
	{
		if((err = ADXL346_ResetCalibration(client)))
		{
			GSE_ERR("reset offset err = %d\n", err);
		}	
	}
	else if(3 == sscanf(buf, "0x%02X 0x%02X 0x%02X", &x, &y, &z))
	{
		dat[ADXL346_AXIS_X] = x;
		dat[ADXL346_AXIS_Y] = y;
		dat[ADXL346_AXIS_Z] = z;
		if((err = ADXL346_WriteCalibration(client, dat)))
		{
			GSE_ERR("write calibration err = %d\n", err);
		}		
	}
	else
	{
		GSE_ERR("invalid format\n");
	}
	
	return count;
}
/*----------------------------------------------------------------------------*/
//Remove it because it is not used now
/*
static ssize_t show_self_value(struct device_driver *ddri, char *buf)
{
	struct i2c_client *client = adxl346_i2c_client;
	//struct adxl346_i2c_data *obj;

	if(NULL == client)
	{
		GSE_ERR("i2c client is null!!\n");
		return 0;
	}

	//obj = i2c_get_clientdata(client);

    return snprintf(buf, 8, "%s\n", selftestRes);
}
*/
/*----------------------------------------------------------------------------*/
//Remove it because it is not used now
/*
static ssize_t store_self_value(struct device_driver *ddri, const char *buf, size_t count)
{   
    //write anything to this register will trigger the process
	struct item{
	s16 raw[ADXL346_AXES_NUM];
	};
	
	struct i2c_client *client = adxl346_i2c_client;  
	int idx, res, num;
	struct item *prv = NULL, *nxt = NULL;
	s32 avg_prv[ADXL346_AXES_NUM] = {0, 0, 0};
	s32 avg_nxt[ADXL346_AXES_NUM] = {0, 0, 0};


	if(1 != sscanf(buf, "%d", &num))
	{
		GSE_ERR("parse number fail\n");
		return count;
	}
	else if(num == 0)
	{
		GSE_ERR("invalid data count\n");
		return count;
	}

	prv = kzalloc(sizeof(*prv) * num, GFP_KERNEL);
	nxt = kzalloc(sizeof(*nxt) * num, GFP_KERNEL);
	if (!prv || !nxt)
	{
		goto exit;
	}


	GSE_LOG("NORMAL:\n");
	ADXL346_SetPowerMode(client,true);
	for(idx = 0; idx < num; idx++)
	{
		if((res = ADXL346_ReadData(client, prv[idx].raw)))
		{            
			GSE_ERR("read data fail: %d\n", res);
			goto exit;
		}
		
		avg_prv[ADXL346_AXIS_X] += prv[idx].raw[ADXL346_AXIS_X];
		avg_prv[ADXL346_AXIS_Y] += prv[idx].raw[ADXL346_AXIS_Y];
		avg_prv[ADXL346_AXIS_Z] += prv[idx].raw[ADXL346_AXIS_Z];        
		GSE_LOG("[%5d %5d %5d]\n", prv[idx].raw[ADXL346_AXIS_X], prv[idx].raw[ADXL346_AXIS_Y], prv[idx].raw[ADXL346_AXIS_Z]);
	}
	
	avg_prv[ADXL346_AXIS_X] /= num;
	avg_prv[ADXL346_AXIS_Y] /= num;
	avg_prv[ADXL346_AXIS_Z] /= num;    

	//initial setting for self test
	ADXL346_InitSelfTest(client);
	GSE_LOG("SELFTEST:\n");    
	for(idx = 0; idx < num; idx++)
	{
		if((res = ADXL346_ReadData(client, nxt[idx].raw)))
		{            
			GSE_ERR("read data fail: %d\n", res);
			goto exit;
		}
		avg_nxt[ADXL346_AXIS_X] += nxt[idx].raw[ADXL346_AXIS_X];
		avg_nxt[ADXL346_AXIS_Y] += nxt[idx].raw[ADXL346_AXIS_Y];
		avg_nxt[ADXL346_AXIS_Z] += nxt[idx].raw[ADXL346_AXIS_Z];        
		GSE_LOG("[%5d %5d %5d]\n", nxt[idx].raw[ADXL346_AXIS_X], nxt[idx].raw[ADXL346_AXIS_Y], nxt[idx].raw[ADXL346_AXIS_Z]);
	}
	
	avg_nxt[ADXL346_AXIS_X] /= num;
	avg_nxt[ADXL346_AXIS_Y] /= num;
	avg_nxt[ADXL346_AXIS_Z] /= num;    

	GSE_LOG("X: %5d - %5d = %5d \n", avg_nxt[ADXL346_AXIS_X], avg_prv[ADXL346_AXIS_X], avg_nxt[ADXL346_AXIS_X] - avg_prv[ADXL346_AXIS_X]);
	GSE_LOG("Y: %5d - %5d = %5d \n", avg_nxt[ADXL346_AXIS_Y], avg_prv[ADXL346_AXIS_Y], avg_nxt[ADXL346_AXIS_Y] - avg_prv[ADXL346_AXIS_Y]);
	GSE_LOG("Z: %5d - %5d = %5d \n", avg_nxt[ADXL346_AXIS_Z], avg_prv[ADXL346_AXIS_Z], avg_nxt[ADXL346_AXIS_Z] - avg_prv[ADXL346_AXIS_Z]); 

	if(!ADXL346_JudgeTestResult(client, avg_prv, avg_nxt))
	{
		GSE_LOG("SELFTEST : PASS\n");
		strcpy(selftestRes,"y");
	}	
	else
	{
		GSE_LOG("SELFTEST : FAIL\n");
		strcpy(selftestRes,"n");
	}
	
	exit:
	//restore the setting
	adxl346_init_client(client, 0);
	kfree(prv);
	kfree(nxt);
	return count;
}
*/
/*----------------------------------------------------------------------------*/
//Remove it because it is not used now
/*
static ssize_t show_selftest_value(struct device_driver *ddri, char *buf)
{
	struct i2c_client *client = adxl346_i2c_client;
	struct adxl346_i2c_data *obj;

	if(NULL == client)
	{
		GSE_ERR("i2c client is null!!\n");
		return 0;
	}

	obj = i2c_get_clientdata(client);
	return snprintf(buf, PAGE_SIZE, "%d\n", atomic_read(&obj->selftest));
}
*/
/*----------------------------------------------------------------------------*/
//Remove it because it is not used now
/*
static ssize_t store_selftest_value(struct device_driver *ddri, const char *buf, size_t count)
{
	struct adxl346_i2c_data *obj = obj_i2c_data;
	int tmp;

	if(NULL == obj)
	{
		GSE_ERR("i2c data obj is null!!\n");
		return 0;
	}
	
	
	if(1 == sscanf(buf, "%d", &tmp))
	{        
		if(atomic_read(&obj->selftest) && !tmp)
		{
			//enable -> disable
			adxl346_init_client(obj->client, 0);
		}
		else if(!atomic_read(&obj->selftest) && tmp)
		{
			//disable -> enable
			ADXL346_InitSelfTest(obj->client);            
		}
		
		GSE_LOG("selftest: %d => %d\n", atomic_read(&obj->selftest), tmp);
		atomic_set(&obj->selftest, tmp); 
	}
	else
	{ 
		GSE_ERR("invalid content: '%s', length = %d\n", buf, count);   
	}
	return count;
}
*/
/*----------------------------------------------------------------------------*/
static ssize_t show_firlen_value(struct device_driver *ddri, char *buf)
{
#ifdef CONFIG_ADXL346_LOWPASS
	struct i2c_client *client = adxl346_i2c_client;
	struct adxl346_i2c_data *obj = i2c_get_clientdata(client);
	if(atomic_read(&obj->firlen))
	{
		int idx, len = atomic_read(&obj->firlen);
		GSE_LOG("len = %2d, idx = %2d\n", obj->fir.num, obj->fir.idx);

		for(idx = 0; idx < len; idx++)
		{
			GSE_LOG("[%5d %5d %5d]\n", obj->fir.raw[idx][ADXL346_AXIS_X], obj->fir.raw[idx][ADXL346_AXIS_Y], obj->fir.raw[idx][ADXL346_AXIS_Z]);
		}
		
		GSE_LOG("sum = [%5d %5d %5d]\n", obj->fir.sum[ADXL346_AXIS_X], obj->fir.sum[ADXL346_AXIS_Y], obj->fir.sum[ADXL346_AXIS_Z]);
		GSE_LOG("avg = [%5d %5d %5d]\n", obj->fir.sum[ADXL346_AXIS_X]/len, obj->fir.sum[ADXL346_AXIS_Y]/len, obj->fir.sum[ADXL346_AXIS_Z]/len);
	}
	return snprintf(buf, PAGE_SIZE, "%d\n", atomic_read(&obj->firlen));
#else
	return snprintf(buf, PAGE_SIZE, "not support\n");
#endif
}
/*----------------------------------------------------------------------------*/
static ssize_t store_firlen_value(struct device_driver *ddri, const char *buf, size_t count)
{
#ifdef CONFIG_ADXL346_LOWPASS
	struct i2c_client *client = adxl346_i2c_client;  
	struct adxl346_i2c_data *obj = i2c_get_clientdata(client);
	int firlen;

	if(1 != sscanf(buf, "%d", &firlen))
	{
		GSE_ERR("invallid format\n");
	}
	else if(firlen > C_MAX_FIR_LENGTH)
	{
		GSE_ERR("exceeds maximum filter length\n");
	}
	else
	{ 
		atomic_set(&obj->firlen, firlen);
		if(0 == firlen)
		{
			atomic_set(&obj->fir_en, 0);
		}
		else
		{
			memset(&obj->fir, 0x00, sizeof(obj->fir));
			atomic_set(&obj->fir_en, 1);
		}
	}
#endif    
	return count;
}
/*----------------------------------------------------------------------------*/
static ssize_t show_trace_value(struct device_driver *ddri, char *buf)
{
	ssize_t res;
	struct adxl346_i2c_data *obj = obj_i2c_data;
	if (obj == NULL)
	{
		GSE_ERR("i2c_data obj is null!!\n");
		return 0;
	}
	
	res = snprintf(buf, PAGE_SIZE, "0x%04X\n", atomic_read(&obj->trace));     
	return res;    
}
/*----------------------------------------------------------------------------*/
static ssize_t store_trace_value(struct device_driver *ddri, const char *buf, size_t count)
{
	struct adxl346_i2c_data *obj = obj_i2c_data;
	int trace;
	if (obj == NULL)
	{
		GSE_ERR("i2c_data obj is null!!\n");
		return 0;
	}
	
	if(1 == sscanf(buf, "0x%x", &trace))
	{
		atomic_set(&obj->trace, trace);
	}	
	else
	{
		GSE_ERR("invalid content: '%s', length = %d\n", buf, count);
	}
	
	return count;    
}
/*----------------------------------------------------------------------------*/
static ssize_t show_status_value(struct device_driver *ddri, char *buf)
{
	ssize_t len = 0;    
	struct adxl346_i2c_data *obj = obj_i2c_data;
	if (obj == NULL)
	{
		GSE_ERR("i2c_data obj is null!!\n");
		return 0;
	}	
	
	if(obj->hw)
	{
		len += snprintf(buf+len, PAGE_SIZE-len, "CUST: %d %d (%d %d)\n", 
	            obj->hw->i2c_num, obj->hw->direction, obj->hw->power_id, obj->hw->power_vol);   
	}
	else
	{
		len += snprintf(buf+len, PAGE_SIZE-len, "CUST: NULL\n");
	}
	return len;    
}

static ssize_t show_power_status_value(struct device_driver *ddri, char *buf)
{
	int relv = 0;
	if(sensor_power)
		relv = snprintf(buf, PAGE_SIZE, "1\n"); 
	else
		relv = snprintf(buf, PAGE_SIZE, "0\n"); 

	return relv;
}

/*----------------------------------------------------------------------------*/
static DRIVER_ATTR(chipinfo,             S_IRUGO, show_chipinfo_value,      NULL);
static DRIVER_ATTR(sensordata,           S_IRUGO, show_sensordata_value,    NULL);
static DRIVER_ATTR(cali,       S_IWUSR | S_IRUGO, show_cali_value,          store_cali_value);
//static DRIVER_ATTR(self,       S_IWUSR | S_IRUGO, show_selftest_value,          store_selftest_value);
//static DRIVER_ATTR(selftest,   S_IWUSR | S_IRUGO, show_self_value ,      store_self_value );
static DRIVER_ATTR(firlen,     S_IWUSR | S_IRUGO, show_firlen_value,        store_firlen_value);
static DRIVER_ATTR(trace,      S_IWUSR | S_IRUGO, show_trace_value,         store_trace_value);
static DRIVER_ATTR(status,               S_IRUGO, show_status_value,        NULL);
static DRIVER_ATTR(powerstatus,          S_IRUGO, show_power_status_value,        NULL);

/*----------------------------------------------------------------------------*/
static struct driver_attribute *adxl346_attr_list[] = {
	&driver_attr_chipinfo,     /*chip information*/
	&driver_attr_sensordata,   /*dump sensor data*/
	&driver_attr_cali,         /*show calibration data*/
	//&driver_attr_self,         /*self test demo*/
	//&driver_attr_selftest,     /*self control: 0: disable, 1: enable*/
	&driver_attr_firlen,       /*filter length: 0: disable, others: enable*/
	&driver_attr_trace,        /*trace log*/
	&driver_attr_status,        
	&driver_attr_powerstatus,        
};
/*----------------------------------------------------------------------------*/
static int adxl346_create_attr(struct device_driver *driver) 
{
	int idx, err = 0;
	int num = (int)(sizeof(adxl346_attr_list)/sizeof(adxl346_attr_list[0]));
	if (driver == NULL)
	{
		return -EINVAL;
	}

	for(idx = 0; idx < num; idx++)
	{
		if((err = driver_create_file(driver, adxl346_attr_list[idx])))
		{            
			GSE_ERR("driver_create_file (%s) = %d\n", adxl346_attr_list[idx]->attr.name, err);
			break;
		}
	}    
	return err;
}
/*----------------------------------------------------------------------------*/
static int adxl346_delete_attr(struct device_driver *driver)
{
	int idx ,err = 0;
	int num = (int)(sizeof(adxl346_attr_list)/sizeof(adxl346_attr_list[0]));

	if(driver == NULL)
	{
		return -EINVAL;
	}
	

	for(idx = 0; idx < num; idx++)
	{
		driver_remove_file(driver, adxl346_attr_list[idx]);
	}
	

	return err;
}

/*----------------------------------------------------------------------------*/
int gsensor_operate(void* self, uint32_t command, void* buff_in, int size_in,
		void* buff_out, int size_out, int* actualout)
{
	int err = 0;
	int value, sample_delay;	
	struct adxl346_i2c_data *priv = (struct adxl346_i2c_data*)self;
	hwm_sensor_data* gsensor_data;
	char buff[ADXL346_BUFSIZE];
	
	//GSE_FUN(f);
	switch (command)
	{
		case SENSOR_DELAY:
			if((buff_in == NULL) || (size_in < sizeof(int)))
			{
				GSE_ERR("Set delay parameter error!\n");
				err = -EINVAL;
			}
			else
			{
				value = *(int *)buff_in;
				if(value <= 5)
				{
					sample_delay = ADXL346_BW_200HZ;
				}
				else if(value <= 10)
				{
					sample_delay = ADXL346_BW_100HZ;
				}
				else
				{
					sample_delay = ADXL346_BW_50HZ;
				}
				
				err = ADXL346_SetBWRate(priv->client, sample_delay);
				if(err != ADXL346_SUCCESS ) //0x2C->BW=100Hz
				{
					GSE_ERR("Set delay parameter error!\n");
				}

				if(value >= 50)
				{
					atomic_set(&priv->filter, 0);
				}
				else
				{					
					priv->fir.num = 0;
					priv->fir.idx = 0;
					priv->fir.sum[ADXL346_AXIS_X] = 0;
					priv->fir.sum[ADXL346_AXIS_Y] = 0;
					priv->fir.sum[ADXL346_AXIS_Z] = 0;
					atomic_set(&priv->filter, 1);
				}
			}
			break;

		case SENSOR_ENABLE:
			if((buff_in == NULL) || (size_in < sizeof(int)))
			{
				GSE_ERR("Enable sensor parameter error!\n");
				err = -EINVAL;
			}
			else
			{
				value = *(int *)buff_in;
				if(((value == 0) && (sensor_power == false)) ||((value == 1) && (sensor_power == true)))
				{
					GSE_LOG("Gsensor device have updated!\n");
				}
				else
				{
					err = ADXL346_SetPowerMode( priv->client, !sensor_power);
				}
			}
			break;

		case SENSOR_GET_DATA:
			if((buff_out == NULL) || (size_out< sizeof(hwm_sensor_data)))
			{
				GSE_ERR("get sensor data parameter error!\n");
				err = -EINVAL;
			}
			else
			{
				gsensor_data = (hwm_sensor_data *)buff_out;
				err = ADXL346_ReadSensorData(priv->client, buff, ADXL346_BUFSIZE);
				if(!err)
				{
				   sscanf(buff, "%x %x %x", &gsensor_data->values[0], 
					   &gsensor_data->values[1], &gsensor_data->values[2]);				
				   gsensor_data->status = SENSOR_STATUS_ACCURACY_MEDIUM;				
				   gsensor_data->value_divide = 1000;
				}
			}
			break;
		default:
			GSE_ERR("gsensor operate function no this parameter %d!\n", command);
			err = -1;
			break;
	}
	
	return err;
}

/****************************************************************************** 
 * Function Configuration
******************************************************************************/
static int adxl346_open(struct inode *inode, struct file *file)
{
	file->private_data = adxl346_i2c_client;

	if(file->private_data == NULL)
	{
		GSE_ERR("null pointer!!\n");
		return -EINVAL;
	}
	return nonseekable_open(inode, file);
}
/*----------------------------------------------------------------------------*/
static int adxl346_release(struct inode *inode, struct file *file)
{
	file->private_data = NULL;
	return 0;
}
/*----------------------------------------------------------------------------*/
//static int adxl346_ioctl(struct inode *inode, struct file *file, unsigned int cmd,
//       unsigned long arg)
static long adxl346_unlocked_ioctl(struct file *file, unsigned int cmd,
       unsigned long arg)
{
	struct i2c_client *client = (struct i2c_client*)file->private_data;
	struct adxl346_i2c_data *obj = (struct adxl346_i2c_data*)i2c_get_clientdata(client);	
	char strbuf[ADXL346_BUFSIZE];
	void __user *data;
	SENSOR_DATA sensor_data;
	long err = 0;
	int cali[3];

	//GSE_FUN(f);
	if(_IOC_DIR(cmd) & _IOC_READ)
	{
		err = !access_ok(VERIFY_WRITE, (void __user *)arg, _IOC_SIZE(cmd));
	}
	else if(_IOC_DIR(cmd) & _IOC_WRITE)
	{
		err = !access_ok(VERIFY_READ, (void __user *)arg, _IOC_SIZE(cmd));
	}

	if(err)
	{
		GSE_ERR("access error: %08X, (%2d, %2d)\n", cmd, _IOC_DIR(cmd), _IOC_SIZE(cmd));
		return -EFAULT;
	}

	switch(cmd)
	{
		case GSENSOR_IOCTL_INIT:
			adxl346_init_client(client, 0);			
			break;

		case GSENSOR_IOCTL_READ_CHIPINFO:
			data = (void __user *) arg;
			if(data == NULL)
			{
				err = -EINVAL;
				break;	  
			}
			
			ADXL346_ReadChipInfo(client, strbuf, ADXL346_BUFSIZE);
			if(copy_to_user(data, strbuf, strlen(strbuf)+1))
			{
				err = -EFAULT;
				break;
			}				 
			break;	  

		case GSENSOR_IOCTL_READ_SENSORDATA:
			data = (void __user *) arg;
			if(data == NULL)
			{
				err = -EINVAL;
				break;	  
			}
			
			ADXL346_ReadSensorData(client, strbuf, ADXL346_BUFSIZE);
			if(copy_to_user(data, strbuf, strlen(strbuf)+1))
			{
				err = -EFAULT;
				break;	  
			}				 
			break;

		case GSENSOR_IOCTL_READ_GAIN:
			data = (void __user *) arg;
			if(data == NULL)
			{
				err = -EINVAL;
				break;	  
			}			
			
			if(copy_to_user(data, &gsensor_gain, sizeof(GSENSOR_VECTOR3D)))
			{
				err = -EFAULT;
				break;
			}				 
			break;

		case GSENSOR_IOCTL_READ_RAW_DATA:
			data = (void __user *) arg;
			if(data == NULL)
			{
				err = -EINVAL;
				break;	  
			}
			ADXL346_ReadRawData(client, strbuf);
			if(copy_to_user(data, strbuf, strlen(strbuf)+1))
			{
				err = -EFAULT;
				break;	  
			}
			break;	  

		case GSENSOR_IOCTL_SET_CALI:
			data = (void __user*)arg;
			if(data == NULL)
			{
				err = -EINVAL;
				break;	  
			}
			if(copy_from_user(&sensor_data, data, sizeof(sensor_data)))
			{
				err = -EFAULT;
				break;	  
			}
			if(atomic_read(&obj->suspend))
			{
				GSE_ERR("Perform calibration in suspend state!!\n");
				err = -EINVAL;
			}
			else
			{
				cali[ADXL346_AXIS_X] = sensor_data.x * obj->reso->sensitivity / GRAVITY_EARTH_1000;
				cali[ADXL346_AXIS_Y] = sensor_data.y * obj->reso->sensitivity / GRAVITY_EARTH_1000;
				cali[ADXL346_AXIS_Z] = sensor_data.z * obj->reso->sensitivity / GRAVITY_EARTH_1000;			  
				err = ADXL346_WriteCalibration(client, cali);			 
			}
			break;

		case GSENSOR_IOCTL_CLR_CALI:
			err = ADXL346_ResetCalibration(client);
			break;

		case GSENSOR_IOCTL_GET_CALI:
			data = (void __user*)arg;
			if(data == NULL)
			{
				err = -EINVAL;
				break;	  
			}
			if((err = ADXL346_ReadCalibration(client, cali)))
			{
				break;
			}
			
			sensor_data.x = cali[ADXL346_AXIS_X] * GRAVITY_EARTH_1000 / obj->reso->sensitivity;
			sensor_data.y = cali[ADXL346_AXIS_Y] * GRAVITY_EARTH_1000 / obj->reso->sensitivity;
			sensor_data.z = cali[ADXL346_AXIS_Z] * GRAVITY_EARTH_1000 / obj->reso->sensitivity;
			if(copy_to_user(data, &sensor_data, sizeof(sensor_data)))
			{
				err = -EFAULT;
				break;
			}		
			break;
		

		default:
			GSE_ERR("unknown IOCTL: 0x%08x\n", cmd);
			err = -ENOIOCTLCMD;
			break;
			
	}

	return err;
}


/*----------------------------------------------------------------------------*/
static struct file_operations adxl346_fops = {
//	.owner = THIS_MODULE,
	.open = adxl346_open,
	.release = adxl346_release,
	.unlocked_ioctl = adxl346_unlocked_ioctl,
};
/*----------------------------------------------------------------------------*/
static struct miscdevice adxl346_device = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "gsensor",
	.fops = &adxl346_fops,
};
/*----------------------------------------------------------------------------*/
//#ifndef CONFIG_HAS_EARLYSUSPEND
/*----------------------------------------------------------------------------*/
static int adxl346_suspend(struct i2c_client *client, pm_message_t msg) 
{
	struct adxl346_i2c_data *obj = i2c_get_clientdata(client);    
	int err = 0;
	GSE_FUN();    

	if(msg.event == PM_EVENT_SUSPEND)
	{   
		if(obj == NULL)
		{
			GSE_ERR("null pointer!!\n");
			return -EINVAL;
		}
		atomic_set(&obj->suspend, 1);
		if((err = ADXL346_SetPowerMode(obj->client, false)))
		{
			GSE_ERR("write power control fail!!\n");
			return err;
		}        
		ADXL346_power(obj->hw, 0);
		GSE_LOG("adxl346_suspend ok\n");
	}
	return err;
}
/*----------------------------------------------------------------------------*/
static int adxl346_resume(struct i2c_client *client)
{
	struct adxl346_i2c_data *obj = i2c_get_clientdata(client);        
	int err;
	GSE_FUN();

	if(obj == NULL)
	{
		GSE_ERR("null pointer!!\n");
		return -EINVAL;
	}

	ADXL346_power(obj->hw, 1);
	if((err = adxl346_init_client(client, 0)))
	{
		GSE_ERR("initialize client fail!!\n");
		return err;        
	}
	atomic_set(&obj->suspend, 0);
	GSE_LOG("adxl346_resume ok\n");

	return 0;
}
/*----------------------------------------------------------------------------*/
//#else /*CONFIG_HAS_EARLY_SUSPEND is defined*/
/*----------------------------------------------------------------------------*/
static void adxl346_early_suspend(struct early_suspend *h) 
{
	struct adxl346_i2c_data *obj = container_of(h, struct adxl346_i2c_data, early_drv);   
	int err;
	GSE_FUN();    

	if(obj == NULL)
	{
		GSE_ERR("null pointer!!\n");
		return;
	}
	atomic_set(&obj->suspend, 1); 
	if((err = ADXL346_SetPowerMode(obj->client, false)))
	{
		GSE_ERR("write power control fail!!\n");
		return;
	}

	sensor_power = false;
	
	ADXL346_power(obj->hw, 0);
}
/*----------------------------------------------------------------------------*/
static void adxl346_late_resume(struct early_suspend *h)
{
	struct adxl346_i2c_data *obj = container_of(h, struct adxl346_i2c_data, early_drv);         
	int err;
	GSE_FUN();

	if(obj == NULL)
	{
		GSE_ERR("null pointer!!\n");
		return;
	}

	ADXL346_power(obj->hw, 1);
	if((err = adxl346_init_client(obj->client, 0)))
	{
		GSE_ERR("initialize client fail!!\n");
		return;        
	}
	atomic_set(&obj->suspend, 0);    
}
/*----------------------------------------------------------------------------*/
//#endif /*CONFIG_HAS_EARLYSUSPEND*/
/*----------------------------------------------------------------------------*/

/*
static int adxl346_i2c_detect(struct i2c_client *client, int kind, struct i2c_board_info *info) 
{    
	strcpy(info->type, ADXL345_DEV_NAME);
	return 0;
}
*/

/*----------------------------------------------------------------------------*/
static int adxl346_i2c_probe(struct i2c_client *client, const struct i2c_device_id *id)
{
	struct i2c_client *new_client;
	struct adxl346_i2c_data *obj;
	struct hwmsen_object sobj;
	int err = 0;
	GSE_FUN();

	if(!(obj = kzalloc(sizeof(*obj), GFP_KERNEL)))
	{
		err = -ENOMEM;
		goto exit;
	}
	
	memset(obj, 0, sizeof(struct adxl346_i2c_data));

	obj->hw = get_cust_acc_hw();
	
	if((err = hwmsen_get_convert(obj->hw->direction, &obj->cvt)))
	{
		GSE_ERR("invalid direction: %d\n", obj->hw->direction);
		goto exit;
	}

	obj_i2c_data = obj;
	obj->client = client;
	new_client = obj->client;
	i2c_set_clientdata(new_client,obj);
	
	atomic_set(&obj->trace, 0);
	atomic_set(&obj->suspend, 0);
	
#ifdef CONFIG_ADXL346_LOWPASS
	if(obj->hw->firlen > C_MAX_FIR_LENGTH)
	{
		atomic_set(&obj->firlen, C_MAX_FIR_LENGTH);
	}	
	else
	{
		atomic_set(&obj->firlen, obj->hw->firlen);
	}
	
	if(atomic_read(&obj->firlen) > 0)
	{
		atomic_set(&obj->fir_en, 1);
	}
	
#endif

	adxl346_i2c_client = new_client;	

	if((err = adxl346_init_client(new_client, 1)))
	{
		goto exit_init_failed;
	}
	

	if((err = misc_register(&adxl346_device)))
	{
		GSE_ERR("adxl346_device register failed\n");
		goto exit_misc_device_register_failed;
	}

	if((err = adxl346_create_attr(&adxl346_gsensor_driver.driver)))
	{
		GSE_ERR("create attribute err = %d\n", err);
		goto exit_create_attr_failed;
	}

	sobj.self = obj;
    sobj.polling = 1;
    sobj.sensor_operate = gsensor_operate;
	if((err = hwmsen_attach(ID_ACCELEROMETER, &sobj)))
	{
		GSE_ERR("attach fail = %d\n", err);
		goto exit_kfree;
	}

#ifdef CONFIG_HAS_EARLYSUSPEND
	obj->early_drv.level    = EARLY_SUSPEND_LEVEL_DISABLE_FB - 1,
	obj->early_drv.suspend  = adxl346_early_suspend,
	obj->early_drv.resume   = adxl346_late_resume,    
	register_early_suspend(&obj->early_drv);
#endif 

	GSE_LOG("%s: OK\n", __func__);    
	return 0;

	exit_create_attr_failed:
	misc_deregister(&adxl346_device);
	exit_misc_device_register_failed:
	exit_init_failed:
	//i2c_detach_client(new_client);
	exit_kfree:
	kfree(obj);
	exit:
	GSE_ERR("%s: err = %d\n", __func__, err);        
	return err;
}

/*----------------------------------------------------------------------------*/
static int adxl346_i2c_remove(struct i2c_client *client)
{
	int err = 0;	
	
	if((err = adxl346_delete_attr(&adxl346_gsensor_driver.driver)))
	{
		GSE_ERR("adxl346_delete_attr fail: %d\n", err);
	}
	
	if((err = misc_deregister(&adxl346_device)))
	{
		GSE_ERR("misc_deregister fail: %d\n", err);
	}

	if((err = hwmsen_detach(ID_ACCELEROMETER)))
	    

	adxl346_i2c_client = NULL;
	i2c_unregister_device(client);
	kfree(i2c_get_clientdata(client));
	return 0;
}
/*----------------------------------------------------------------------------*/
static int adxl346_probe(struct platform_device *pdev) 
{
	struct acc_hw *hw = get_cust_acc_hw();
	GSE_FUN();

	ADXL346_power(hw, 1);
	//adxl346_force[0] = hw->i2c_num;
	if(i2c_add_driver(&adxl346_i2c_driver))
	{
		GSE_ERR("add driver error\n");
		return -1;
	}
	return 0;
}
/*----------------------------------------------------------------------------*/
static int adxl346_remove(struct platform_device *pdev)
{
    struct acc_hw *hw = get_cust_acc_hw();

    GSE_FUN();    
    ADXL346_power(hw, 0);    
    i2c_del_driver(&adxl346_i2c_driver);
    return 0;
}
/*----------------------------------------------------------------------------*/
static struct platform_driver adxl346_gsensor_driver = {
	.probe      = adxl346_probe,
	.remove     = adxl346_remove,    
	.driver     = {
		.name  = "gsensor",
//		.owner = THIS_MODULE,
	}
};

/*----------------------------------------------------------------------------*/
static int __init adxl346_init(void)
{
	GSE_FUN();
	i2c_register_board_info(0, &i2c_adxl346, 1);
	if(platform_driver_register(&adxl346_gsensor_driver))
	{
		GSE_ERR("failed to register driver");
		return -ENODEV;
	}
	return 0;    
}
/*----------------------------------------------------------------------------*/
static void __exit adxl346_exit(void)
{
	GSE_FUN();
	platform_driver_unregister(&adxl346_gsensor_driver);
}
/*----------------------------------------------------------------------------*/
module_init(adxl346_init);
module_exit(adxl346_exit);
/*----------------------------------------------------------------------------*/
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("ADXL346 I2C driver");
MODULE_AUTHOR("Chunlei.Wang@mediatek.com");
