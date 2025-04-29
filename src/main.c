/*******************************************************************
 *
 * main.c - LVGL simulator for GNU/Linux
 *
 * Based on the original file from the repository
 *
 * @note eventually this file won't contain a main function and will
 * become a library supporting all major operating systems
 *
 * To see how each driver is initialized check the
 * 'src/lib/display_backends' directory
 *
 * - Clean up
 * - Support for multiple backends at once
 *   2025 EDGEMTech Ltd.
 *
 * Author: EDGEMTech Ltd, Erik Tagirov (erik.tagirov@edgemtech.ch)
 *
 ******************************************************************/
#include <unistd.h>
#include <pthread.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>

#include <fcntl.h>
#include <termios.h>
#include <stdint.h>
#include <dirent.h>
 #include "ui.h"
 #include "../../lv_port_linux/lvgl/lvgl.h"


#define DEFAULT_UART_PORT "/dev/tty*"
#define HEADER_OUTPUT 0xF1
#define CMD_XXX_193   0xC1  
#define BUFFER_SIZE 8192
static size_t buffer_pos = 0;
#define HEADER_INPUT  0xf0

#define CMD_GET     0xa1
static char data_buffer[BUFFER_SIZE];

int uart_fd = -1;
char selected_port[128] = DEFAULT_UART_PORT;
lv_obj_t *ui_ConnectBtn;
lv_obj_t *ui_StatusLabel;

bool is_connected = false;
static bool is_reading = false;
static bool command_sent = false; 

#include "../lvgl/demos/lv_demos.h"

#include "src/lib/driver_backends.h"
#include "src/lib/simulator_util.h"
#include "src/lib/simulator_settings.h"

static void configure_simulator(int argc, char **argv);
static void print_lvgl_version(void);
static void print_usage(void);
static void draw_event_cb(lv_event_t * e);
static void add_gradient_area(lv_event_t * e);
static void event_handler(lv_event_t * e);
void sendCommand(uint8_t c1, uint8_t c2, uint8_t c3, uint8_t* c5, size_t c5_len);
void print_device_data(uint8_t type, uint8_t* data, uint8_t length);
static lv_timer_t *serial_read_timer = NULL;
void uart_close();
float parse_float(uint8_t* bytes);
void sendCommandFloat(uint8_t c1, uint8_t c2, uint8_t c3, float c5);
void button_event_handler(lv_event_t * e);
void clear_serial_buffer();


static char *selected_backend;

extern simulator_settings_t settings;



static void print_lvgl_version(void)
{
    fprintf(stdout, "%d.%d.%d-%s\n",
            LVGL_VERSION_MAJOR,
            LVGL_VERSION_MINOR,
            LVGL_VERSION_PATCH,
            LVGL_VERSION_INFO);
}


static void print_usage(void)
{
    fprintf(stdout, "\nlvglsim [-V] [-B] [-b backend_name] [-W window_width] [-H window_height]\n\n");
    fprintf(stdout, "-V print LVGL version\n");
    fprintf(stdout, "-B list supported backends\n");
}


static void event_handler(lv_event_t * e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t * obj = lv_event_get_target_obj(e);
    uint8_t value = 1; // Gönderilecek veri

    if(code == LV_EVENT_VALUE_CHANGED) {
        
        LV_UNUSED(obj);
        LV_LOG_USER("State: %s\n", lv_obj_has_state(obj, LV_STATE_CHECKED) ? "On" : "Off");
        if(lv_obj_has_state(obj, LV_STATE_CHECKED)){


            //sendCommandFloat(HEADER_OUTPUT, 0xb1, 193, 3.14f);

            sendCommand(HEADER_OUTPUT, 177, 219, &value, 1);

            
        }
        else{
            sendCommand(HEADER_OUTPUT, 177, 219, &value, 0);

        }
    }
}


/**
 * @brief Configure simulator
 * @description process arguments recieved by the program to select
 * appropriate options
 * @param argc the count of arguments in argv
 * @param argv The arguments
 */
 void clear_serial_buffer() {
    tcflush(uart_fd, TCIFLUSH); // Gelen veri buffer'ını temizle
    usleep(10000); // 10ms bekle (cihazın hazır olması için)
}

 void button_event_handler(lv_event_t * e) {
    lv_obj_t * btn = lv_event_get_target(e);
    
    if (btn == ui_Button3  || btn == ui_Button2) {
        sendCommandFloat(HEADER_OUTPUT, 0xb1, 193, lv_spinbox_get_value(ui_Spinbox1)/100.0f);

    } else if (btn == ui_Button4  || btn == ui_Button5) {
        sendCommandFloat(HEADER_OUTPUT, 0xb1,194,   lv_spinbox_get_value(ui_Spinbox2)/100.0f);

    }
}


