#include <stdlib.h>
#include <string.h>
#include <sys/param.h>
#include <inttypes.h>
#include "sdkconfig.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/event_groups.h"

#include "esp_system.h"
#include "esp_event.h"

#include "esp_log.h"
#include "driver/pulse_cnt.h"
#include "driver/gpio.h"
#include "esp_sleep.h"

#include "nvs_flash.h"
#include "esp_netif.h"
#include "protocol_examples_common.h"

#include "lwip/err.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"
#include <lwip/netdb.h>

//mines
#include "my_pcnt.h"

#define HOST_IP_ADDR CONFIG_EXAMPLE_IPV4_ADDR
#define PORT CONFIG_EXAMPLE_PORT
#define PCNT_HIGH_LIMIT 4
#define PCNT_LOW_LIMIT  -4

#define GPIO_A 14
#define GPIO_B 15
#define GPIO_C 12
#define GPIO_D 2
#define GPIO_OUTPUT_IO_0    18
#define GPIO_OUTPUT_IO_1    19
#define GPIO_OUTPUT_PIN_SEL  ((1ULL<<GPIO_OUTPUT_IO_0) | (1ULL<<GPIO_OUTPUT_IO_1))

#define GPIO_INPUT_IO_0     4
#define GPIO_INPUT_IO_1     5
#define GPIO_INPUT_PIN_SEL  ((1ULL<<GPIO_INPUT_IO_0) | (1ULL<<GPIO_INPUT_IO_1))
#define ESP_INTR_FLAG_DEFAULT 0

#define HOST_IP_ADDR CONFIG_EXAMPLE_IPV4_ADDR
#define PORT CONFIG_EXAMPLE_PORT


static const char *TAG = "jawn";

static QueueHandle_t gpio_evt_queue = NULL;
static QueueHandle_t xreceive_queue;
static QueueHandle_t xsend_queue;

static TaskHandle_t xbutton_read;

static TaskHandle_t xisr_test = NULL;
static TaskHandle_t xreceive_data = NULL;

static TaskHandle_t xpoll_data = NULL;
static SemaphoreHandle_t xmutex = NULL;

int sock;
int pagenum;

int channel_array[16];

typedef struct data_packet_out{
    int  cnum;
    int val;
};

void udp_write(struct data_packet_out *dp);

struct incoming_data{
    char update[28];
};
struct sockaddr_in dest_addr;

static void IRAM_ATTR gpio_isr_handler(void* arg)
{
    uint32_t gpio_num = (uint32_t) arg;
    xQueueSendFromISR(gpio_evt_queue, &gpio_num, NULL);
    xTaskResumeFromISR(xbutton_read);
}

