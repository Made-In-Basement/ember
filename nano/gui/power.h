#ifndef POWER_H
#define POWER_H

void power_tick(void);          /* from the main loop */
int  power_known(void);         /* 1 when the controller answers */
int  power_percent(void);       /* 0..100, or -1 */
int  power_on_mains(void);      /* 1, 0, or -1 */
int  power_charging(void);
int  power_cycles(void);

/* the real-time clock, in one call */
struct rtc_time { int hour, min, sec, day, month, year, wday; };
void rtc_read(struct rtc_time *t);

#endif