static void configure_simulator(int argc, char **argv)
{
    int opt = 0;
    char *backend_name;

    selected_backend = NULL;
    driver_backends_register();

    /* Default values */
    settings.window_width = atoi(getenv("LV_SIM_WINDOW_WIDTH") ? : "800");
    settings.window_height = atoi(getenv("LV_SIM_WINDOW_HEIGHT") ? : "480");

    /* Parse the command-line options. */
    while ((opt = getopt (argc, argv, "b:fmW:H:BVh")) != -1) {
        switch (opt) {
        case 'h':
            print_usage();
            exit(EXIT_SUCCESS);
            break;
        case 'V':
            print_lvgl_version();
            exit(EXIT_SUCCESS);
            break;
        case 'B':
            driver_backends_print_supported();
            exit(EXIT_SUCCESS);
            break;
        case 'b':
            if (driver_backends_is_supported(optarg) == 0) {
                die("error no such backend: %s\n", optarg);
            }
            selected_backend = strdup(optarg);
            break;
        case 'W':
            settings.window_width = atoi(optarg);
            break;
        case 'H':
            settings.window_height = atoi(optarg);
            break;
        case ':':
            print_usage();
            die("Option -%c requires an argument.\n", optopt);
            break;
        case '?':
            print_usage();
            die("Unknown option -%c.\n", optopt);
        }
    }
}


/////////////////////////////////////////////////////////////////////////////////




static void serial_read_timer_cb(lv_timer_t * timer) {
    if (uart_fd < 0 || !is_reading) return;
   
    // 1. Tüm verileri sorgulamak için tek komut gönder
    if (!command_sent) {
        uint8_t empty_data = 0;
        // Use our new sendCommand function
        sendCommand(HEADER_OUTPUT, CMD_GET, 255, &empty_data, 1);

        command_sent = true;
        return;
    }

    // 2. Seri porttan veri oku
    ssize_t bytes_read = read(uart_fd, data_buffer + buffer_pos, BUFFER_SIZE - buffer_pos - 1);
    if (bytes_read > 0) {
        buffer_pos += bytes_read;
        data_buffer[buffer_pos] = '\0';

        // 3. Veriyi işle
        for (int i = 0; i < buffer_pos - 6; i++) {
            if ((uint8_t)data_buffer[i] == HEADER_INPUT && (uint8_t)data_buffer[i+1] == CMD_GET) {
                uint8_t c3 = data_buffer[i+2];
                uint8_t c4 = data_buffer[i+3];

                if (i + 4 + c4 >= buffer_pos) break; // eksik paket

                uint8_t checksum = c3 + c4;
                for (int j = 0; j < c4; j++) {
                    checksum += data_buffer[i+4+j];
                }
                checksum %= 0x100;

                if (checksum != (uint8_t)data_buffer[i+4+c4]) {
                    printf("Checksum hatası: hesaplanan %02x, gelen %02x\n",
                           checksum, (uint8_t)data_buffer[i+4+c4]);
                    break;
                }

                // Geçerli veri bulundu: işle
                print_device_data(c3, (uint8_t*)(data_buffer + i + 4), c4);

                // Buffer'ı temizle
                memset(data_buffer, 0, BUFFER_SIZE);
                buffer_pos = 0;
                command_sent = false; // Yeni komut gönderimine izin ver
                break;
            }
        }
    } else if (bytes_read < 0 && errno != EAGAIN) {
        printf("Seri port okuma hatası: %s\n", strerror(errno));
        uart_close();
        is_reading = false;

        lv_obj_t *label = lv_obj_get_child(ui_Button1, 0);
        lv_label_set_text(label, "CONNECT");

        if (serial_read_timer != NULL) {
            lv_timer_pause(serial_read_timer);
        }
    }
}

