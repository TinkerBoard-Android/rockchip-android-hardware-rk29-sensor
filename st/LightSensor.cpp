/*
 * Copyright (C) 2008 The Android Open Source Project
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
 */

#include <fcntl.h>
#include <errno.h>
#include <math.h>
#include <poll.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/select.h>
#include <cutils/log.h>
#include <stdio.h>

#include "LightSensor.h"

#define ALS_CAL_FILE "/sys/bus/i2c/devices/i2c-5/5-0029/cal"
#define ALS_CAL_SYSFS "/sys/bus/i2c/devices/i2c-5/5-0029/calibration"

/*****************************************************************************/

LightSensor::LightSensor()
    : SensorBase(NULL, "cm32183-ls"),
      mEnabled(0),
      mInputReader(4),
      mHasPendingEvent(false)
{
    mPendingEvent.version = sizeof(sensors_event_t);
    mPendingEvent.sensor = ID_L;
    mPendingEvent.type = SENSOR_TYPE_LIGHT;
    memset(mPendingEvent.data, 0, sizeof(mPendingEvent.data));
    als_val=0;
    white_val=0;

    if (data_fd) {
        strcat(input_sysfs_path, "/sys/bus/i2c/devices/i2c-5/5-0029/");
        input_sysfs_path_len = strlen(input_sysfs_path);
        enable(0, 1);
    }

    if (loadCaliVal())
        ALOGD("Load calibration data fail");
    else
        ALOGD("Load calibration data success");
}

LightSensor::~LightSensor() {
    if (mEnabled) {
        enable(0, 0);
    }
}

int LightSensor::loadCaliVal()
{
    int err, ret = 0;
    int cali_factor = 65535;
    int cali_value[5] = {0xFF};
    int cali_factor_check = 0;

    FILE *in_fd, *out_fd;

    out_fd = fopen(ALS_CAL_SYSFS, "w");
    if (out_fd == NULL) {
        err = errno;
        ALOGD("open calibration sysfs %s FAIL: %s", ALS_CAL_SYSFS, strerror(err));
        return -1;
    } else {
        if (out_fd)
            fclose(out_fd);
    }

    in_fd = fopen(ALS_CAL_FILE, "r");
    if (in_fd == NULL) {
        err = errno;
        ALOGD("open calibration data FAIL: %s", strerror(err));
    } else if (5 != (ret = fscanf(in_fd, "%d %d %d %d %d",  &cali_value[0], \
            &cali_value[1], &cali_value[2], &cali_value[3], &cali_value[4]))) {
        err = errno;
        ALOGD("reading calibration data %s: errno: %d, "   \
                "possible meaning: %s\n", ALS_CAL_FILE, err,    \
                strerror(err));
    }

    int fd;
    char buf[20];

    cali_factor = cali_value[3] << 24 | cali_value[2] << 16 | \
            cali_value[1] << 8 | cali_value[0];
    ALOGD("cali_factor is %d", cali_factor);
    cali_factor_check = (cali_value[3] | cali_value[2] | \
            cali_value[1]  | cali_value[0]);
    ALOGD("cali_factor_check %d", cali_factor_check);
    if (cali_factor_check != cali_value[4]) {
        ALOGD("No valid calibration data");
        return -1;
    }

    sprintf(buf, "%d %s", cali_factor, "-setcv");
    fd = open(ALS_CAL_SYSFS, O_RDWR);
    if (fd < 0) {
        err = errno;
        ALOGD("Error writing calibration data %s: errno: %d, "   \
                "possible meaning: %s\n", ALS_CAL_SYSFS, err,   \
                 strerror(err));
        ret = -1;
    } else {
        write(fd, buf, sizeof(buf));
        close(fd);
        ALOGD("Success loading calibration from %s into %s "     \
                "with value %d", ALS_CAL_FILE,  \
                ALS_CAL_SYSFS, cali_factor);
        ret = 0;
    }

    if (in_fd)
        fclose(in_fd);

    return ret;
}

int LightSensor::setDelay(int32_t handle, int64_t ns)
{
    return 0;
}

int LightSensor::enable(int32_t handle, int en)
{
    int flags = en ? 1 : 0;
    if (flags != mEnabled) {
        int fd;
        strcpy(&input_sysfs_path[input_sysfs_path_len], "enable");
        fd = open(input_sysfs_path, O_RDWR);
        if (fd >= 0) {
            char buf[2];
            int err;
            buf[1] = 0;
            if (flags) {
                buf[0] = '1';
            } else {
                buf[0] = '0';
            }
            err = write(fd, buf, sizeof(buf));
            close(fd);
            mEnabled = flags;
            return 0;
        }
        return 0;
    }
    return 0;
}

bool LightSensor::hasPendingEvents() const {
    return mHasPendingEvent;
}

int LightSensor::readEvents(sensors_event_t* data, int count)
{
    //double lux_gain;
    if (count < 1)
        return -EINVAL;

    if (mHasPendingEvent) {
        mHasPendingEvent = false;
        mPendingEvent.timestamp = getTimestamp();
        *data = mPendingEvent;
        return mEnabled ? 1 : 0;
    }

    ssize_t n = mInputReader.fill(data_fd);
    if (n < 0)
        return n;

    int numEventReceived = 0;
    input_event const* event;

    while (count && mInputReader.readEvent(&event)) {
        int type = event->type;
        if (type == EV_REL) {
            int32_t value = event->value;
            if (event->code == EVENT_TYPE_ALS_ALS) {
                als_val = value;
            } else if (event->code == EVENT_TYPE_ALS_W) {
                white_val = value;
                mPendingEvent.data[0] = (int)als_val;
                mPendingEvent.data[1] = (int)als_val;
                mPendingEvent.data[2] = (int)white_val;
            }
        } else if (type == EV_SYN) {
            mPendingEvent.timestamp = timevalToNano(event->time);
            if (mEnabled) {
                *data++ = mPendingEvent;
                count--;
                numEventReceived++;
            }
        } else {
            ALOGE("LightSensor: unknown event (type=%d, code=%d)",
                    type, event->code);
        }
        mInputReader.next();
    }
    return numEventReceived;
}
