#include <sys/types.h>
#include "include/Clock.h"

#ifndef NULL
#define NULL 0
#endif

// public
Clock g_clock;


// class definition

// cons
Clock::Clock() {

}

double g_now = 0.0;

double Clock::gettime() {
  g_now += .0001;
  return g_now;

  /*
  // get actual time
  gettimeofday(&curtime,NULL);

  return curtime.tv_sec + curtime.tv_usec/1000000.0;
  */
}

void Clock::settime(double t) {
  // do nothing for now.
  curtime.tv_sec = (unsigned long)t;
  curtime.tv_usec = (unsigned long)(t - (double)(curtime.tv_sec * 1000000.0));
}
