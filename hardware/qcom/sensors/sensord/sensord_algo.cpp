/**
 * @section LICENSE
 *
 * (C) Copyright 2011~2015 Bosch Sensortec GmbH All Rights Reserved
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 *------------------------------------------------------------------------------
 *  Disclaimer
 *
 * Common: Bosch Sensortec products are developed for the consumer goods
 * industry. They may only be used within the parameters of the respective valid
 * product data sheet.  Bosch Sensortec products are provided with the express
 * understanding that there is no warranty of fitness for a particular purpose.
 * They are not fit for use in life-sustaining, safety or security sensitive
 * systems or any system or device that may lead to bodily harm or property
 * damage if the system or device malfunctions. In addition, Bosch Sensortec
 * products are not fit for use in products which interact with motor vehicle
 * systems.  The resale and/or use of products are at the purchaser's own risk
 * and his own responsibility. The examination of fitness for the intended use
 * is the sole responsibility of the Purchaser.
 *
 * The purchaser shall indemnify Bosch Sensortec from all third party claims,
 * including any claims for incidental, or consequential damages, arising from
 * any product use not covered by the parameters of the respective valid product
 * data sheet or not approved by Bosch Sensortec and reimburse Bosch Sensortec
 * for all costs in connection with such claims.
 *
 * The purchaser must monitor the market for the purchased products,
 * particularly with regard to product safety and inform Bosch Sensortec without
 * delay of all security relevant incidents.
 *
 * Engineering Samples are marked with an asterisk (*) or (e). Samples may vary
 * from the valid technical specifications of the product series. They are
 * therefore not intended or fit for resale to third parties or for use in end
 * products. Their sole purpose is internal client testing. The testing of an
 * engineering sample may in no way replace the testing of a product series.
 * Bosch Sensortec assumes no liability for the use of engineering samples. By
 * accepting the engineering samples, the Purchaser agrees to indemnify Bosch
 * Sensortec from all claims arising from the use of engineering samples.
 *
 * Special: This software module (hereinafter called "Software") and any
 * information on application-sheets (hereinafter called "Information") is
 * provided free of charge for the sole purpose to support your application
 * work. The Software and Information is subject to the following terms and
 * conditions:
 *
 * The Software is specifically designed for the exclusive use for Bosch
 * Sensortec products by personnel who have special experience and training. Do
 * not use this Software if you do not have the proper experience or training.
 *
 * This Software package is provided `` as is `` and without any expressed or
 * implied warranties, including without limitation, the implied warranties of
 * merchantability and fitness for a particular purpose.
 *
 * Bosch Sensortec and their representatives and agents deny any liability for
 * the functional impairment of this Software in terms of fitness, performance
 * and safety. Bosch Sensortec and their representatives and agents shall not be
 * liable for any direct or indirect damages or injury, except as otherwise
 * stipulated in mandatory applicable law.
 *
 * The Information provided is believed to be accurate and reliable. Bosch
 * Sensortec assumes no responsibility for the consequences of use of such
 * Information nor for any infringement of patents or other rights of third
 * parties which may result from its use.
 *
 * @file         sensord_algo.cpp
 * @date         "Fri Feb 5 15:40:38 2016 +0800"
 * @commit       "666efb6"
 *
 * @brief
 *
 * @detail
 *
 */

#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <ctype.h>
#include <sys/types.h>
#include <stdarg.h>

#include "BstSensor.h"

#include "sensord_pltf.h"
#include "sensord_cfg.h"
#include "sensord_algo.h"
#include "sensord_hwcntl.h"
#include "util_misc.h"


#define CONVERT_ACC (0.0098) //library output is in mg = 0.0098 m/s^2
#define CONVERT_GYRO (0.000066322) //125deg/Second Full scale configuration
#define CONVERT_MAG (0.1)
#define CONVERT_ORI (57.2958)

#define HAS_ACC 0x1
#define HAS_MAG 0x2
#define HAS_GYR 0x4

typedef struct
{
    bsx_s32_t x;
    bsx_s32_t y;
    bsx_s32_t z;
    bsx_ts_external_t t;
} BSX_DATALOG_BUF;


