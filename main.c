#include "stdio.h"
#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"
#include "hardware/i2c.h"
#include "hardware/irq.h"
#include "hardware/pwm.h"
#include "hardware/adc.h"
#include "hardware/timer.h"
#include "lwip/apps/mqtt.h"
#include "lwip/ip_addr.h"
#include "libs/ssd1306.h"

// Definição dos pinos GPIO
#define LED_RED_PIN 13    // Pino do LED vermelho
#define LED_BLUE_PIN 12   // Pino do LED azul
#define LED_GREEN_PIN 11  // Pino do LED verde
#define BUZZER_PIN_A 10   // Pino do buzzer A
#define BUZZER_PIN_B 21   // Pino do buzzer B
#define OLED_SDA_PIN 14   // Pino SDA do display OLED
#define OLED_SCL_PIN 15   // Pino SCL do display OLED
#define PIR_SENSOR_PIN 5  // Pino do PIR (sensor de movimento)
#define MICROPHONE_PIN 28 // Pino do microfone (simulado por um potenciômetro)

// Definição de constantes
#define SOUND_THRESHOLD 2500     // Limite de som para ativar o alarme
#define ALERT_BLINK_TIME 150     // Tempo de piscar do LED em modo ALERTA
#define IDLE_BLINK_TIME 300      // Tempo de piscar do LED em modo INATIVO
#define ALARM_DURATION_MS 10000  // Duração do alarme em milissegundos
#define I2C_PORT i2c1            // Instância I2C para o display
#define WIFI_SSID "teste"        // Nome da rede Wi-Fi
#define WIFI_PASSWORD "teste123" // Senha da rede Wi-Fi

// Variáveis globais de controle
volatile bool is_message_being_sent = false; // Indica se a mensagem está sendo enviada ou não
volatile bool is_alarm_active = false;       // Indica se o alarme está ativado ou não
volatile bool is_system_init = true;         // Indica que o sistema está iniciando

mqtt_client_t *global_mqtt_client = NULL;

// typedef struct mensage // TODO: Resolve struct
// {
//     int id;
//     char status[50];
//     char message[100];
// };

void init_pins_config() // Função para inicializar das configurações dos pinos
{
    // Inicializa LED vermelho
    gpio_init(LED_RED_PIN);
    gpio_set_dir(LED_RED_PIN, GPIO_OUT);

    // Inicializa LED verde
    gpio_init(LED_GREEN_PIN);
    gpio_set_dir(LED_GREEN_PIN, GPIO_OUT);

    // Inicializa sensor de movimento PIR
    gpio_init(PIR_SENSOR_PIN);
    gpio_set_dir(PIR_SENSOR_PIN, GPIO_IN);
    gpio_pull_up(PIR_SENSOR_PIN);

    // Inicializa o microfone (simulado por um potenciômetro)
    adc_init();
    adc_gpio_init(MICROPHONE_PIN);
    adc_select_input(2);

    // Inicializa display OLED SSD1306 via I2C
    i2c_init(I2C_PORT, 400 * 1000);
    gpio_set_function(OLED_SDA_PIN, GPIO_FUNC_I2C);
    gpio_set_function(OLED_SCL_PIN, GPIO_FUNC_I2C);
    gpio_pull_up(OLED_SDA_PIN);
    gpio_pull_up(OLED_SCL_PIN);
    ssd1306_init(I2C_PORT);
    ssd1306_clear();
}

void display_text(uint pos_x, uint pos_y, const char *message)
{
    ssd1306_draw_string(pos_x, pos_y, message, true);
    ssd1306_update(I2C_PORT);
}

void display_clear()
{
    ssd1306_clear();
    ssd1306_update(I2C_PORT);
}

void blink_leds_off(uint pin)
{
    uint slice = pwm_gpio_to_slice_num(pin);
    pwm_set_enabled(slice, false);
    gpio_set_function(pin, GPIO_FUNC_SIO); // Redefine o GPIO como saída
    gpio_put(pin, 0);
}