void print_device_data(uint8_t type, uint8_t* data, uint8_t length) {
    switch (type) {
        case 192: // Input voltage
            printf("Input voltage: %.2f V\n", parse_float(data));
            break;
        case 195: // Output voltage, current, power
            printf("Output voltage: %.2f V\n", parse_float(data));
            printf("Output current: %.2f A\n", parse_float(data + 4));
            printf("Output power: %.2f W\n", parse_float(data + 8));
            break;
        case 196: // Temperature
           
            printf("Temperature: %.1f °C\n", parse_float(data));
      
           
            //lv_chart_refresh(ui_Chart1);
            break;
        case 219: // Output status
            printf("Output: %s\n", data[0] ? "ON" : "OFF");
            break;
        case 221: // CC/CV mode
            printf("Mode: %s\n", data[0] ? "CV" : "CC");
            break;
        case 222: // Model name
            {
                char model[32] = {0};
                memcpy(model, data, length);
                printf("Model name: %s\n", model);
            }
            break;
        case 223: // Hardware version
            {
                char version[32] = {0};
                memcpy(version, data, length);
                printf("Hardware version: %s\n", version);
            }
            break;
        case 224: // Firmware version
            {
                char version[32] = {0};
                memcpy(version, data, length);
                printf("Firmware version: %s\n", version);
            }
            break;
        case 255: // All data
            printf("=========== COMPLETE DEVICE STATUS ===========\n");
            printf("Input voltage: %.2f V\n", parse_float(data));
            printf("Set voltage: %.2f V\n", parse_float(data + 4));
            printf("Set current: %.2f A\n", parse_float(data + 8));
            printf("Output voltage: %.2f V\n", parse_float(data + 12));
            printf("Output current: %.2f A\n", parse_float(data + 16));
            printf("Output power: %.2f W\n", parse_float(data + 20));
            printf("Temperature: %.1f °C\n", parse_float(data + 24));
            char buff[10];
            sprintf(buff," %.1f C\n", parse_float(data + 24));
            lv_label_set_text(ui_Label3,buff);
            float temp = parse_float(data+ 24);
            int32_t temp_scaled = (int32_t)(temp);
            lv_chart_series_t * ser = lv_chart_get_series_next(ui_Chart1, NULL);
            lv_chart_set_next_value(ui_Chart1, ser,  temp_scaled); // 0.1°C hassasiyet için 10 ile çarp
            lv_chart_refresh(ui_Chart1);
            sprintf(buff," %.1f W\n", parse_float(data + 20));
            lv_label_set_text(ui_Label5,buff);
            temp = parse_float(data+ 20);
            temp_scaled = (int32_t)(temp);
            ser = lv_chart_get_series_next(ui_Chart2, NULL);
            lv_chart_set_next_value(ui_Chart2, ser,  temp_scaled); // 0.1°C hassasiyet için 10 ile çarp
            lv_chart_refresh(ui_Chart2);





            lv_chart_series_t *ser_V = lv_chart_get_series_next(ui_Chart3, NULL);
            temp = parse_float(data+ 12);
            temp_scaled = (int32_t)(temp);
            lv_chart_set_next_value(ui_Chart3, ser_V,  temp_scaled); // 0.1°C hassasiyet için 10 ile çarp
            
            lv_chart_series_t *ser_A = lv_chart_get_series_next(ui_Chart3, ser_V);
            temp = parse_float(data+ 16);
            temp_scaled = (int32_t)(temp);
            lv_chart_set_next_value(ui_Chart3, ser_A,  temp_scaled); // 0.1°C hassasiyet için 10 ile çarp

            lv_chart_refresh(ui_Chart3);

            // Groups 1-6 settings
            for (int i = 0; i < 6; i++) {
                printf("Group %d voltage: %.2f V\n", i+1, parse_float(data + 28 + i*8));
                printf("Group %d current: %.2f A\n", i+1, parse_float(data + 32 + i*8));
            }
            
            // Protection settings
            printf("OVP: %.2f V\n", parse_float(data + 76));
            printf("OCP: %.2f A\n", parse_float(data + 80));
            printf("OPP: %.2f W\n", parse_float(data + 84));
            printf("OTP: %.2f °C\n", parse_float(data + 88));
            printf("LVP: %.2f V\n", parse_float(data + 92));
            
            // Device settings
            printf("Brightness: %d\n", data[96]);
            printf("Volume: %d\n", data[97]);
            printf("Metering: %s\n", data[98] ? "CLOSED" : "OPEN");
            
            // Output stats
            printf("Output capacity: %.3f Ah\n", parse_float(data + 99));
            printf("Output energy: %.3f Wh\n", parse_float(data + 103));
            printf("Output: %s\n", data[107] ? "ON" : "OFF");
            
            // Status
            const char* protection_states[] = {"NONE", "OVP", "OCP", "OPP", "OTP", "LVP", "REP"};
            int protection = data[108];
            if (protection >= 0 && protection < 7) {
                printf("Protection: %s\n", protection_states[protection]);
            } else {
                printf("Protection: UNKNOWN (%d)\n", protection);
            }
            
            printf("Mode: %s\n", data[109] ? "CV" : "CC");
            
            // Limits
            printf("Upper limit voltage: %.2f V\n", parse_float(data + 111));
            printf("Upper limit current: %.2f A\n", parse_float(data + 115));
            printf("============================================\n");
            break;
        default:
            printf("Unknown data type: %d\n", type);
            printf("Data: ");
            for (int i = 0; i < length; i++) {
                printf("%02x ", data[i]);
            }
            printf("\n");
    }
}
float parse_float(uint8_t* bytes) {
    float value;
    memcpy(&value, bytes, 4);
    return value;
}