void button_read(void* pvParameters){
    uint32_t io_num;
    ESP_LOGI(TAG, "were rollin");
    while(1){
        vTaskSuspend(NULL);
        if(xQueueReceive(gpio_evt_queue, &io_num, portMAX_DELAY)){
        if (io_num==4 && pagenum<12 ){
            pagenum+=4;
        }
        else if(io_num ==5 && pagenum > 1){
            pagenum-=4;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
        xQueueReset(gpio_evt_queue);
    }
    }
}


inline uint32_t Reverse32(uint32_t value) 
{
    return (((value & 0x000000FF) << 24) |
            ((value & 0x0000FF00) <<  8) |
            ((value & 0x00FF0000) >>  8) |
            ((value & 0xFF000000) >> 24));
}




static bool isr_handler(pcnt_unit_handle_t unit, const pcnt_watch_event_data_t *edata, void *user_ctx){
    BaseType_t high_task_wakeup;
    int encoder_arg = (int)user_ctx;
    int encoder_val = edata->watch_point_value;
    struct data_packet_out dp = {
        .cnum = encoder_arg,
        .val = encoder_val, 
    };
    xQueueSendFromISR(xsend_queue, &dp, &high_task_wakeup);
    xTaskResumeFromISR(xisr_test);
    return (high_task_wakeup == pdFALSE);
}

void isr_test(void *pvParameters){
    BaseType_t high_task_wakeup = pdTRUE;
    struct data_packet_out dp_isr = {
        .cnum = 0,
        .val = 0,
    };
    while(1){
        vTaskSuspend(NULL);
        while(xQueueReceive(xsend_queue, &dp_isr, pdMS_TO_TICKS(200))){
            
        
        udp_write( &dp_isr);
        }
       
    }
}

void remote(void *pvParameters){
    
    char rem_buf[12]="/xremote\0\0\0\0";
    char host_ip[] = HOST_IP_ADDR;
    int addr_family = 0;
    int ip_protocol = 0;
    int err = 0;

    //struct sockaddr_in dest_addr;
    dest_addr.sin_addr.s_addr = inet_addr(HOST_IP_ADDR);
    dest_addr.sin_family = AF_INET;
    dest_addr.sin_port = htons(PORT);
    addr_family = AF_INET;
    ip_protocol = IPPROTO_IP;
        
    sock = socket(addr_family, SOCK_DGRAM, ip_protocol);
    if (sock < 0) {
        //ESP_LOGE(TAG, "Unable to create socket: errno %d", errno);
    }

    struct timeval timeout;
    timeout.tv_sec = 0;
    timeout.tv_usec = 1000;
    setsockopt (sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof timeout);
    while(1){
        err = sendto(sock, rem_buf, sizeof(rem_buf), 0, (struct sockaddr *)&dest_addr, sizeof(dest_addr));
        vTaskPrioritySet(NULL, 1);
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}

void receive_data(void *pvParameters){
    char receive_buf[28];
    struct sockaddr_storage source_addr; // Large enough for both IPv4 or IPv6
    socklen_t socklen = sizeof(source_addr);
    struct incoming_data just_in={
        .update='\0',
    };
    
    while(1){
        int len = recvfrom(sock, receive_buf, sizeof(receive_buf) - 1, 0, (struct sockaddr *)&source_addr, &socklen);
        while(len ==-1){

            len = recvfrom(sock, receive_buf, sizeof(receive_buf) - 1, 0, (struct sockaddr *)&source_addr, &socklen);
        }
        vTaskPrioritySet(NULL, 3);
        if( xSemaphoreTake( xmutex, ( TickType_t ) 10 ) == pdTRUE ){
            memcpy(&just_in.update[0], &receive_buf[0], (len+1));
            xQueueSend(xreceive_queue, &just_in, pdMS_TO_TICKS(100));
            vTaskPrioritySet(NULL, 1);
        }
        xSemaphoreGive(xmutex);
        
    }
}

void parse_incoming(void *pvParameters){
    char to_parse[28];
    int ch_sel = 0;
    float value_f = 0.0;
    uint32_t value_i = 0;
    while(1){

        while(xQueueReceive(xreceive_queue, &to_parse, pdMS_TO_TICKS(10))){
            vTaskPrioritySet(NULL, 3);
            memcpy(&value_i, &to_parse[24], 4);
            value_i = Reverse32(value_i);
            memcpy(&value_f, &value_i, 4);

            value_i = (value_f * 1024);

            ch_sel = to_parse[5] - '0';
            if(to_parse[4]=='1'){
                ch_sel+=10;
            }
            channel_array[(ch_sel-1)] = value_i;
            vTaskPrioritySet(NULL, 1);
        }    
    }
}
void poll_data(void *pvParameters){
    int index = 0;
    char tx_buf[20] = "/ch/\0\0/mix/fader\0\0\0\0";
    while(1){
        for(int i = 0; i< 16; i++){
            index = i + 1;
            if(index>=10){
                tx_buf[4] = '1';
                index -= 10;
            }else tx_buf[4] = '0';
            tx_buf[5] = '0' + index;
            sendto(sock, tx_buf, sizeof(tx_buf), 0, (struct sockaddr *)&dest_addr, sizeof(dest_addr));

            vTaskPrioritySet(xreceive_data, 3);
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }


}

void udp_write(struct data_packet_out *dp){
    u32_t sval=0;
    int nval;
    dp->cnum+= pagenum;
    int channel = dp->cnum+1;
    printf("ch num is %d",channel);
    unsigned char tx_buf[28] = "/ch/\0\0/mix/fader\0\0\0\0,i\0\0,0,0,0,0";
    unsigned char schannel[2] = "00" ;
    if(channel >=10){
        schannel[0] = '1';
        channel -= 10;
        schannel[1] = channel + '0';
    }
    else{
        schannel[0] = '0';
        schannel[1] = '0' + channel;
    }
    nval = channel_array[dp->cnum] + (dp->val * 6);
    if(nval<0){
        nval = 0;
    }
    if(nval> 1024){
        nval = 1024;
    }
    channel_array[dp->cnum] = nval;
    sval = (uint32_t)(nval);
    sval = Reverse32(sval);

    // char rx_buffer[128];
    // char host_ip[] = HOST_IP_ADDR;
    // int addr_family = 0;
    // int ip_protocol = 0;
    int err = 0;
    ESP_LOGI(TAG, "Socket created, sending to %s:%d sval is %lu val is %d", HOST_IP_ADDR, PORT, sval, nval);
    memcpy(&tx_buf[24], &sval, 4);
    memcpy(&tx_buf[4], &schannel, 2);

    err = sendto(sock, tx_buf, sizeof(tx_buf), 0, (struct sockaddr *)&dest_addr, sizeof(dest_addr));

    // struct sockaddr_in dest_addr;
    // dest_addr.sin_addr.s_addr = inet_addr(HOST_IP_ADDR);
    // dest_addr.sin_family = AF_INET;
    // dest_addr.sin_port = htons(PORT);
    // addr_family = AF_INET;
    // ip_protocol = IPPROTO_IP;
        
    // int sock = socket(addr_family, SOCK_DGRAM, ip_protocol);
    // if (sock < 0) {
    //     ESP_LOGE(TAG, "Unable to create socket: errno %d", errno);
    // }

    // struct timeval timeout;
    // timeout.tv_sec = 0;
    // timeout.tv_usec = 1000;
    // setsockopt (sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof timeout);



    

    // //receive
    // struct sockaddr_storage source_addr; // Large enough for both IPv4 or IPv6
    // socklen_t socklen = sizeof(source_addr);
    // int len = recvfrom(sock, rx_buffer, sizeof(rx_buffer) - 1, 0, (struct sockaddr *)&source_addr, &socklen);
    // rx_buffer[len]=0;


    // if(len < 0) {
    //     ESP_LOGE(TAG, "recvfrom failed: errno %d", errno);
                
    // }else {
    //     rx_buffer[len] = 0; // Null-terminate whatever we received and treat like a string
    //     ESP_LOGI(TAG, "Received %d bytes from %s:", len, host_ip);
    //     ESP_LOGI(TAG, "%s", rx_buffer);
    //     if (strncmp(rx_buffer, "OK: ", 4) == 0) {
    //         ESP_LOGI(TAG, "Received expected message, reconnecting");
    //     }
    // }

     
}

void app_main(void)
{
    
    int pagenum=0;
     gpio_config_t io_conf = {};
    //disable interrupt
    io_conf.intr_type = GPIO_INTR_DISABLE;
    //set as output mode
    io_conf.mode = GPIO_MODE_OUTPUT;
    //bit mask of the pins that you want to set,e.g.GPIO18/19
    io_conf.pin_bit_mask = GPIO_OUTPUT_PIN_SEL;
    //disable pull-down mode
    io_conf.pull_down_en = 0;
    //disable pull-up mode
    io_conf.pull_up_en = 0;
    //configure GPIO with the given settings
    gpio_config(&io_conf);

    //interrupt of rising edge
    io_conf.intr_type = GPIO_INTR_POSEDGE;
    //bit mask of the pins, use GPIO4/5 here
    io_conf.pin_bit_mask = GPIO_INPUT_PIN_SEL;
    //set as input mode
    io_conf.mode = GPIO_MODE_INPUT;
    //enable pull-up mode
    io_conf.pull_up_en = 1;
    gpio_config(&io_conf);

    gpio_set_intr_type(GPIO_INPUT_IO_0, GPIO_INTR_POSEDGE);

    gpio_evt_queue = xQueueCreate(10, sizeof(uint32_t));
    //start gpio task
    xTaskCreate(button_read, "gpio_task_example", 2048, NULL, 1, &xbutton_read);

    //install gpio isr service
    gpio_install_isr_service(ESP_INTR_FLAG_DEFAULT);
    //hook isr handler for specific gpio pin
    gpio_isr_handler_add(GPIO_INPUT_IO_0, gpio_isr_handler, (void*) GPIO_INPUT_IO_0);
    gpio_isr_handler_add(GPIO_INPUT_IO_1, gpio_isr_handler, (void*) GPIO_INPUT_IO_1);

        gpio_isr_handler_remove(GPIO_INPUT_IO_0);
    //hook isr handler for specific gpio pin again
    gpio_isr_handler_add(GPIO_INPUT_IO_0, gpio_isr_handler, (void*) GPIO_INPUT_IO_0);




    xmutex = xSemaphoreCreateMutex();
    sock = 0;
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    /* This helper function configures Wi-Fi or Ethernet, as selected in menuconfig.
     * Read "Establishing Wi-Fi or Ethernet Connection" section in
     * examples/protocols/README.md for more information about this function.
     */
    ESP_ERROR_CHECK(example_connect());

    
    pcnt_unit_handle_t encoder0 = create_encoder(PCNT_HIGH_LIMIT, PCNT_LOW_LIMIT, GPIO_A, GPIO_B );
    int encoder_arg0 = 0; 

    
    pcnt_unit_handle_t encoder1 = create_encoder(PCNT_HIGH_LIMIT, PCNT_LOW_LIMIT, GPIO_C, GPIO_D );
    int encoder_arg1 = 1;

     int watch_points[] = {PCNT_LOW_LIMIT, PCNT_HIGH_LIMIT};
    for (size_t i = 0; i < sizeof(watch_points) / sizeof(watch_points[0]); i++) {
        ESP_ERROR_CHECK(pcnt_unit_add_watch_point(encoder1, watch_points[i]));
        ESP_ERROR_CHECK(pcnt_unit_add_watch_point(encoder0, watch_points[i]));
    }


      pcnt_event_callbacks_t cbs = {
        .on_reach = isr_handler,
    };
    
    xreceive_queue = xQueueCreate(20, 32);
    xsend_queue = xQueueCreate(10, 32);

     xTaskCreate(isr_test, "isr_test", 4096, NULL,1, &xisr_test);

    ESP_ERROR_CHECK(pcnt_unit_register_event_callbacks(encoder0, &cbs, encoder_arg0));
    ESP_ERROR_CHECK(pcnt_unit_register_event_callbacks(encoder1, &cbs, encoder_arg1));
    
    for(int i=0; i<16; i++){
        //channel_array[i].ch_name = i+1;
        channel_array[i] = 0;
    };
    start_encoder(encoder0);
    start_encoder(encoder1);

    xTaskCreate(remote, NULL, 4096, NULL,2,NULL);
    xTaskCreate(receive_data, NULL, 4096, NULL, 1, &xreceive_data);
    xTaskCreate(poll_data, NULL, 4096, NULL,1, &xpoll_data);
    xTaskCreate(parse_incoming,NULL,4096,NULL,1,NULL);

    //udp
    printf("Minimum free heap size: %"PRIu32" bytes\n", esp_get_minimum_free_heap_size());

    ESP_LOGI(TAG,"input pinny %llu", GPIO_INPUT_PIN_SEL);


        gpio_set_level(GPIO_OUTPUT_IO_0, 0);
        gpio_set_level(GPIO_OUTPUT_IO_1, 0);
    

}