void blink_leds_on(uint pin, uint frequency, uint brightnessInPercentage) // Faz o led piscar
{
    gpio_set_function(pin, GPIO_FUNC_PWM);
    uint16_t wrap = (uint16_t)(125000000 / frequency) - 1; // Fórmula para calcular o Wrap
    uint slice = pwm_gpio_to_slice_num(pin);
    uint duty_cycle = (wrap * brightnessInPercentage) / 100;
    pwm_set_wrap(slice, wrap);
    pwm_set_gpio_level(pin, duty_cycle);
    pwm_set_clkdiv(slice, 1.0);
    pwm_set_enabled(slice, true);
    sleep_ms(is_alarm_active ? ALERT_BLINK_TIME : IDLE_BLINK_TIME); // Determina que tempo o led deve deve durar
    blink_leds_off(pin);
}

void play_tone(uint pin, float frequency, uint duration_ms) // Faz o som no buzzer usando pwm
{
    gpio_set_function(pin, GPIO_FUNC_PWM);
    uint slice_num = pwm_gpio_to_slice_num(pin);
    uint16_t wrap = (uint16_t)(125000000 / frequency) - 1; // Fórmula para calcular o Wrap
    pwm_set_wrap(slice_num, wrap);
    pwm_set_chan_level(slice_num, pwm_gpio_to_channel(pin), (wrap + 1) / 2);
    pwm_set_enabled(slice_num, true);
    sleep_ms(duration_ms);
    pwm_set_enabled(slice_num, false);     // Desliga o buzzer
    gpio_set_function(pin, GPIO_FUNC_SIO); // Redefine o GPIO como saída
    gpio_put(pin, 0);
}
void play_alarm() // Ativa o alarme
{
    blink_leds_off(LED_GREEN_PIN);
    for (int i = 0; i < 3; i++)
    {
        blink_leds_on(LED_RED_PIN, 10000, 100);
        play_tone(BUZZER_PIN_A, 800.0, 200);
        play_tone(BUZZER_PIN_B, 800.0, 200);
        sleep_ms(100);

        blink_leds_on(LED_RED_PIN, 10000, 100);
        play_tone(BUZZER_PIN_A, 1600.0, 200);
        play_tone(BUZZER_PIN_B, 1600.0, 200);
        sleep_ms(150);
    }
    sleep_ms(250);
    display_clear();
}

void send_message_to_base() // Simula o envio de uma mensagem de alertar para um broker MQTT
{
    // Simulando o tempo atual
    uint32_t current_time_ms = to_ms_since_boot(get_absolute_time());
    uint32_t seconds = current_time_ms / 1000;
    uint32_t minutes = seconds / 60;
    uint32_t hours = minutes / 60;
    uint32_t days = hours / 24;
    uint32_t hour_of_day = hours % 24;
    uint32_t minute_of_hour = minutes % 60;
    uint32_t second_of_minute = seconds % 60;
    is_message_being_sent = true;

    // Simula dados para enviar uma mensagem pelo mqtt
    const char *topic = "Alerta";
    char payload[70];
    sprintf(payload, "Um Alerta foi detectado no dispositivo X no dia %d às %02d:%02d:%02d",
            days, hour_of_day, minute_of_hour, second_of_minute);

    if (global_mqtt_client && mqtt_client_is_connected(global_mqtt_client))
    {
        err_t err = mqtt_publish(global_mqtt_client, topic, payload, strlen(payload), 1, 0, NULL, NULL);
        if (err == ERR_OK)
        {
            printf("Mensagem enviada - Tópico: %s, Messagem: %s\n", topic, payload);
            display_clear();
            display_text(8, 16, "Mensagem Enviada !");
        }
        else
        {
            printf("Erro ao enviar mensagem !");
        }
    }
    is_message_being_sent = false;
}

int64_t on_alarm_timeout_callback() // Desliga o alarme
{
    printf("Alarme desligado !\n\n");
    is_alarm_active = false;
    return 0;
}

void trigger_alarm() // Liga o alarme
{
    printf("Alarme disparado!\n");
    is_alarm_active = true;
    add_alarm_in_ms(ALARM_DURATION_MS, on_alarm_timeout_callback, NULL, false);
}