/*!
 * @brief This function loads spec files from file system and set them into
 *            bsx_init, after that it calls algo_adapter_init to set up
 *            sensor_hw and working modes
 *
 * @param none
 *
 * @return 0 success, < 0 failed
 */
int sensord_bsx_init(void)
{
    return 0;
}



//declare i/p buffer
/// accel data
bsx_data_content_t accel_sli_in_xyz[3] =
{
    {   .slli = 0},
    {   .slli = 0},
    {   .slli = 0}};

bsx_fifo_data_t accel_in_data =
        {
                0, /* timestamp */
                accel_sli_in_xyz,
                #if 1
                BSX_BUFFER_TYPE_S32, /* data type */
#else
                BSX_BUFFER_TYPE_FLONG, /* data type */
#endif
                BSX_INPUT_ID_ACCELERATION,
                3, /* dims */
                1, /* depth */
        };

/// mag test data
bsx_data_content_t mag_sli_in_xyz[3] =
{
    {   .slli = 0},
    {   .slli = 0},
    {   .slli = 0}};

bsx_fifo_data_t mag_in_data =
        {
                0, /* timestamp */
                mag_sli_in_xyz,
                #if 1
                BSX_BUFFER_TYPE_S32, /* data type */
#else
                BSX_BUFFER_TYPE_FLONG, /* data type */
#endif
                BSX_INPUT_ID_MAGNETICFIELD,
                3, /* dims */
                1, /* depth */
        };

/// ang test data
bsx_data_content_t ang_sli_in_xyz[3] =
{
    {   .slli = 0},
    {   .slli = 0},
    {   .slli = 0}};

bsx_fifo_data_t ang_in_data =
        {
                0, /* timestamp */
                ang_sli_in_xyz,
                #if 1
                BSX_BUFFER_TYPE_S32, /* data type */
#else
                BSX_BUFFER_TYPE_FLONG, /* data type */
#endif
                BSX_INPUT_ID_ANGULARRATE,
                3, /* dims */
                1, /* depth */

        };

/// library input package
static bsx_fifo_data_t library_in_package[3];


/**
 * generate an align indication array @param p_align_ind XX XX XX XX... with length @param p_len
 * if @param p_align_ind [i] & HAS_ACC/HAS_MAG/HAS_GYR, then a acc/mag/gyro should be delivered
 * to libary at that postion.
 */
static void sort_input_samples(int8_t **pp_align_ind, uint32_t *p_len,
        HW_DATA_UNION **pp_ACC_hwdata, uint32_t ACC_hwdata_len,
        HW_DATA_UNION **pp_MAG_hwdata, uint32_t MAG_hwdata_len,
        HW_DATA_UNION **pp_GYRO_hwdata, uint32_t GYRO_hwdata_len)
{
    int64_t cur_base_tm = 0;
    int acc_cur_index = 0;
    int mag_cur_index = 0;
    int gyr_cur_index = 0;
    int pos = 0;

    if(0 == ACC_hwdata_len + MAG_hwdata_len + GYRO_hwdata_len)
    {
        *p_len = 0;
        return;
    }

    (*pp_align_ind) = (int8_t *)calloc(ACC_hwdata_len + MAG_hwdata_len + GYRO_hwdata_len, 1);
    if(NULL == (*pp_align_ind))
    {
        *p_len = 0;
        PERR("calloc fail");
        return;
    }

    while(ACC_hwdata_len + MAG_hwdata_len + GYRO_hwdata_len)
    {
        /** firstly find out the current tm base as the alignment standard*/
        cur_base_tm = 0;

        if(GYRO_hwdata_len){
            cur_base_tm = pp_GYRO_hwdata[gyr_cur_index]->timestamp;
        }

        if(ACC_hwdata_len)
        {
            if(cur_base_tm)
            {
                if(pp_ACC_hwdata[acc_cur_index]->timestamp < cur_base_tm){
                    cur_base_tm = pp_ACC_hwdata[acc_cur_index]->timestamp;
                }
            }else{
                cur_base_tm = pp_ACC_hwdata[acc_cur_index]->timestamp;
            }
        }

        if(MAG_hwdata_len)
        {
            if(cur_base_tm)
            {
                if(pp_MAG_hwdata[mag_cur_index]->timestamp < cur_base_tm){
                    cur_base_tm = pp_MAG_hwdata[mag_cur_index]->timestamp;
                }
            }else{
                cur_base_tm = pp_MAG_hwdata[mag_cur_index]->timestamp;
            }
        }

        /**Then the raw data can be aligned according to the currect tm base*/
        if(ACC_hwdata_len)
        {
            if(pp_ACC_hwdata[acc_cur_index]->timestamp == cur_base_tm)
            {
                (*pp_align_ind)[pos] |= HAS_ACC;
                acc_cur_index++;
                ACC_hwdata_len--;
            }
        }

        if(MAG_hwdata_len)
        {
            if(pp_MAG_hwdata[mag_cur_index]->timestamp == cur_base_tm)
            {
                (*pp_align_ind)[pos] |= HAS_MAG;
                mag_cur_index++;
                MAG_hwdata_len--;
            }
        }

        if(GYRO_hwdata_len)
        {
            if(pp_GYRO_hwdata[gyr_cur_index]->timestamp == cur_base_tm)
            {
                (*pp_align_ind)[pos] |= HAS_GYR;
                gyr_cur_index++;
                GYRO_hwdata_len--;
            }
        }

        pos++;
    }

    *p_len = pos;

    return;
}

