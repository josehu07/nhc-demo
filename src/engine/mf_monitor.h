/**
 * The multi-factor caching algorithm monitor.
 *
 * Dynamically monitors and tweaks `data_admit` & `load_admit` switches
 * on the fly.
 */

/*========== [Orthus FLAG BEGIN] ==========*/

#ifndef MF_MONITOR_H_
#define MF_MONITOR_H_


#include <stdbool.h>


bool monitor_query_data_admit();
double monitor_query_load_admit();


#endif /* MF_MONITOR_H_ */

/*========== [Orthus FLAG END] ==========*/