////////////////////////////////////////////////////////////////////////////////


int uart_open(const char* portname) {
    uart_fd = open(portname, O_RDWR | O_NOCTTY | O_NDELAY);
    if (uart_fd == -1) {
        perror("UART open failed");
        return -1;
    }

    struct termios options;
    tcgetattr(uart_fd, &options);

    cfsetispeed(&options, B115200);  // Baud rate ayarı
    cfsetospeed(&options, B115200);

    options.c_cflag |= (CLOCAL | CREAD);    // Receiver açık
    options.c_cflag &= ~PARENB;              // Parity yok
    options.c_cflag &= ~CSTOPB;              // 1 stop bit
    options.c_cflag &= ~CSIZE;
    options.c_cflag |= CS8;                  // 8 data bits
    options.c_cflag |= CRTSCTS;
    options.c_lflag &= ~(ICANON | ECHO | ECHOE | ISIG); // Raw input
    options.c_iflag &= ~(IXON | IXOFF | IXANY);         // Yazılım akış kontrolü yok
    options.c_oflag &= ~OPOST;                          // Raw output

    tcsetattr(uart_fd, TCSANOW, &options);

    return 0;
}

// UART kapatma fonksiyonu
void uart_close() {
    if (uart_fd != -1) {
        close(uart_fd);
        uart_fd = -1;
    }
}

// UART'a veri gönderen fonksiyon
void sendCommandRaw(uint8_t* data, size_t length) {
    if (uart_fd == -1) {
        printf("UART not opened!\n");
        return;
    }
    clear_serial_buffer();  // Buffer'ı temizle

    ssize_t bytes_written = write(uart_fd, data, length);
    if (bytes_written < 0) {
        perror("UART write failed");
    } else {
        printf("Sent %zd bytes over UART.\n", bytes_written);
    }
}

// Komut oluşturan fonksiyon
void sendCommand(uint8_t c1, uint8_t c2, uint8_t c3, uint8_t* c5, size_t c5_len) {
    uint8_t c4 = (uint8_t)c5_len;
    uint8_t c6 = c3 + c4;

    for (size_t i = 0; i < c5_len; i++) {
        c6 += c5[i];
    }

    size_t total_len = c5_len + 5;
    uint8_t* c = (uint8_t*)malloc(total_len);
    if (c == NULL) {
        printf("Memory allocation failed!\n");
        return;
    }

    c[0] = c1;
    c[1] = c2;
    c[2] = c3;
    c[3] = c4;
    memcpy(&c[4], c5, c5_len);
    c[total_len - 1] = c6;

    sendCommandRaw(c, total_len);

    free(c);
}
void sendCommandFloat(uint8_t c1, uint8_t c2, uint8_t c3, float c5) {
    // Float değerini 4 byte'lık bir buffer'a kopyala
    uint8_t buffer[4];
    memcpy(buffer, &c5, sizeof(float));
    
    // sendCommand fonksiyonunu çağır
    sendCommand(c1, c2, c3, buffer, sizeof(buffer));
}
// Sabitler

// Linux sisteminde kullanılabilir seri portları tarar
char* scan_serial_ports() {
    static char ports_list[1024] = {0};
    memset(ports_list, 0, sizeof(ports_list));

    // Linux'ta tipik seri port konumları
    const char* paths[] = {
        "/dev/ttyUSB", 
        "/dev/ttyACM", 
        "/dev/ttyS"
    };

    int total_count = 0;
    DIR* dir;
    struct dirent* ent;
    char temp[256];

    // /dev dizinini aç
    if ((dir = opendir("/dev")) != NULL) {
        // Tüm portları tara
        while ((ent = readdir(dir)) != NULL) {
            for (int i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) {
                if (strncmp(ent->d_name, paths[i] + 5, strlen(paths[i]) - 5) == 0) {
                    snprintf(temp, sizeof(temp), "%s%s", strlen(ports_list) > 0 ? "\n" : "", ent->d_name);
                    strcat(ports_list, temp);
                    total_count++;
                    break;
                }
            }
        }
        closedir(dir);
    }

    // Eğer hiç port bulunamadıysa, varsayılan değeri kullan
    if (total_count == 0) {
        strcpy(ports_list, "ttyACM0\nttyUSB0");
    }

    return ports_list;
}

static void dropdown_event_cb(lv_event_t * e) {
    lv_obj_t * obj = lv_event_get_target(e);
    int selected_idx = lv_dropdown_get_selected(obj);
    char buf[128];
    
    lv_dropdown_get_selected_str(obj, buf, sizeof(buf));
    snprintf(selected_port, sizeof(selected_port), "/dev/%s", buf);
    printf("Selected UART port: %s\n", selected_port);
}

