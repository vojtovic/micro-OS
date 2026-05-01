#ifndef KLOG_H
#define KLOG_H

#include "esp_err.h"
#include "hal/hal.h"

esp_err_t klog_init(void);
void      klog_dump(void);

const hal_klog_ops_t *klog_get_ops(void);

#endif
