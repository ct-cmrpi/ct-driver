#include <cstdio>
#include <cstdint>
#include <fstream>
#include <unistd.h>
#include <errno.h>
#include <getopt.h>
#include <sys/file.h>
#include <linux/input.h>
#include <chrono>
#include <thread>
 
#include <pigpiod_if2.h>
 
using namespace std;
using namespace std::chrono;
 
static int instance = -1;
 
#define PIN 12
 
void backlight_on()
{
    auto instance = pigpio_start(NULL, NULL);
 
    set_mode(instance, PIN, PI_OUTPUT);
    gpio_write(instance, PIN, 1);
 
    pigpio_stop(instance);
}
 
void backlight_off()
{
    auto instance = pigpio_start(NULL, NULL);
 
    set_mode(instance, PIN, PI_OUTPUT);
    gpio_write(instance, PIN, 0);
 
    pigpio_stop(instance);
}
 
bool backlight_isOn()
{
    auto instance = pigpio_start(NULL, NULL);
 
    set_mode(instance, PIN, PI_INPUT);
    auto isOn = gpio_read(instance, PIN) == 1;
 
    pigpio_stop(instance);
 
    return isOn;
}
 
int main(int argc, char* argv[])
{
    int secs = 60;
 
    if (argc >= 2)
    {
        secs = stoi(argv[1]);
    }
 
    // Touch events happen on event0
    auto input_fd = open("/dev/input/event0", O_RDONLY | O_NONBLOCK);
 
    struct input_event ev;
    auto lastTouch = high_resolution_clock::now();
    while (true)
    {
        // get the input event
        auto n = read(input_fd, &ev, sizeof(ev));
 
        // If an input event
        if (n != (ssize_t)-1)
        {
            if (ev.type == EV_KEY && ev.value >= 0 && ev.value <= 2)
            {
                if (ev.value == 1)  // If a touch
                {
                    backlight_on();
                    lastTouch = high_resolution_clock::now();
                }
            }
        }
 
        auto now = chrono::high_resolution_clock::now();
        auto sec = duration_cast<seconds>(now - lastTouch).count();
        if (sec > secs)
        {
            backlight_off();
        }
 
        this_thread::sleep_for(milliseconds(10));
    }
 
    return 0;
}