void gpio_irq_handler(uint gpio, uint32_t events) // Função callback para as interrupções dos GPIOs
{
    if (!is_message_being_sent && !is_alarm_active && !is_system_init)
    {
        if (gpio == PIR_SENSOR_PIN)
        {
            printf("Movimento detectado!\n");
            trigger_alarm();
            send_message_to_base();
            display_clear();
        }
    }
}

bool adc_check_callback(struct repeating_timer *t) // Função callback do temporizador ADC
{
    if (!is_message_being_sent && !is_alarm_active && !is_system_init)
    {
        uint16_t adc_value = adc_read();

        if (adc_value > SOUND_THRESHOLD) // Verifica se o valor ultrapassa o limite de som permitido
        {
            printf("Som alto detectado!\n");
            trigger_alarm();
            send_message_to_base();
            display_clear();
        }
        return true;
    }
}

void init_wifi()
{

    // Initialise the Wi-Fi chip
    if (cyw43_arch_init())
    {
        printf("Wi-Fi init failed\n");
    }

    // Enable wifi station
    cyw43_arch_enable_sta_mode();

    printf("Connecting to Wi-Fi...\n");
    if (cyw43_arch_wifi_connect_timeout_ms(WIFI_SSID, WIFI_PASSWORD, CYW43_AUTH_WPA2_AES_PSK, 30000))
    {
        printf("failed to connect.\n");
    }
    else
    {
        printf("Connected.\n");
        // Read the ip address in a human readable way
        uint8_t *ip_address = (uint8_t *)&(cyw43_state.netif[0].ip_addr.addr);
        printf("IP address %d.%d.%d.%d\n", ip_address[0], ip_address[1], ip_address[2], ip_address[3]);
    }
}

void mqtt_connection_cb(mqtt_client_t *client, void *arg, mqtt_connection_status_t status)
{
    if (status == MQTT_CONNECT_ACCEPTED)
    {
        printf("Conexão MQTT bem-sucedida!\n");
        return;
    }
    else
    {
        printf("Falha na conexão MQTT, Status:%d\n", status);
        printf("Tentando conectar tudo novamente...\n");
        start_mqtt_client();
    }
}

void start_mqtt_client(void)
{
    if (!global_mqtt_client)
    {
        global_mqtt_client = mqtt_client_new();
        if (!global_mqtt_client)
        {
            printf("1 - Falha ao criar cliente MQTT\n");
            return;
        }
    }

    ip_addr_t broker_ip;
    IP4_ADDR(&broker_ip, 192, 168, 100, 121);

    struct mqtt_connect_client_info_t client_info = {
        .client_id = "pico_client", // your client id
        .client_user = NULL,
        .client_pass = NULL,
        .keep_alive = 60,
    };
    mqtt_client_connect(global_mqtt_client, &broker_ip, 1883, mqtt_connection_cb, NULL, &client_info);
    sleep_ms(200);
}

int main()
{
    // Inicialização das principais configurações do sistema
    stdio_init_all();
    init_pins_config();
    init_wifi();         // Simula a inialização do wifi
    start_mqtt_client(); // Simula a inialização do mqtt

    // Configura as interrupções dos GPIOs
    gpio_set_irq_enabled_with_callback(PIR_SENSOR_PIN, GPIO_IRQ_EDGE_RISE, true, gpio_irq_handler);

    // Checa se houve alguma mudança significativa no adc a cada 100ms
    struct repeating_timer timer;
    add_repeating_timer_ms(200, adc_check_callback, NULL, &timer);

    printf("Inicialização concluida com sucesso !\n\n");
    is_system_init = false;

    while (true)
    {
        if (is_alarm_active || is_message_being_sent)
        {
            display_text(8, 16, "Sistema em Alerta!");
            play_alarm();
            sleep_ms(500);
        }
        else
        {
            display_text(8, 0, "Sistema Funcionando!");
            blink_leds_off(LED_RED_PIN);
            blink_leds_on(LED_GREEN_PIN, 10000, 50);
        }
        sleep_ms(200);
    }
    return 0;
}
