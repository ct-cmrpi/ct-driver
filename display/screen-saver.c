#include <cstdio>
#include <X11/Xlib.h>
#include <X11/extensions/dpms.h>
#include <chrono>
#include <thread>
 
using namespace std;
using namespace std::chrono;
 
#include <pigpiod_if2.h>
 
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
 
int main(int argc, char *argv[])
{
    Display *dpy;
    dpy = XOpenDisplay(NULL);
    if (dpy == NULL)
    {
        fprintf(stderr, "%s:  unable to open display\n", argv[0]);
        exit(EXIT_FAILURE);
    }
 
    BOOL onoff;
    CARD16 state;
    while(true)
    {
        DPMSInfo(dpy, &state, &onoff);
        if (onoff)
        {
            switch (state)
            {
                case DPMSModeOn:
                    backlight_on();
                    break;
                case DPMSModeOff:
                    backlight_off();
                    break;
                default:
                    break;
            }
        }
        this_thread::sleep_for(milliseconds(200));
    }
 
    XCloseDisplay(dpy);
 
    return 0;
}