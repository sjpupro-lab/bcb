#ifndef BT_MODEL_V4_H
#define BT_MODEL_V4_H
#include <stdint.h>
void bt_v4_init(void);
void bt_v4_free(void);
void bt_v4_train(unsigned char b);
void bt_v4_distribution(unsigned int *cum, unsigned int scale);
unsigned long bt_v4_entries(void);
#endif
