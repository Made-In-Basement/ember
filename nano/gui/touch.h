#ifndef TOUCH_H
#define TOUCH_H

extern int touch_present;

int  touch_open(void);          /* find and wake the pad; 0 if there is one */
void touch_poll(void);          /* from the main loop, often */
void touch_close(void);

#endif