static void connect_btn_event_cb(lv_event_t * e) {
    lv_obj_t *btn = lv_event_get_target(e);
    lv_obj_t *label = lv_obj_get_child(btn, 0);  // Buton içindeki etiketi al
    
    if (!is_connected) {
        // Bağlan
        if (uart_open(selected_port) == 0) {
            is_connected = true;
            is_reading= true;

            lv_label_set_text(ui_StatusLabel, "Stat: Success");
            lv_obj_set_style_bg_color(ui_StatusLabel, lv_color_hex(0x008800), 0); // Yeşil
            if (serial_read_timer == NULL) {
                serial_read_timer = lv_timer_create(serial_read_timer_cb, 100, NULL);  // 100ms interval
                uint8_t value = 1; // Gönderilecek veri
    
                // Komut gönder
                sendCommand(HEADER_OUTPUT, CMD_XXX_193, 0, &value, 1);
                printf("Command sent\n");
            } else {
                lv_timer_resume(serial_read_timer);
            }
            // Buton etiketini doğru şekilde güncelle
            if (label != NULL && lv_obj_check_type(label, &lv_label_class)) {
                lv_obj_set_style_text_color(label,lv_color_hex(0x008800), 0);
                lv_label_set_text(label, "Disconnect");
            }
            
            // Dropdown'u devre dışı bırak
            lv_obj_add_state(ui_Dropdown2, LV_STATE_DISABLED);
            lv_obj_clear_state(ui_Switch1, LV_STATE_DISABLED);

            printf("Connected to %s successfully\n", selected_port);
        } else {
            lv_label_set_text(ui_StatusLabel, "Stat: Connection Error!");
            lv_obj_set_style_bg_color(ui_StatusLabel, lv_color_hex(0x1F1F1F), 0); // Kırmızı
            printf("Failed to connect to %s\n", selected_port);
        }
    } else {
        // Bağlantıyı kes
        uart_close();
        is_connected = false;
        lv_label_set_text(ui_StatusLabel, "Stat: Connection  Lost");
        lv_obj_set_style_bg_color(ui_StatusLabel, lv_color_hex(0x1F1F1F), 0); // Gri
        
        // Buton etiketini doğru şekilde güncelle
        if (label != NULL && lv_obj_check_type(label, &lv_label_class)) {
            lv_obj_set_style_text_color(label,lv_color_hex(0xffffff), 0);
           

            lv_label_set_text(label, "Connect");
        }
        
        // Dropdown'u tekrar aktif et
        lv_obj_clear_state(ui_Dropdown2, LV_STATE_DISABLED);
        lv_obj_add_state(ui_Switch1, LV_STATE_DISABLED);

        printf("Disconnected from %s\n", selected_port);
    }
}

// Komut gönderme butonu için event handler
static void send_cmd_event_cb(lv_event_t * e) {
    if (!is_connected) {
        printf("Cannot send command, not connected!\n");
        return;
    }
    
    uint8_t value = 1; // Gönderilecek veri
    
    // Komut gönder
    sendCommand(HEADER_OUTPUT, CMD_XXX_193, 0, &value, 1);
    printf("Command sent\n");
    //sendCommand(HEADER_OUTPUT, CMD_XXX_193, 0, &value, 1);
    usleep(500000);
    sendCommand(HEADER_OUTPUT, 177, 219, &value, 1);
    usleep(500000);
    sendCommand(HEADER_OUTPUT, 177, 219, &value, 0);
}