/**
 * free thoroughly the hwdata
 */
static void distory_hwdata(HW_DATA_UNION **pp_hwdata, uint32_t hwdata_len)
{
    uint32_t i;

    if(NULL == pp_hwdata)
    {
        return;
    }

    for (i = 0; i < hwdata_len; ++i) {
        free(pp_hwdata[i]);
    }

    free(pp_hwdata);
}


void sensord_algo_process(BstSensor *bstsensor)
{
    uint32_t i;
    uint32_t j;
    HW_DATA_UNION **pp_ACC_hwdata = NULL;
    uint32_t ACC_hwdata_len = 0;
    HW_DATA_UNION **pp_MAG_hwdata = NULL;
    uint32_t MAG_hwdata_len = 0;
    HW_DATA_UNION **pp_GYRO_hwdata = NULL;
    uint32_t GYRO_hwdata_len = 0;
    int8_t *p_align_ind = NULL;
    uint32_t align_ind_len;
    int32_t ACC_hwdata_index;
    int32_t MAG_hwdata_index;
    int32_t GYRO_hwdata_index;

    char data_log_buf[256] = { 0 };
    uint32_t acc_has_input = 0;
    uint32_t mag_has_input = 0;
    uint32_t gyr_has_input = 0;
    uint32_t input_package_index = 0;
    sensors_event_t *p_event = NULL;
    BSX_DATALOG_BUF acc_log_data;
    BSX_DATALOG_BUF gyr_log_data;
    BSX_DATALOG_BUF mag_log_data;

    if(0 == bstsensor->tmplist_sensord_acclraw->list_len +
            bstsensor->tmplist_sensord_gyroraw->list_len +
            bstsensor->tmplist_sensord_magnraw->list_len)
    {
        return;
    }

    /**
     * Step 1 save the hardware data and generate align indication array
     */
    if(bstsensor->tmplist_sensord_acclraw->list_len)
    {
        pp_ACC_hwdata = (HW_DATA_UNION **)malloc(bstsensor->tmplist_sensord_acclraw->list_len * sizeof(HW_DATA_UNION *));
        if(NULL == pp_ACC_hwdata)
        {
            PWARN("malloc fail");
            return;
        }

        ACC_hwdata_len = bstsensor->tmplist_sensord_acclraw->list_len;
        for (i = 0; i < ACC_hwdata_len; ++i) {
            bstsensor->tmplist_sensord_acclraw->list_get_headdata((void **) &(pp_ACC_hwdata[i]));
        }
    }

    if(bstsensor->tmplist_sensord_magnraw->list_len)
    {
        pp_MAG_hwdata = (HW_DATA_UNION **)malloc(bstsensor->tmplist_sensord_magnraw->list_len * sizeof(HW_DATA_UNION *));
        if(NULL == pp_MAG_hwdata)
        {
            PWARN("malloc fail");
            distory_hwdata(pp_ACC_hwdata, ACC_hwdata_len);
            return;
        }

        MAG_hwdata_len = bstsensor->tmplist_sensord_magnraw->list_len;
        for (i = 0; i < MAG_hwdata_len; ++i) {
            bstsensor->tmplist_sensord_magnraw->list_get_headdata((void **) &(pp_MAG_hwdata[i]));
        }
    }

    if(bstsensor->tmplist_sensord_gyroraw->list_len)
    {
        pp_GYRO_hwdata = (HW_DATA_UNION **)malloc(bstsensor->tmplist_sensord_gyroraw->list_len * sizeof(HW_DATA_UNION *));
        if(NULL == pp_GYRO_hwdata)
        {
            PWARN("malloc fail");
            distory_hwdata(pp_ACC_hwdata, ACC_hwdata_len);
            distory_hwdata(pp_MAG_hwdata, MAG_hwdata_len);
            return;
        }

        GYRO_hwdata_len = bstsensor->tmplist_sensord_gyroraw->list_len;
        for (i = 0; i < GYRO_hwdata_len; ++i) {
            bstsensor->tmplist_sensord_gyroraw->list_get_headdata((void **) &(pp_GYRO_hwdata[i]));
        }
    }

    //PDEBUG("Acc len: %u, Gyro len: %u, Mag len: %u", simple_listAccl->list_len, simple_listGyro->list_len, simple_listMagn->list_len);

    sort_input_samples(&p_align_ind, &align_ind_len,
            pp_ACC_hwdata, ACC_hwdata_len,
            pp_MAG_hwdata, MAG_hwdata_len,
            pp_GYRO_hwdata, GYRO_hwdata_len);
    if(NULL == p_align_ind)
    {
        PWARN("sort_input_samples fail");
        distory_hwdata(pp_ACC_hwdata, ACC_hwdata_len);
        distory_hwdata(pp_MAG_hwdata, MAG_hwdata_len);
        distory_hwdata(pp_GYRO_hwdata, GYRO_hwdata_len);
        return;
    }

    /**
     * Step 2 deliver hardware data to library according to align indication array
     */
    ACC_hwdata_index = 0;
    MAG_hwdata_index = 0;
    GYRO_hwdata_index = 0;

    for (i = 0; i < align_ind_len; ++i)
    {
        acc_has_input = 0;
        gyr_has_input = 0;
        mag_has_input = 0;

        if(p_align_ind[i] & HAS_ACC)
        {
            acc_has_input = 1;
            accel_sli_in_xyz[0].lw.mslw.sli = pp_ACC_hwdata[ACC_hwdata_index]->x;
            accel_sli_in_xyz[1].lw.mslw.sli = pp_ACC_hwdata[ACC_hwdata_index]->y;
            accel_sli_in_xyz[2].lw.mslw.sli = pp_ACC_hwdata[ACC_hwdata_index]->z;
            accel_in_data.time_stamp = (bsx_ts_external_t) (pp_ACC_hwdata[ACC_hwdata_index]->timestamp);
            accel_in_data.sensor_id = BSX_INPUT_ID_ACCELERATION;
            ACC_hwdata_index++;
        }

        if(p_align_ind[i] & HAS_MAG)
        {
            mag_has_input = 1;
            mag_sli_in_xyz[0].lw.mslw.sli = pp_MAG_hwdata[MAG_hwdata_index]->x;
            mag_sli_in_xyz[1].lw.mslw.sli = pp_MAG_hwdata[MAG_hwdata_index]->y;
            mag_sli_in_xyz[2].lw.mslw.sli = pp_MAG_hwdata[MAG_hwdata_index]->z;
            mag_in_data.time_stamp = (bsx_ts_external_t) (pp_MAG_hwdata[MAG_hwdata_index]->timestamp);
            mag_in_data.sensor_id = BSX_INPUT_ID_MAGNETICFIELD;
            MAG_hwdata_index++;
        }

        if(p_align_ind[i] & HAS_GYR)
        {
            gyr_has_input = 1;
            ang_sli_in_xyz[0].lw.mslw.sli = pp_GYRO_hwdata[GYRO_hwdata_index]->x;
            ang_sli_in_xyz[1].lw.mslw.sli = pp_GYRO_hwdata[GYRO_hwdata_index]->y;
            ang_sli_in_xyz[2].lw.mslw.sli = pp_GYRO_hwdata[GYRO_hwdata_index]->z;
            ang_in_data.time_stamp = (bsx_ts_external_t) (pp_GYRO_hwdata[GYRO_hwdata_index]->timestamp);
            ang_in_data.sensor_id = BSX_INPUT_ID_ANGULARRATE;
            GYRO_hwdata_index++;
        }

        input_package_index = 0;

        if (data_log){
            memset(&acc_log_data, 0, sizeof(BSX_DATALOG_BUF));
            memset(&gyr_log_data, 0, sizeof(BSX_DATALOG_BUF));
            memset(&mag_log_data, 0, sizeof(BSX_DATALOG_BUF));
        }

        if(acc_has_input){
            library_in_package[input_package_index++] = accel_in_data;
            PINFO("input ACC data: id=%u, D=%d, %d, %d T=%lld",
                    accel_in_data.sensor_id,
                    accel_in_data.content_p[0].lw.mslw.sli,
                    accel_in_data.content_p[1].lw.mslw.sli,
                    accel_in_data.content_p[2].lw.mslw.sli,
                    accel_in_data.time_stamp);

            if(data_log){
                acc_log_data.x = accel_in_data.content_p[0].lw.mslw.sli;
                acc_log_data.y = accel_in_data.content_p[1].lw.mslw.sli;
                acc_log_data.z = accel_in_data.content_p[2].lw.mslw.sli;
                acc_log_data.t = accel_in_data.time_stamp;
            }
            if(bsx_datalog){
                sprintf(data_log_buf, "%lld,\t%u,\t%d, %d, %d,\t%lld\n",
                        (bsx_s64_t)GET_TIME_TICK(),
                        accel_in_data.sensor_id,
                        accel_in_data.content_p[0].lw.mslw.sli,
                        accel_in_data.content_p[1].lw.mslw.sli,
                        accel_in_data.content_p[2].lw.mslw.sli,
                        accel_in_data.time_stamp);
                bsx_datalog_algo(data_log_buf);
            }
        }

        if(mag_has_input){
            library_in_package[input_package_index++] = mag_in_data;
            PINFO("input MAG data: id=%u, D=%d, %d, %d T=%lld",
                    mag_in_data.sensor_id,
                    mag_in_data.content_p[0].lw.mslw.sli,
                    mag_in_data.content_p[1].lw.mslw.sli,
                    mag_in_data.content_p[2].lw.mslw.sli,
                    mag_in_data.time_stamp);

            if(data_log){
                mag_log_data.x = mag_in_data.content_p[0].lw.mslw.sli;
                mag_log_data.y = mag_in_data.content_p[1].lw.mslw.sli;
                mag_log_data.z = mag_in_data.content_p[2].lw.mslw.sli;
                mag_log_data.t = mag_in_data.time_stamp;
            }
            if(bsx_datalog){
                sprintf(data_log_buf, "%lld,\t%u,\t%d, %d, %d,\t%lld\n",
                        (bsx_s64_t)GET_TIME_TICK(),
                        mag_in_data.sensor_id,
                        mag_in_data.content_p[0].lw.mslw.sli,
                        mag_in_data.content_p[1].lw.mslw.sli,
                        mag_in_data.content_p[2].lw.mslw.sli,
                        mag_in_data.time_stamp);
                bsx_datalog_algo(data_log_buf);
            }
        }

        if(gyr_has_input){
            library_in_package[input_package_index++] = ang_in_data;
            PINFO("input GYRO data: id=%u, D=%d, %d, %d T=%lld",
                    ang_in_data.sensor_id,
                    ang_in_data.content_p[0].lw.mslw.sli,
                    ang_in_data.content_p[1].lw.mslw.sli,
                    ang_in_data.content_p[2].lw.mslw.sli,
                    ang_in_data.time_stamp);

            if(data_log){
                gyr_log_data.x = ang_in_data.content_p[0].lw.mslw.sli;
                gyr_log_data.y = ang_in_data.content_p[1].lw.mslw.sli;
                gyr_log_data.z = ang_in_data.content_p[2].lw.mslw.sli;
                gyr_log_data.t = ang_in_data.time_stamp;
            }
            if(bsx_datalog){
                sprintf(data_log_buf, "%lld,\t%u,\t%d, %d, %d,\t%lld\n",
                        (bsx_s64_t)GET_TIME_TICK(),
                        ang_in_data.sensor_id,
                        ang_in_data.content_p[0].lw.mslw.sli,
                        ang_in_data.content_p[1].lw.mslw.sli,
                        ang_in_data.content_p[2].lw.mslw.sli,
                        ang_in_data.time_stamp);
                bsx_datalog_algo(data_log_buf);
            }
        }

        if (data_log)
        {
            sprintf(data_log_buf, "%d, %d, %d, %lld,\t %d, %d, %d, %lld,\t %d, %d, %d, %lld\n",
                    acc_log_data.x, acc_log_data.y, acc_log_data.z, acc_log_data.t,
                    mag_log_data.x, mag_log_data.y, mag_log_data.z, mag_log_data.t,
                    gyr_log_data.x, gyr_log_data.y, gyr_log_data.z, gyr_log_data.t);
            data_log_algo_input(data_log_buf);
        }


        {
            for (j = 0; j < input_package_index; ++j)
            {
                p_event = (sensors_event_t *) calloc(1, sizeof(sensors_event_t));
                if (NULL == p_event)
                {
                    PWARN("calloc fail");
                    continue;
                }

                p_event->version = sizeof(sensors_event_t);
                p_event->timestamp = library_in_package[j].time_stamp;

                switch(library_in_package[j].sensor_id)
                {
                    case BSX_INPUT_ID_ACCELERATION:
                        p_event->sensor = BSX_SENSOR_ID_ACCELEROMETER;
                        p_event->type = SENSOR_TYPE_ACCELEROMETER;
                        p_event->acceleration.x = library_in_package[j].content_p[0].lw.mslw.sli * CONVERT_ACC;
                        p_event->acceleration.y = library_in_package[j].content_p[1].lw.mslw.sli * CONVERT_ACC;
                        p_event->acceleration.z = library_in_package[j].content_p[2].lw.mslw.sli * CONVERT_ACC;
                        p_event->acceleration.status = 0;
                        break;
                    case BSX_INPUT_ID_MAGNETICFIELD:
                        p_event->sensor = BSX_SENSOR_ID_MAGNETIC_FIELD_UNCALIBRATED;
                        p_event->type = SENSOR_TYPE_MAGNETIC_FIELD_UNCALIBRATED;
                        p_event->uncalibrated_magnetic.x_uncalib = library_in_package[j].content_p[0].lw.mslw.sli * CONVERT_MAG;
                        p_event->uncalibrated_magnetic.y_uncalib = library_in_package[j].content_p[1].lw.mslw.sli * CONVERT_MAG;
                        p_event->uncalibrated_magnetic.z_uncalib = library_in_package[j].content_p[2].lw.mslw.sli * CONVERT_MAG;
                        break;
                    case BSX_INPUT_ID_ANGULARRATE:
                        p_event->sensor = BSX_SENSOR_ID_GYROSCOPE_UNCALIBRATED;
                        p_event->type = SENSOR_TYPE_GYROSCOPE_UNCALIBRATED;
                        p_event->uncalibrated_gyro.x_uncalib = library_in_package[j].content_p[0].lw.mslw.sli * CONVERT_GYRO;
                        p_event->uncalibrated_gyro.y_uncalib = library_in_package[j].content_p[1].lw.mslw.sli * CONVERT_GYRO;
                        p_event->uncalibrated_gyro.z_uncalib = library_in_package[j].content_p[2].lw.mslw.sli * CONVERT_GYRO;
                        break;
                    default:
                        PERR("impossible bsx_distribute_id: %d", library_in_package[j].sensor_id);
                        free(p_event);
                        continue;
                }

                bstsensor->sensord_deliver_event(p_event);
            }
        }
    }

    distory_hwdata(pp_ACC_hwdata, ACC_hwdata_len);
    distory_hwdata(pp_MAG_hwdata, MAG_hwdata_len);
    distory_hwdata(pp_GYRO_hwdata, GYRO_hwdata_len);
    free(p_align_ind);

    return;
}


