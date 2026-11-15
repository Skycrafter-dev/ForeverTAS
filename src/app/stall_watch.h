#ifndef FOREVERTAS_APP_STALL_WATCH_H
#define FOREVERTAS_APP_STALL_WATCH_H

namespace forevertas::app {

// The UI freeze watch: a thread pings the calling (GUI) thread every 25 ms;
// while it does not answer for more than 40 ms, the watch samples where it
// is (its stack, every 10 ms). A freeze over 100 ms goes to the log with its
// length and the functions it spent its time in, so a "it lags when..."
// report comes with its cause. Costs a queued call every 25 ms otherwise.
void StartStallWatch();
void StopStallWatch();

}  // namespace forevertas::app

#endif