void lv_example_chart_gradient(void)
 {
 
     /*Gradyent efekt için olay işleyici ekleme*/
     lv_obj_add_event_cb(ui_Chart1, draw_event_cb, LV_EVENT_DRAW_TASK_ADDED, NULL);
     lv_obj_add_flag(ui_Chart1, LV_OBJ_FLAG_SEND_DRAW_TASK_EVENTS);
     lv_obj_add_event_cb(ui_Chart2, draw_event_cb, LV_EVENT_DRAW_TASK_ADDED, NULL);
     lv_obj_add_flag(ui_Chart2, LV_OBJ_FLAG_SEND_DRAW_TASK_EVENTS);
     lv_obj_add_event_cb(ui_Chart3, draw_event_cb, LV_EVENT_DRAW_TASK_ADDED, NULL);
     lv_obj_add_flag(ui_Chart3, LV_OBJ_FLAG_SEND_DRAW_TASK_EVENTS);
 
 }
 static void add_gradient_area(lv_event_t * e)
 {
     lv_obj_t * obj = lv_event_get_target_obj(e);
     lv_area_t coords;
     lv_obj_get_coords(obj, &coords);
 
     lv_draw_task_t * draw_task = lv_event_get_draw_task(e);
     lv_draw_dsc_base_t * base_dsc = (lv_draw_dsc_base_t *)lv_draw_task_get_draw_dsc(draw_task);
 
     /*Çizilen seriyi al*/
     const lv_chart_series_t * ser = lv_chart_get_series_next(obj, NULL);
     int series_index = 0;
     
     /*Doğru seriyi bulmak için serileri döngüyle kontrol et*/
     while(ser != NULL) {
         if(base_dsc->id1 == series_index) {
             break; /*Doğru seriyi bulduk*/
         }
         ser = lv_chart_get_series_next(obj, ser);
         series_index++;
     }
     
     /*Eğer seri bulunamadıysa çık*/
     if(ser == NULL) return ;
 
     
     lv_color_t ser_color = lv_chart_get_series_color(obj, ser);
 
     /*Çizginin altına gradyent üçgen ekleme*/
     lv_draw_line_dsc_t * draw_line_dsc = (lv_draw_line_dsc_t *)lv_draw_task_get_draw_dsc(draw_task);
     lv_draw_triangle_dsc_t tri_dsc;
 
     lv_draw_triangle_dsc_init(&tri_dsc);
     tri_dsc.p[0].x = draw_line_dsc->p1.x;
     tri_dsc.p[0].y = draw_line_dsc->p1.y;
     tri_dsc.p[1].x = draw_line_dsc->p2.x;
     tri_dsc.p[1].y = draw_line_dsc->p2.y;
     tri_dsc.p[2].x = draw_line_dsc->p1.y < draw_line_dsc->p2.y ? draw_line_dsc->p1.x : draw_line_dsc->p2.x;
     tri_dsc.p[2].y = LV_MAX(draw_line_dsc->p1.y, draw_line_dsc->p2.y);
     tri_dsc.grad.dir = LV_GRAD_DIR_VER;
 
     int32_t full_h = lv_obj_get_height(obj);
     int32_t fract_upper = (int32_t)(LV_MIN(draw_line_dsc->p1.y, draw_line_dsc->p2.y) - coords.y1) * 255 / full_h;
     int32_t fract_lower = (int32_t)(LV_MAX(draw_line_dsc->p1.y, draw_line_dsc->p2.y) - coords.y1) * 255 / full_h;
     
     /*Opaklığı ayarla - her seri için daha düşük değer kullan, böylece üst üste binince görünür kalır*/
     lv_opa_t max_opa = series_index == 0 ? 180 : 150; /*İlk seri daha opak, ikinci daha transparan*/
     
     tri_dsc.grad.stops[0].color = ser_color;
     tri_dsc.grad.stops[0].opa = (max_opa * (255 - fract_upper)) / 255;
     tri_dsc.grad.stops[0].frac = 0;
     tri_dsc.grad.stops[1].color = ser_color;
     tri_dsc.grad.stops[1].opa = (max_opa * (255 - fract_lower)) / 255;
     tri_dsc.grad.stops[1].frac = 255;
 
     lv_draw_triangle(base_dsc->layer, &tri_dsc);
 
     /*Üçgenin altına gradyent dikdörtgen ekleme*/
     lv_draw_rect_dsc_t rect_dsc;
     lv_draw_rect_dsc_init(&rect_dsc);
     rect_dsc.bg_grad.dir = LV_GRAD_DIR_VER;
     rect_dsc.bg_grad.stops[0].color = ser_color;
     rect_dsc.bg_grad.stops[0].frac = 0;
     rect_dsc.bg_grad.stops[0].opa = (max_opa * (255 - fract_lower)) / 255;
     rect_dsc.bg_grad.stops[1].color = ser_color;
     rect_dsc.bg_grad.stops[1].frac = 255;
     rect_dsc.bg_grad.stops[1].opa = 0;
 
     lv_area_t rect_area;
     rect_area.x1 = (int32_t)draw_line_dsc->p1.x;
     rect_area.x2 = (int32_t)draw_line_dsc->p2.x - 1;
     rect_area.y1 = (int32_t)LV_MAX(draw_line_dsc->p1.y, draw_line_dsc->p2.y) - 1;
     rect_area.y2 = (int32_t)coords.y2;
     lv_draw_rect(base_dsc->layer, &rect_dsc, &rect_area);
 }
 static void draw_event_cb(lv_event_t * e)
 {
     lv_draw_task_t * draw_task = lv_event_get_draw_task(e);
     lv_draw_dsc_base_t * base_dsc = (lv_draw_dsc_base_t *)lv_draw_task_get_draw_dsc(draw_task);
 
     /*Sadece çizgilerin çizimi sırasında gradyent ekle*/
     if(base_dsc->part == LV_PART_ITEMS && lv_draw_task_get_type(draw_task) == LV_DRAW_TASK_TYPE_LINE) {
         add_gradient_area(e);
     }
 }