uint8_t sensord_resample5to4(int32_t data[3], int64_t *tm,  int32_t pre_data[3], int64_t *pre_tm, uint32_t counter)
{
  uint8_t statusBit;
  int32_t x_in_tmp, y_in_tmp, z_in_tmp;
  int64_t tm_in_tmp;
  uint32_t sampleNumber;
  bool guard1 = false;
  int32_t  x0, y0, z0;
  int32_t  x1, y1, z1;
  int64_t t0;
  int64_t t1;

  x_in_tmp = data[0];
  y_in_tmp = data[1];
  z_in_tmp = data[2];
  tm_in_tmp = tm[0];

  sampleNumber = counter % 5U;
  guard1 = false;

  switch (sampleNumber) {
   case 0U:
    statusBit = 1;
    data[0] = pre_data[0];
    data[1] = pre_data[1];
    data[2] = pre_data[2];
    tm[0] = pre_tm[0];
    break;

   case 1U:
    guard1 = true;
    break;

   case 2U:
    guard1 = true;
    break;

   case 3U:
    guard1 = true;
    break;

   case 4U:
    statusBit = 0;
    break;
  }

  if (guard1)
  {
    statusBit = 1;
    x0 = (pre_data[0] < 0x7FFFFFFD ? pre_data[0] : 0x7FFFFFFD);
    y0 = (pre_data[1] < 0x7FFFFFFD ? pre_data[1] : 0x7FFFFFFD);
    z0 = (pre_data[2] < 0x7FFFFFFD ? pre_data[2] : 0x7FFFFFFD);
    x1 = (data[0] < 0x7FFFFFFD ? data[0] : 0x7FFFFFFD);
    y1 = (data[1] < 0x7FFFFFFD ? data[1] : 0x7FFFFFFD);
    z1 = (data[2] < 0x7FFFFFFD ? data[2] : 0x7FFFFFFD);

    data[0] = (4 - (int32_t)sampleNumber) * ((x0 + 2) >> 2) + (int32_t)sampleNumber * ((x1 + 2) >> 2);
    data[1] = (4 - (int32_t)sampleNumber) * ((y0 + 2) >> 2) + (int32_t)sampleNumber * ((y1 + 2) >> 2);
    data[2] = (4 - (int32_t)sampleNumber) * ((z0 + 2) >> 2) + (int32_t)sampleNumber * ((z1 + 2) >> 2);

    t0 = (pre_tm[0] < 0x7FFFFFFFFFFFFFFD ? pre_tm[0] : 0x7FFFFFFFFFFFFFFD);
    t1 = (tm[0] < 0x7FFFFFFFFFFFFFFD ? tm[0] : 0x7FFFFFFFFFFFFFFD);

    tm[0] = (4LL - sampleNumber) * ((t0 + 2LL) >> 2LL) + sampleNumber * ((t1 +2LL) >> 2LL);
  }

  pre_data[0] = x_in_tmp;
  pre_data[1] = y_in_tmp;
  pre_data[2] = z_in_tmp;
  pre_tm[0] = tm_in_tmp;

  return statusBit;
}


#ifdef __cplusplus
extern "C"
{
#endif

void ladon_assert(int condition, char *file_name, char *function_name, int line_number, char *format_string_p, ...)
{
    if (!condition)
    {
        char message_buffer[1024];
        va_list variable_argument_list_p;

        va_start(variable_argument_list_p, format_string_p);
        vsprintf(message_buffer, format_string_p, variable_argument_list_p);
        va_end(variable_argument_list_p);

        PLADON("Assertion failed in file [%s]\n at function %s on line %d with message:\n\"%s\"",
               file_name, function_name, line_number, message_buffer);
    }
}

void ladon_warning(char *file_name, char *function_name, int line_number, char *format_string_p, ...)
{
    char message_buffer[1024];
    va_list variable_argument_list_p;

    va_start(variable_argument_list_p, format_string_p);
    vsprintf(message_buffer, format_string_p, variable_argument_list_p);
    va_end(variable_argument_list_p);

    PLADON("Warning issued in file [%s]\n at function %s on line %d with message:\n\"%s\"",
           file_name, function_name, line_number, message_buffer);
}

#ifdef __cplusplus
}
#endif

