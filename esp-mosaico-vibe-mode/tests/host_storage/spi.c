// SPDX-License-Identifier: Apache-2.0
#include "sdk.h"
#include STORAGE_METRICS_SOURCE
static unsigned interrupts,polls;static int result;static uint8_t busy;
int __real_spi_device_transmit(spi_device_handle_t d,spi_transaction_t*t){interrupts++;t->rx_data[0]=busy;return result;}
int spi_device_polling_transmit(spi_device_handle_t d,spi_transaction_t*t){polls++;t->rx_data[0]=busy;return result;}
int main(void){
 spi_device_handle_t nand=(void*)1,lcd=(void*)2;storage_metrics_bind(nand,2048);storage_metrics_begin();
 spi_transaction_t page={.cmd=0x13};spi_transaction_t status={.cmd=0x0f,.addr=0xc0,.rxlength=8,.flags=SPI_TRANS_USE_RXDATA};
 spi_transaction_t data={.cmd=0x0b,.rxlength=2048*8};
 assert(__wrap_spi_device_transmit(lcd,&page)==0);assert(interrupts==1&&polls==0);assert(s_total[STORAGE_SPI_PAGE]==0);
 assert(__wrap_spi_device_transmit(nand,&page)==0);busy=1;assert(__wrap_spi_device_transmit(nand,&status)==0);assert(s_ready_started>0);busy=0;assert(__wrap_spi_device_transmit(nand,&status)==0);assert(s_ready_started==0);assert(__wrap_spi_device_transmit(nand,&data)==0);
 assert(s_total[STORAGE_RAW_DATA]==1&&s_total[STORAGE_RAW_META]==0);assert(s_total[STORAGE_RAW_BYTES]==2048);
 assert(polls==(CONFIG_IRIS_FACTORY_NAND_POLL_SHORT?3u:0u));assert(interrupts==(CONFIG_IRIS_FACTORY_NAND_POLL_SHORT?2u:5u));
 data.rxlength=144*8;assert(!__wrap_spi_device_transmit(nand,&page));assert(!__wrap_spi_device_transmit(nand,&status));assert(!__wrap_spi_device_transmit(nand,&data));assert(s_total[STORAGE_RAW_META]==1);
 spi_transaction_t write={.cmd=0x02,.length=2048*8};unsigned before=interrupts;assert(!__wrap_spi_device_transmit(nand,&write));assert(interrupts==before+1);
 spi_transaction_t config={.cmd=0x0f,.addr=0xb0,.rxlength=8};before=interrupts;assert(!__wrap_spi_device_transmit(nand,&config));assert(interrupts==before+1);
 result=ESP_FAIL;unsigned calls=interrupts+polls;assert(__wrap_spi_device_transmit(nand,&page)==ESP_FAIL);assert(interrupts+polls==calls+1);assert(s_total[STORAGE_ERRORS]==1);assert(s_raw_started==0&&s_ready_started==0);
 storage_metrics_end();assert(!s_active);storage_metrics_end();storage_metrics_bind(NULL,0);
 return 0;
}