// UI oluşturma fonksiyonu
void create_ui() {

    lv_obj_t *ui_PortLabel = lv_label_create(ui_Screen1);
    lv_label_set_text(ui_PortLabel, "Seri Port:");
    lv_obj_align(ui_PortLabel, LV_ALIGN_TOP_LEFT, 40, 70);
    
    // Dropdown menü
    lv_dropdown_set_options(ui_Dropdown2, scan_serial_ports());
    lv_obj_add_event_cb(ui_Dropdown2, dropdown_event_cb, LV_EVENT_VALUE_CHANGED, NULL);
    
    // Bağlan butonu
    lv_obj_add_event_cb(ui_Button1, connect_btn_event_cb, LV_EVENT_CLICKED, NULL);
    
    lv_obj_t *ui_ConnectBtnLabel = lv_label_create(ui_Button1);
    lv_label_set_text(ui_ConnectBtnLabel, "Connect");
    lv_obj_center(ui_ConnectBtnLabel);
    
    // Durum etiketi
    ui_StatusLabel = lv_label_create(ui_Panel1);
    lv_obj_set_x(ui_StatusLabel,350);
    lv_obj_set_y(ui_StatusLabel,-15);

    lv_label_set_text(ui_StatusLabel, "Stat: No Connection");
    lv_obj_set_style_bg_color(ui_StatusLabel, lv_color_hex(0x1F1F1F), 0);
    lv_obj_set_style_bg_opa(ui_StatusLabel, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(ui_StatusLabel, 5, 0);
    lv_obj_set_style_pad_all(ui_StatusLabel, 10, 0);
    lv_obj_set_style_text_color(ui_StatusLabel, lv_color_white(), 0);
    lv_scr_load(ui_Screen1);
}
static lv_obj_t * list1;

void lv_example_list_1(void)
 {
     /*Create a list*/
     list1 = lv_list_create(ui_Panel2);
     lv_obj_set_scrollbar_mode(list1, LV_SCROLLBAR_MODE_OFF);
 
     lv_obj_set_size(list1, 225, 150);
     lv_obj_set_x(list1, -25);
     lv_obj_set_y(list1, 10);
     lv_obj_set_style_radius(list1, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
     lv_obj_set_style_bg_opa(list1, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
     lv_obj_set_style_outline_opa(list1, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
     lv_obj_set_style_border_opa(list1, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
     lv_obj_set_style_text_font(list1, &lv_font_montserrat_18, LV_PART_MAIN | LV_STATE_DEFAULT);
 
     /*Add buttons to the list*/
     lv_obj_t * btn;
     btn = lv_list_add_button(list1, NULL, "12V     1A");
     lv_obj_set_size(btn, 200, 49);
     lv_obj_set_style_text_color(btn, lv_color_hex(0x808080), LV_PART_MAIN | LV_STATE_DEFAULT);
     lv_obj_set_style_bg_color(btn, lv_color_hex(0x2B2B2B), LV_PART_MAIN | LV_STATE_DEFAULT);
     lv_obj_set_style_radius(btn, 10, LV_PART_MAIN | LV_STATE_DEFAULT);
     btn = lv_list_add_button(list1, NULL, "5V     1A");
     lv_obj_set_size(btn, 200, 49);
 
     lv_obj_set_style_text_color(btn, lv_color_hex(0x808080), LV_PART_MAIN | LV_STATE_DEFAULT);
     lv_obj_set_style_bg_color(btn, lv_color_hex(0x2B2B2B), LV_PART_MAIN | LV_STATE_DEFAULT);
     lv_obj_set_style_radius(btn, 10, LV_PART_MAIN | LV_STATE_DEFAULT);
 
     btn = lv_list_add_button(list1, NULL, "3V     1A");
     lv_obj_set_size(btn, 200, 49);
 
     lv_obj_set_style_text_color(btn, lv_color_hex(0x808080), LV_PART_MAIN | LV_STATE_DEFAULT);
     lv_obj_set_style_bg_color(btn, lv_color_hex(0x2B2B2B), LV_PART_MAIN | LV_STATE_DEFAULT);
     lv_obj_set_style_radius(btn, 10, LV_PART_MAIN | LV_STATE_DEFAULT);
 
 
 
     
 }


 void lv_example_line_1(void)
{
    static lv_point_precise_t line_points[] = { {380, 20}, {440, 20} };
    static lv_point_precise_t line_points2[] = { {380, 45}, {440, 45} };
    /*Create style*/
    static lv_style_t style_line;
    lv_style_init(&style_line);
    lv_style_set_line_width(&style_line, 3);
    lv_style_set_line_color(&style_line, lv_palette_main(LV_PALETTE_BLUE));
    lv_style_set_line_rounded(&style_line, true);

    /*Create a line and apply the new style*/
    lv_obj_t * line1;
    line1 = lv_line_create(ui_Panel5);
    lv_line_set_points(line1, line_points, 2);     /*Set the points*/
    lv_obj_add_style(line1, &style_line, 0);


    lv_obj_t * labelLine;
    labelLine = lv_label_create(ui_Panel5);
    lv_label_set_text(labelLine,"Current");
    lv_obj_set_x(labelLine,385);
    lv_obj_set_y(labelLine,0);

    static lv_style_t style_line2;
    lv_style_init(&style_line2);
    lv_style_set_line_width(&style_line2, 3);
    lv_style_set_line_color(&style_line2, lv_palette_main(LV_PALETTE_GREEN));
    lv_style_set_line_rounded(&style_line2, true);


    lv_obj_t * line2;
    line2 = lv_line_create(ui_Panel5);
    lv_line_set_points(line2, line_points2, 2);     /*Set the points*/
    lv_obj_add_style(line2, &style_line2, 0);


    lv_obj_t * labelLine2;
    labelLine2 = lv_label_create(ui_Panel5);
    lv_label_set_text(labelLine2,"Voltage");
    lv_obj_set_x(labelLine2,385);
    lv_obj_set_y(labelLine2,25);

}
int main(int argc, char **argv)
{
    configure_simulator(argc, argv);

    /* Initialize LVGL. */
    lv_init();

    /* Initialize the configured backend */
    if (driver_backends_init_backend(selected_backend) == -1) {
        die("Failed to initialize display backend");
    }

    /* Enable for EVDEV support */
#if LV_USE_EVDEV
    if (driver_backends_init_backend("EVDEV") == -1) {
        die("Failed to initialize evdev");
    }
#endif

    /* Initialize UI */
    ui_init();
    lv_obj_t * ui_btnMinus1 = lv_label_create(ui_Button2);          /*Add a label to the button*/
     lv_label_set_text(ui_btnMinus1, LV_SYMBOL_MINUS);                     /*Set the labels text*/
     lv_obj_set_style_text_color(ui_btnMinus1, lv_color_hex(0x808080), LV_PART_MAIN | LV_STATE_DEFAULT);
      
     lv_obj_t * ui_btnMinus2 = lv_label_create(ui_Button4);          /*Add a label to the button*/
     lv_label_set_text(ui_btnMinus2, LV_SYMBOL_MINUS);                     /*Set the labels text*/
     lv_obj_center(ui_btnMinus2);
     lv_obj_set_style_text_color(ui_btnMinus2, lv_color_hex(0x808080), LV_PART_MAIN | LV_STATE_DEFAULT);
     
 
  
     lv_obj_t * ui_btnPlus1 = lv_label_create(ui_Button3);          /*Add a label to the button*/
     lv_label_set_text(ui_btnPlus1, LV_SYMBOL_PLUS);                     /*Set the labels text*/
     lv_obj_center(ui_btnPlus1);
     lv_obj_set_style_text_color(ui_btnPlus1, lv_color_hex(0x808080), LV_PART_MAIN | LV_STATE_DEFAULT);
      
     lv_obj_t * ui_btnPlus2 = lv_label_create(ui_Button5);          /*Add a label to the button*/
     lv_label_set_text(ui_btnPlus2, LV_SYMBOL_PLUS);                     /*Set the labels text*/
     lv_obj_center(ui_btnPlus2);
     lv_obj_set_style_text_color(ui_btnPlus2, lv_color_hex(0x808080), LV_PART_MAIN | LV_STATE_DEFAULT);
     lv_example_list_1();
    /* Create additional UI elements */
    create_ui();
    lv_obj_add_event_cb(ui_Switch1, event_handler, LV_EVENT_ALL, NULL);
    lv_obj_add_event_cb(ui_Button2, button_event_handler, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(ui_Button3, button_event_handler, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(ui_Button4, button_event_handler, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(ui_Button5, button_event_handler, LV_EVENT_CLICKED, NULL);


    lv_example_line_1();
    /* Verify critical UI objects */
    if (ui_Dropdown2 == NULL || ui_Button1 == NULL || ui_StatusLabel == NULL) {
        printf("Critical UI objects not initialized!\n");
        return -1;
    }

    /* Apply gradient effects if charts exist */
    if (ui_Chart1 && ui_Chart2 && ui_Chart3) {
        lv_example_chart_gradient();
    }

    /* Enter the run loop of the selected backend */
    driver_backends_run_loop();

    return 0;
}