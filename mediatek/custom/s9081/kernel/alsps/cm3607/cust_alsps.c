
#include <linux/types.h>
#include <cust_alsps.h>
#ifdef MT6575
#include <mach/mt6575_pm_ldo.h>
#endif
#ifdef MT6577
#include <mach/mt6577_pm_ldo.h>
#endif

static struct alsps_hw cust_alsps_hw = {
    .i2c_num    = 0,
    .polling_mode =1,
    .power_id   = MT65XX_POWER_NONE,    /*LDO is not used*/
    .power_vol  = VOL_DEFAULT,          /*LDO is not used*/
    .i2c_addr   = {0x0C, 0x48, 0x78, 0x00},
//    .als_level  = { 0,  1,  1,   7,  15,  15,  100, 1000, 2000,  3000,  6000, 10000, 14000, 18000, 20000},
//    .als_value  = {40, 40, 90,  90, 160, 160,  225,  320,  640,  1280,  1280,  2600,  2600, 2600,  10240, 10240},
    .als_level  = { 0,  13,  16,   19,   22,   25,   28,  31,  34,   37,   40,  43,  46, 49,  52, 55},
    .als_value  = {1, 11, 19,  32, 56, 96,  167,  289,  499,  864,  1459,  2586,  4475, 7743,  13396, 23178},    
    .ps_threshold = 3,
};
struct alsps_hw *get_cust_alsps_hw(void) {
    return &cust_alsps_hw;
}
